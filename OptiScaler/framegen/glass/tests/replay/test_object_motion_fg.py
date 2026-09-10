"""Run actual FG on per-object 3D motion with an inner boundary-width sweep.

Optional NumPy/OpenCV/Pillow experiment. Synthetic geometry/color only. Caller
supplies the local FG provider, unlock, packet template and raster/replay tools.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
import cv2
import numpy as np
from PIL import Image, ImageDraw
import object_motion_fixture as fixture

ROOT = Path(__file__).resolve().parent
VARIANTS = ['background', 'alpha'] + [f'edge{r}{suffix}' for r in (1, 2, 4) for suffix in ('', '_depth')]
BOUNDARY_ONLY = [f'boundary{r}{suffix}' for r in (1, 2, 4) for suffix in ('', '_depth')]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def rgba(color):
    rgb = np.uint8(np.rint(np.clip(color, 0, 1)*255))
    return np.concatenate((rgb, np.full((*rgb.shape[:2], 1), 255, 'uint8')), axis=-1)


def invoke(command, log):
    with log.open('xb') as stream:
        result = subprocess.run([str(x) for x in command], stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {log}')


def prepare(args):
    target = args.output.resolve()
    target.mkdir(parents=True, exist_ok=False)
    template_path = args.template.resolve(strict=True)
    template = json.loads(template_path.read_text(encoding='utf-8-sig'))
    w, h, n = args.width, args.height, 12
    template_packet = (template_path.parent/template['arguments']).resolve(strict=True)
    assert template['generatedCount'] == 3 and template['frames'] >= n
    for key in ('provider', 'unlock'):
        template[key] = str((template_path.parent/template[key]).resolve(strict=True))
    for key in ('overrides', 'uiAlpha', 'userInterfaceRecomposition'):
        template.pop(key, None)
    for scene_name in ('stationary', 'camera_object'):
        scene_dir = target/scene_name
        scene_dir.mkdir()
        packet = scene_dir/'arguments.bin'
        fixture.write_packets(template_packet, packet, scene_name, n, w, h, w, h)
        inputs = scene_dir/'input'; inputs.mkdir()
        for variant in VARIANTS[1:]:
            (scene_dir/variant).mkdir()
        audits = []
        for frame in range(n):
            frame_dir = scene_dir/f'geometry-{frame:02d}'; frame_dir.mkdir()
            objects = fixture.objects_at(scene_name, frame, w, h)
            fixture.geometry.write_fixture(frame_dir/'input', objects, w, h)
            invoke([args.geometry.resolve(), ROOT.parent/'ObjectMotion.hlsl', frame_dir/'input', frame_dir/'gpu'],
                   frame_dir/'raster.log')
            layers = [{name: np.fromfile(frame_dir/'gpu'/f'{name}-{i}.bin', '<f4').reshape(h, w, 4)
                       for name in ('motion', 'coverage')} for i in range(5)]
            scene = fixture.compose(layers)
            analytic = fixture.analytic_layers(objects, w, h)
            errors = []
            for observed, reference in zip(layers, analytic):
                same = (observed['coverage'][..., 0] != 0) & (reference['coverage'][..., 0] != 0)
                error = np.linalg.norm((observed['motion'][..., :2]-reference['motion'][..., :2])*[w, h], axis=-1)
                errors.append(float(error[same].max()))
            assert max(errors) < .005, errors
            cpu_color = rgba(fixture.compose(analytic)['color'])
            image = rgba(scene['color'])
            input_delta = np.abs(cpu_color[..., :3].astype('f4')-image[..., :3])
            assert float(input_delta.mean()) < .1
            image.tofile(inputs/f'frame-{frame:02d}-index-0.bin')
            image.tofile(inputs/f'frame-{frame:02d}-index-7.bin')
            base = np.zeros((h, w, 4), '<f2'); base[..., :2] = scene['mv']
            base.tofile(inputs/f'frame-{frame:02d}-index-4.bin')
            scene['depth'].astype('<f4').tofile(inputs/f'frame-{frame:02d}-index-5.bin')
            variants = {}
            for variant in VARIANTS[1:]:
                radius = int(variant[4]) if variant.startswith('edge') else 0
                half, depth, audit = fixture.select(layers, scene, radius, variant.endswith('_depth'))
                mv = np.zeros_like(base); mv[..., :2] = half
                mv.tofile(scene_dir/variant/f'frame-{frame:02d}-index-4.bin')
                depth.astype('<f4').tofile(scene_dir/variant/f'frame-{frame:02d}-index-5.bin')
                audit['mvSha256'] = digest(scene_dir/variant/f'frame-{frame:02d}-index-4.bin')
                variants[variant] = audit
                if frame == 8 and variant in ('alpha', 'edge1', 'edge2', 'edge4'):
                    Image.fromarray(fixture.geometry.field(half, w, h)).save(scene_dir/f'{variant}-input-mv.png')
                    delta = np.linalg.norm((half.astype('f4')-base[..., :2].astype('f4'))*[w, h], axis=-1)
                    Image.fromarray(np.uint8(np.clip(delta/8, 0, 1)*255)).save(scene_dir/f'{variant}-mv-delta.png')
            if frame == 8:
                Image.fromarray(image[..., :3]).save(scene_dir/'rendered-08.png')
                Image.fromarray(fixture.geometry.field(base[..., :2], w, h)).save(scene_dir/'background-input-mv.png')
            audits.append(dict(frame=frame, gpuMotionErrorPixels=errors, endpointColorMAE=float(input_delta.mean()),
                               variants=variants))
            print('PREPARED', scene_name, frame, flush=True)
        (scene_dir/'input-audit.json').write_text(json.dumps(audits, indent=2))
        for variant in VARIANTS:
            config = dict(template, capture=str(inputs), arguments=str(packet), output=str(scene_dir/(variant+'-fg')),
                          outputWidth=w, outputHeight=h, renderWidth=w, renderHeight=h, frames=n,
                          fixture='Synthetic 3D camera/object translation/rotation; scalar alpha, no refraction/SR/RR')
            if variant != 'background':
                config['overrides'] = str(scene_dir/variant)
            (scene_dir/(variant+'.json')).write_text(json.dumps(config, indent=2))
    config = dict(width=w, height=h, frames=n, variants=VARIANTS, boundaryWidthUnits='MV pixels, equal to output pixels',
                  geometryExecutable=str(args.geometry.resolve()), geometrySha256=digest(args.geometry),
                  replayExecutable=str(args.executable.resolve()), replaySha256=digest(args.executable),
                  fixtureSha256=digest(ROOT/'object_motion_fixture.py'), runnerSha256=digest(Path(__file__)),
                  shaderSha256=digest(ROOT.parent/'ObjectMotion.hlsl'), templateSha256=digest(template_path),
                  gameCapture=False, exactEngineObjectInputsAcquired=False, gameQualityAccepted=False,
                  packetABI='Observed native provider keys; explicit 58 degree FOV, .1/100 forward-Z, row-vector matrices')
    (target/'config.json').write_text(json.dumps(config, indent=2))


def extend(args):
    """Add edge-only ablations without regenerating or overwriting the first inputs."""
    config = json.loads((args.output/'config.json').read_text())
    w, h = config['width'], config['height']
    variants = args.variants or BOUNDARY_ONLY
    if any(v not in BOUNDARY_ONLY or v in config['variants'] for v in variants):
        raise ValueError('Extension accepts only new boundary-only variants')
    for scene_name in ('stationary', 'camera_object'):
        path = args.output/scene_name
        audits = []
        for variant in variants:
            (path/variant).mkdir()
        for frame in range(config['frames']):
            layers = [{name: np.fromfile(path/f'geometry-{frame:02d}'/'gpu'/f'{name}-{i}.bin', '<f4').reshape(h, w, 4)
                       for name in ('motion', 'coverage')} for i in range(5)]
            scene = fixture.compose(layers)
            selected = {}
            for variant in variants:
                radius = int(variant.removeprefix('boundary')[0])
                half, depth, audit = fixture.select(layers, scene, radius, variant.endswith('_depth'), interior_gain=0)
                mv = np.zeros((h, w, 4), '<f2'); mv[..., :2] = half
                mv.tofile(path/variant/f'frame-{frame:02d}-index-4.bin')
                depth.astype('<f4').tofile(path/variant/f'frame-{frame:02d}-index-5.bin')
                audit['mvSha256'] = digest(path/variant/f'frame-{frame:02d}-index-4.bin')
                selected[variant] = audit
                if frame == 8 and not variant.endswith('_depth'):
                    Image.fromarray(fixture.geometry.field(half, w, h)).save(path/(variant+'-input-mv.png'))
            audits.append(dict(frame=frame, variants=selected))
        (path/'boundary-only-input-audit.json').write_text(json.dumps(audits, indent=2))
        for variant in variants:
            manifest = json.loads((path/'background.json').read_text())
            manifest.update(output=str(path/(variant+'-fg')), overrides=str(path/variant))
            (path/(variant+'.json')).write_text(json.dumps(manifest, indent=2))
        print('EXTENDED', scene_name, flush=True)
    config['variants'].extend(variants)
    config.setdefault('extensions', []).append(dict(variants=variants, fixtureSha256=digest(ROOT/'object_motion_fixture.py'),
                                                    runnerSha256=digest(Path(__file__))))
    (args.output/'config.json').write_text(json.dumps(config, indent=2))


def run(args):
    config = json.loads((args.output/'config.json').read_text())
    assert digest(args.executable) == config['replaySha256']
    for name in ('stationary', 'camera_object'):
        scene_dir = args.output/name
        for variant in args.variants or config['variants']:
            manifest = scene_dir/(variant+'.json')
            invoke([sys.executable, ROOT/'run.py', manifest, '--executable', args.executable.resolve()],
                   scene_dir/(variant+'-validation.log'))
            print('FG_COMPLETE', name, variant, flush=True)


def mean_error(image, truth, mask):
    if not mask.any():
        return None
    return float(np.abs(image[mask].astype('f4')-truth[mask]).mean())


def analyze(args):
    config = json.loads((args.output/'config.json').read_text())
    w, h = config['width'], config['height']
    report = dict(actualFG=True, gameCapture=False, qualityAccepted=False, scenes={},
                  analyzerSha256=digest(Path(__file__)), fixtureSha256=digest(ROOT/'object_motion_fixture.py'))
    for scene_name in ('stationary', 'camera_object'):
        path = args.output/scene_name
        variants = args.variants or config['variants']
        for variant in variants:
            native = json.loads((path/(variant+'-fg.report.json')).read_text())
            assert native['complete'] and native['evaluations'] == config['frames']*3
        outputs = {v: [] for v in variants}
        phase_errors = []
        for frame in range(4, config['frames']):
            for phase in (.25, .5, .75):
                time = frame-1+phase
                objects = fixture.objects_at(scene_name, time, w, h)
                truth = fixture.compose(fixture.analytic_layers(objects, w, h))
                expected = rgba(truth['color'])[..., :3]
                zones = {}
                for i, label in ((2, 'pane'), (3, 'object')):
                    mask = truth['masks'][i]
                    inner = fixture.inner_edge(truth['raw_masks'][i], 2) & mask
                    band = cv2.dilate(inner.astype('uint8'), np.ones((13, 13), 'uint8')) != 0
                    zones[label+'_edge_band'] = band
                    zones[label+'_interior'] = mask & ~cv2.dilate(inner.astype('uint8'), np.ones((21, 21), 'uint8')).astype(bool)
                zones['transmitted_figure'] = truth['figure'] & (truth['masks'][2] | truth['masks'][3])
                zones['outside'] = ~(truth['masks'][2] | truth['masks'][3])
                samples = {}
                for variant in variants:
                    file = path/(variant+'-fg')/f'frame-{frame:02d}-generated-{int(phase*100):02d}.bin'
                    actual = np.fromfile(file, 'uint8').reshape(h, w, 4)[..., :3]
                    samples[variant] = actual
                    row = dict(frame=frame, phase=phase, wholeMAE=mean_error(actual, expected, np.ones((h, w), bool)))
                    row.update({zone: mean_error(actual, expected, mask) for zone, mask in zones.items()})
                    # Known scalar-alpha decomposition is an oracle diagnostic;
                    # an error here can include both misplaced surface and background.
                    t = truth['transmission']
                    valid = zones['transmitted_figure'] & (t > .3)
                    recovered = (actual.astype('f4')/255-truth['source'])/np.maximum(t[..., None], .001)
                    row['transmitted_reconstruction_error'] = float(np.abs(recovered[valid]-truth['background'][valid]).mean()*255)
                    if variant == 'background':
                        control = (actual[..., 0] > 200) & (actual[..., 1] < 35) & (actual[..., 2] > 180)
                        assert control.any() and truth['control'].any()
                        cy, cx = np.where(control); ty, tx = np.where(truth['control'])
                        error = float(np.linalg.norm([cx.mean()-tx.mean(), cy.mean()-ty.mean()]))
                        phase_errors.append(error)
                        row['opaqueControlCentroidErrorPixels'] = error
                    outputs[variant].append(row)
                if frame == 8 and phase == .5:
                    Image.fromarray(expected).save(path/'truth-08-50.png')
                    for variant, rgb in samples.items():
                        Image.fromarray(rgb).save(path/(variant+'-08-50.png'))
                    selection = ['background', 'alpha', 'edge1', 'edge2', 'edge4', 'edge4_depth',
                                 'boundary1', 'boundary2', 'boundary4', 'boundary4_depth']
                    panels = [('Analytic 3D truth', expected)] + [(v+' / actual DLSS FG', samples[v]) for v in selection if v in samples]
                    scale = .5
                    panel_w, panel_h = int(w*scale), int(h*scale)
                    sheet = Image.new('RGB', (panel_w*2, (panel_h+24)*((len(panels)+1)//2)))
                    draw = ImageDraw.Draw(sheet)
                    for i, (label, rgb) in enumerate(panels):
                        x, y = i%2*panel_w, i//2*(panel_h+24)
                        sheet.paste(Image.fromarray(rgb).resize((panel_w, panel_h), Image.Resampling.LANCZOS), (x, y+24))
                        draw.text((x+5, y+5), label, fill='white')
                    sheet.save(path/'fg-comparison.png')
        summary = {v: {key: float(np.mean([r[key] for r in rows if r[key] is not None]))
                       for key in rows[0] if key not in ('frame', 'phase')} for v, rows in outputs.items()}
        report['scenes'][scene_name] = dict(summary=summary, samples=outputs,
                    opaqueControlMeanPixels=float(np.mean(phase_errors)), opaqueControlMaxPixels=float(np.max(phase_errors)))
        print(scene_name, json.dumps(summary), flush=True)
    (args.output/'analysis.json').write_text(json.dumps(report, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=('prepare', 'extend', 'run', 'analyze', 'all'), default='all')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--template', type=Path)
    parser.add_argument('--geometry', type=Path)
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--variants', nargs='+', choices=VARIANTS+BOUNDARY_ONLY)
    args = parser.parse_args()
    args.output = args.output.resolve()
    if args.stage == 'extend':
        extend(args)
    if args.stage in ('prepare', 'all'):
        if not all((args.template, args.geometry, args.executable)):
            parser.error('Preparation requires template, geometry and replay executables')
        if min(args.width, args.height) < 128 or max(args.width, args.height) > 1280:
            parser.error('Fixture dimensions must be 128..1280')
        prepare(args)
    if args.stage in ('run', 'all'):
        if not args.executable:
            parser.error('Run requires --executable')
        run(args)
    if args.stage in ('analyze', 'all'):
        analyze(args)


if __name__ == '__main__':
    main()
