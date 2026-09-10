"""Inspect unchanged recorded HUDless and actual generated frames side by side.

No intermediate-time game ground truth exists in this recording. Differences
below measure sensitivity, not improvement; endpoint images are labeled as such.
"""
import argparse
import json
from pathlib import Path
import cv2
import numpy as np
from PIL import Image, ImageDraw


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifests', nargs='+', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--roi', nargs=4, type=int, required=True, metavar=('LEFT', 'TOP', 'RIGHT', 'BOTTOM'))
    p.add_argument('--roi-map', type=Path, help='Optional per-frame JSON rectangles for moving screen locations')
    p.add_argument('--video-variant', help='Manifest stem to compare with the first manifest in a slow whole-frame video')
    p.add_argument('--frames', nargs='+', type=int, default=[4, 12, 21, 28])
    args = p.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    configs = [json.loads(path.read_text(encoding='utf-8-sig')) for path in args.manifests]
    w, h = configs[0]['outputWidth'], configs[0]['outputHeight']
    left, top, right, bottom = args.roi
    assert 0 <= left < right <= w and 0 <= top < bottom <= h
    width, height = right-left, bottom-top
    rectangles = json.loads(args.roi_map.read_text()) if args.roi_map else {}
    roots, labels = [], []
    reports = []
    for path, config in zip(args.manifests, configs):
        assert (config['outputWidth'], config['outputHeight']) == (w, h)
        root = (path.parent/config['output']).resolve(strict=True)
        report = json.loads(root.with_name(root.name+'.report.json').read_text())
        assert report['complete'] and report['evaluations'] == config['frames']*config['generatedCount']
        assert all(not item['changed'] for item in report['inputs'] if item['slot'] in (0, 7))
        reports.append(report)
        roots.append(root); labels.append(path.stem)
    color_identity = [[(row['frame'], row['slot'], row['selectedSha256']) for row in report['inputs']
                       if row['slot'] in (0, 7)] for report in reports]
    assert all(identity == color_identity[0] for identity in color_identity)
    capture = (args.manifests[0].parent/configs[0]['capture']).resolve(strict=True)

    def read(file):
        return np.fromfile(file, 'uint8').reshape(h, w, 4)[..., :3]

    def sheet(panels, destination, crop=True):
        pw, ph = (width, height) if crop else (1280, round(h*1280/w))
        image = Image.new('RGB', (pw*2, (ph+24)*((len(panels)+1)//2)))
        draw = ImageDraw.Draw(image)
        for i, (label, rgb) in enumerate(panels):
            if crop:
                rgb = rgb[top:bottom, left:right]
            else:
                rgb = cv2.resize(rgb, (pw, ph), interpolation=cv2.INTER_AREA)
            x, y = i%2*pw, i//2*(ph+24)
            image.paste(Image.fromarray(rgb), (x, y+24))
            draw.text((x+5, y+5), label, fill='white')
        image.save(destination)

    comparisons = []
    for frame in args.frames:
        assert 0 < frame < configs[0]['frames']
        left, top, right, bottom = rectangles.get(str(frame), args.roi)
        assert 0 <= left < right <= w and 0 <= top < bottom <= h
        width, height = right-left, bottom-top
        current = read(capture/f'frame-{frame:02d}-index-7.bin')
        previous = read(capture/f'frame-{frame-1:02d}-index-7.bin')
        panels = [('Previous actual HUDless endpoint (not phase truth)', previous),
                  ('Current actual HUDless endpoint (not phase truth)', current)]
        generated = [read(root/f'frame-{frame:02d}-generated-50.bin') for root in roots]
        panels += [(label+' / actual FG 50%', rgb) for label, rgb in zip(labels, generated)]
        sheet(panels, out/f'frame-{frame:02d}-detail.png')
        if frame == 12:
            # Full frames stay part of review, separate from close-up evidence.
            chosen = [0, 1, 2, 3, len(panels)-1]
            sheet([panels[i] for i in chosen], out/'frame-12-full.png', crop=False)
            Image.fromarray(current).save(out/'frame-12-actual-hudless.png')
        baseline = generated[0].astype('int16')
        for label, rgb in zip(labels[1:], generated[1:]):
            delta = np.abs(rgb.astype('int16')-baseline)
            comparisons.append(dict(frame=frame, variant=label,
                changedOutputPixels=int(np.any(delta != 0, axis=-1).sum()),
                roi=[left, top, right, bottom], roiDifference=float(delta[top:bottom, left:right].mean()),
                differenceIsNotQualityScore=True))
        for phase in (25, 75):
            phase_panels = [(label+f' / actual FG {phase}%', read(root/f'frame-{frame:02d}-generated-{phase:02d}.bin'))
                            for label, root in zip(labels, roots)]
            sheet(phase_panels, out/f'frame-{frame:02d}-phase-{phase}.png')
    report = dict(sameBackbufferAndHudlessHashes=True, actualFG=True, groundTruthAvailable=False,
                  qualityAccepted=False, roi=args.roi, perFrameRectangles=rectangles, frames=args.frames, comparisons=comparisons,
                  providerHashes=[r['identities']['provider']['sha256'] for r in reports],
                  inputReports=[str(root.with_name(root.name+'.report.json')) for root in roots])
    (out/'review.json').write_text(json.dumps(report, indent=2))
    if args.video_variant:
        selected = labels.index(args.video_variant)
        thumb = (1280, round(h*1280/w))
        video = cv2.VideoWriter(str(out/'recorded-4x-comparison-slow.mp4'), cv2.VideoWriter_fourcc(*'mp4v'),
                                15, (thumb[0]*2, thumb[1]+32))
        if not video.isOpened():
            raise RuntimeError('Video encoder unavailable')
        count = 0
        try:
            for frame in range(configs[0]['frames']):
                phases = (0,) if frame == 0 else (25, 50, 75, 0)
                for phase in phases:
                    images = ([read(capture/f'frame-{frame:02d}-index-0.bin')]*2 if not phase else
                              [read(roots[i]/f'frame-{frame:02d}-generated-{phase:02d}.bin') for i in (0, selected)])
                    canvas = np.zeros((thumb[1]+32, thumb[0]*2, 3), 'uint8')
                    for column, (i, rgb) in enumerate(zip((0, selected), images)):
                        canvas[32:, column*thumb[0]:(column+1)*thumb[0]] = cv2.resize(rgb, thumb, interpolation=cv2.INTER_AREA)
                        label = f'{labels[i]} | frame {frame:02d} | ' + (f'FG {phase}%' if phase else 'rendered endpoint')
                        cv2.putText(canvas, label, (column*thumb[0]+8, 23), cv2.FONT_HERSHEY_SIMPLEX, .58, (255, 255, 255), 1)
                    video.write(cv2.cvtColor(canvas, cv2.COLOR_RGB2BGR)); count += 1
        finally:
            video.release()
        report['video'] = dict(frames=count, fps=15, downsampled=True, lossyPreview=True,
                               originals='Unmodified full-resolution generated .bin files remain authoritative')
        (out/'review.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(dict(output=str(out), sameCapturedColor=True, generatedFrames=sum(r['evaluations'] for r in reports))))


if __name__ == '__main__':
    main()
