"""Recorded-input boundary diagnostic; no game attachment or inferred object IDs.

Keeps recorded Backbuffer, HUDless and evaluation packets unchanged. Tests an
auxiliary-depth silhouette under the explicit static-world-motion assumption.
This cannot supply independent object animation or boundaries hidden behind
another transparent layer. No material alpha is available in this recording.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import cv2
import numpy as np
from PIL import Image, ImageDraw


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def packets(path):
    data = path.read_bytes()
    magic, count = struct.unpack_from('<II', data)
    assert magic == 0x31524647
    cursor, frames = 8, []
    for frame in range(count):
        index, entries = struct.unpack_from('<II', data, cursor); cursor += 8
        assert index == frame
        values = {}
        for _ in range(entries):
            slot, result, bits, length, kind, size = struct.unpack_from('<IIQIII', data, cursor); cursor += 28
            key = data[cursor:cursor+length].decode(); cursor += length
            payload = data[cursor:cursor+size]; cursor += size
            if result != 1:
                continue
            if size == 64 and kind == 1:
                values[key] = np.frombuffer(payload, '<f4').reshape(4, 4).astype('f8')
            elif slot == 14:
                values[key] = struct.unpack('<f', struct.pack('<I', bits & 0xffffffff))[0]
            elif slot in (11, 12, 15):
                values[key] = bits
        frames.append(values)
    assert cursor == len(data)
    return frames


def preview(path, frame, color, surface, foreground, masks, original, candidate):
    h, w = surface.shape
    base = cv2.resize(color[..., :3], (w, h), interpolation=cv2.INTER_AREA)
    rows = [('Actual captured DLSSG.HUDLess', base)]
    for radius, mask in masks.items():
        overlay = base.copy()
        overlay[mask] = np.rint(.25*base[mask]+.75*np.array([255, 45, 20])).astype('uint8')
        rows.append((f'Depth silhouette {radius} MV pixels; static-world candidate', overlay))
    # Independent display of the actually changed vector magnitude.
    delta = np.linalg.norm((candidate-original[..., :2].astype('f4'))*[w, h], axis=-1)
    delta = np.uint8(np.clip(delta/60, 0, 1)*255)
    changed = cv2.applyColorMap(delta, cv2.COLORMAP_TURBO)[..., ::-1]
    changed[~masks[4]] = 0
    rows.append(('Applied boundary4 MV change only (0..60 render pixels)', changed))
    size = (w//2, h//2)
    sheet = Image.new('RGB', (size[0]*2, (size[1]+24)*((len(rows)+1)//2)))
    draw = ImageDraw.Draw(sheet)
    for i, (label, image) in enumerate(rows):
        x, y = i%2*size[0], i//2*(size[1]+24)
        sheet.paste(Image.fromarray(image).resize(size, Image.Resampling.LANCZOS), (x, y+24))
        draw.text((x+5, y+5), label, fill='white')
    sheet.save(path/f'frame-{frame:02d}-input-review.png')
    Image.fromarray(color[..., :3]).save(path/f'frame-{frame:02d}-hudless.png')
    Image.fromarray(masks[4].astype('uint8')*255).save(path/f'frame-{frame:02d}-boundary4-mask.png')


def combine(manifest_path, manifest, prepared, base, target):
    """Add confirmed diagnostic bands to an existing captured correction."""
    source = (manifest_path.parent/manifest['capture']).resolve(strict=True)
    w, h = manifest['renderWidth'], manifest['renderHeight']
    target.mkdir(parents=True, exist_ok=False)
    audits = []
    for radius in (2, 4):
        name = f'combined{radius}_depth'
        folder = target/name; folder.mkdir()
        for frame in range(manifest['frames']):
            mname, zname = f'frame-{frame:02d}-index-4.bin', f'frame-{frame:02d}-index-5.bin'
            original_z = np.fromfile(source/zname, '<f4').reshape(h, w)
            selected_z = np.fromfile(prepared/f'boundary{radius}_depth'/zname, '<f4').reshape(h, w)
            mask = selected_z != original_z
            mv = np.fromfile(base/mname, '<f2').reshape(h, w, 4)
            z = np.fromfile(base/zname, '<f4').reshape(h, w)
            selected = np.fromfile(prepared/f'boundary{radius}_depth'/mname, '<f2').reshape(h, w, 4)
            before, before_z = mv.copy(), z.copy()
            mv[mask, :2], z[mask] = selected[mask, :2], selected_z[mask]
            assert np.array_equal(mv[~mask], before[~mask]) and np.array_equal(mv[..., 2:], before[..., 2:])
            assert np.array_equal(z[~mask], before_z[~mask])
            mv.tofile(folder/mname); z.tofile(folder/zname)
            audits.append(dict(variant=name, frame=frame, maskPixels=int(mask.sum()), outsideAdditionalMaskExact=True,
                               mvSha256=digest(folder/mname), depthSha256=digest(folder/zname)))
        config = dict(manifest, capture=str(source), arguments=str((manifest_path.parent/manifest['arguments']).resolve()),
                      overrides=str(folder), output=str(target/(name+'-fg')),
                      fixture='Recorded color/HUDless; captured correction plus diagnostic static-world boundary bands')
        for key in ('provider', 'unlock'):
            config[key] = str((manifest_path.parent/manifest[key]).resolve(strict=True))
        (target/(name+'.json')).write_text(json.dumps(config, indent=2))
    (target/'combined-input-audit.json').write_text(json.dumps(dict(base=str(base), prepared=str(prepared),
        rows=audits, scriptSha256=digest(Path(__file__)), qualityAccepted=False), indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--template', type=Path, required=True, help='Recorded baseline manifest, not a synthetic packet')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--auxiliary-slot', type=int, default=6)
    p.add_argument('--prepared', type=Path, help='Previously prepared boundary directories, for the combination control')
    p.add_argument('--base-overrides', type=Path, help='Captured existing correction to combine with prepared bands')
    args = p.parse_args()
    manifest_path = args.template.resolve(strict=True)
    manifest = json.loads(manifest_path.read_text(encoding='utf-8-sig'))
    if args.prepared or args.base_overrides:
        if not (args.prepared and args.base_overrides):
            p.error('--prepared and --base-overrides are required together')
        combine(manifest_path, manifest, args.prepared.resolve(strict=True),
                args.base_overrides.resolve(strict=True), args.output.resolve())
        return
    source = (manifest_path.parent/manifest['capture']).resolve(strict=True)
    packet = (manifest_path.parent/manifest['arguments']).resolve(strict=True)
    rows = packets(packet)
    w, h = manifest['renderWidth'], manifest['renderHeight']
    cw, ch = manifest['outputWidth'], manifest['outputHeight']
    assert len(rows) == manifest['frames']
    target = args.output.resolve(); target.mkdir(parents=True, exist_ok=False)
    variants = [f'boundary{r}{suffix}' for r in (1, 2, 4) for suffix in ('', '_depth')]
    for name in variants:
        (target/name).mkdir()
    y, x = np.mgrid[:h, :w]
    uv = np.stack(((x+.5)/w, (y+.5)/h), axis=-1)
    previous_surface = None
    audits = []
    for frame, params in enumerate(rows):
        assert params['DLSSG.DepthInverted'] == 1
        assert params['DLSSG.CameraMotionIncluded'] == 1 and params['DLSSG.MvecJittered'] == 0
        assert (params['DLSSG.MVecsSubrectWidth'], params['DLSSG.MVecsSubrectHeight']) == (w, h)
        original = np.fromfile(source/f'frame-{frame:02d}-index-4.bin', '<f2').reshape(h, w, 4)
        depth = np.fromfile(source/f'frame-{frame:02d}-index-5.bin', '<f4').reshape(h, w)
        auxiliary_file = source/f'frame-{frame:02d}-index-{args.auxiliary_slot}.bin'
        surface = np.fromfile(auxiliary_file, '<f4').reshape(h, w)
        assert np.isfinite(original).all() and np.isfinite(depth).all() and np.isfinite(surface).all()
        motion_to_uv = np.array([params['DLSSG.MvecScaleX']/w, params['DLSSG.MvecScaleY']/h])
        assert np.all(np.isfinite(motion_to_uv)) and np.all(motion_to_uv != 0)
        jitter = np.array([params['DLSSG.JitterOffsetX']/w, params['DLSSG.JitterOffsetY']/h])
        unjittered = uv-jitter
        clip = np.concatenate((unjittered*[2, -2]+[-1, 1], surface[..., None], np.ones((h, w, 1))), axis=-1)
        previous = clip @ params['DLSSG.ClipToPrevClip']
        valid = np.isfinite(previous).all(axis=-1) & (previous[..., 3] > 1e-6)
        previous /= np.where(valid, previous[..., 3], 1)[..., None]
        motion_uv = previous[..., :2]*[.5, -.5]+.5-unjittered
        candidate = (motion_uv/motion_to_uv).astype('f4')
        previous_uv = uv+motion_uv
        valid &= np.all((previous_uv > 0) & (previous_uv < 1), axis=-1)
        foreground = (surface > 0) & (surface > depth+np.maximum(depth*.002, 1e-7))
        minimum = cv2.erode(surface, np.ones((3, 3), 'uint8'), borderType=cv2.BORDER_REPLICATE)
        # A depth-mask cut alone is not a geometric outline. Require a jump in
        # the auxiliary surface itself, on its nearer side.
        seeds = foreground & ((surface-minimum) > np.maximum(surface*.02, 1e-7))
        if previous_surface is None or params.get('DLSSG.Reset', 0):
            valid[:] = False
        else:
            px = np.clip((previous_uv[..., 0]*w).astype('int32'), 0, w-1)
            py = np.clip((previous_uv[..., 1]*h).astype('int32'), 0, h-1)
            error = np.full((h, w), np.inf)
            for dx, dy in ((0, 0), (-1, 0), (1, 0), (0, -1), (0, 1)):
                z = previous_surface[np.clip(py+dy, 0, h-1), np.clip(px+dx, 0, w-1)]
                error = np.minimum(error, np.where(z > 0, np.abs(z-previous[..., 2]), np.inf))
            valid &= error <= np.maximum(np.abs(previous[..., 2])*.015, 1e-6)
        # CPU-only diagnostic distance transform; not a proposed production pass.
        distances, labels = cv2.distanceTransformWithLabels((~seeds).astype('uint8'), cv2.DIST_C, 3,
                                                           labelType=cv2.DIST_LABEL_PIXEL)
        seed_depths = np.zeros(int(labels.max())+1, 'f4')
        seed_depths[labels[seeds]] = surface[seeds]
        continuity = np.abs(surface-seed_depths[labels]) <= np.maximum(surface*.015, 1e-7)
        masks = {r: foreground & valid & continuity & (distances < r) for r in (1, 2, 4)}
        audit = dict(frame=frame, auxiliarySha256=digest(auxiliary_file), foregroundPixels=int(foreground.sum()),
                     geometricSeedPixels=int(seeds.sum()), masks={}, colorInputs={})
        for slot, name in ((0, 'DLSSG.Backbuffer'), (7, 'DLSSG.HUDLess')):
            file = source/f'frame-{frame:02d}-index-{slot}.bin'
            assert file.stat().st_size == cw*ch*4
            audit['colorInputs'][name] = dict(path=str(file), sha256=digest(file), unchanged=True)
        for radius, mask in masks.items():
            changed = original.copy(); changed[mask, :2] = candidate[mask].astype('<f2')
            assert np.array_equal(changed[~mask], original[~mask])
            assert np.array_equal(changed[..., 2:], original[..., 2:])
            z = depth.copy(); z[mask] = surface[mask]
            for suffix in ('', '_depth'):
                output = target/f'boundary{radius}{suffix}'
                changed.tofile(output/f'frame-{frame:02d}-index-4.bin')
                (z if suffix else depth).tofile(output/f'frame-{frame:02d}-index-5.bin')
            delta = np.linalg.norm((changed[..., :2].astype('f4')-original[..., :2].astype('f4'))*motion_to_uv*[w, h], axis=-1)
            audit['masks'][str(radius)] = dict(selectedPixels=int(mask.sum()), changedPixels=int((delta > 0).sum()),
                     outsideExact=True, zwExact=True, medianChangeRenderPixels=float(np.median(delta[mask])) if mask.any() else 0)
        if frame in (4, 12, 21, 28):
            color = np.fromfile(source/f'frame-{frame:02d}-index-7.bin', 'uint8').reshape(ch, cw, 4)
            preview(target, frame, color, surface, foreground, masks, original, candidate)
        previous_surface = surface
        audits.append(audit)
        print('RECORDED_INPUTS', frame, {r:audit['masks'][str(r)]['selectedPixels'] for r in masks}, flush=True)
    for name in variants:
        config = dict(manifest, capture=str(source), arguments=str(packet), overrides=str(target/name),
                      output=str(target/(name+'-fg')), fixture='Real recorded Cyberpunk Backbuffer/HUDless; auxiliary-depth boundary and static-world MV diagnostic; no object history or alpha')
        for key in ('provider', 'unlock'):
            config[key] = str((manifest_path.parent/manifest[key]).resolve(strict=True))
        (target/(name+'.json')).write_text(json.dumps(config, indent=2))
    report = dict(frames=audits, scriptSha256=digest(Path(__file__)), packetSha256=digest(packet),
                  recordedColor=True, exactObjectMotion=False, perObjectMask=False, materialAlphaAvailable=False,
                  boundaryUnits='1/2/4 render pixels; output radius scales by output/render extent',
                  source=str(source), qualityAccepted=False, productionCostMeasured=False)
    (target/'input-audit.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
