"""Continuous material-motion hypotheses using a frozen captured material sample.

Actual local FG calls; synthetic motion/depth/background/tone curve, not a game replay.
The directional fit has oracle separated background access. No candidate is deployed.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import subprocess
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent
FIXTURE_FILE = ROOT / 'material_fixture.py'
spec = importlib.util.spec_from_file_location('selection_fixture', FIXTURE_FILE)
fixture_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture_module)
VARIANTS = ('background', 'alpha_mv', 'alpha_depth', 'edge_alpha_mv',
            'edge_alpha_depth', 'directional_fit_mv', 'directional_fit_depth')


def hard_weights(s, output_size, motion_size, opacity_threshold=.9, edge_width=1.):
    """Object/background selection, with the boundary wholly inside material coverage."""
    w, h = output_size
    rw, rh = motion_size
    rx, ry = max(1, int(np.ceil(edge_width*w/rw))), max(1, int(np.ceil(edge_width*h/rh)))
    cov = s['cov'] & ~s['opaque']
    edge = cov & (cv2.erode(cov.astype(np.uint8), np.ones((2*ry+1, 2*rx+1), np.uint8)) == 0)
    visible = (np.max(np.abs(s['f']), axis=2)>1e-6) | (np.min(s['t'], axis=2)<1-1e-6)
    edge &= visible
    # Every RGB channel must attenuate enough; colored transmission is not scalar alpha.
    high = cov & (np.max(s['t'], axis=2) <= 1-opacity_threshold)
    masks = dict(background=np.zeros_like(cov), opaque_only_mv=high, edge_only_mv=edge,
                 edge_opaque_mv=edge|high, edge_opaque_depth=edge|high)
    return {name: (mask | s['opaque']).astype(np.float32) for name, mask in masks.items()}


def weights(s):
    """No future frames or score regions enter these weights."""
    f, t, b, cov = (s[k] for k in ('f', 't', 'b', 'cov'))
    alpha = np.clip(1 - t @ np.array([.2126, .7152, .0722], np.float32), 0, 1)
    # A 1-output-pixel inner silhouette. With the test's coarse MV input,
    # some thin edges can be missed; the actual changed-input audit records this.
    edge = cov & (cv2.erode(cov.astype(np.uint8), np.ones((3, 3), np.uint8)) == 0)
    visible = (np.max(np.abs(f), axis=2) > 1e-6) | (np.min(t, axis=2) < 1-1e-6)
    edge_alpha = np.where(edge & visible, 1., alpha)
    dx = lambda a: cv2.Sobel(a, cv2.CV_32F, 1, 0, ksize=3, scale=1/8)
    c = np.maximum(f + t*b, 1e-5)
    # Derivative of the fixture's Reinhard/gamma display mapping.
    derivative = (1/2.2) * (c/(1+c))**(1/2.2-1) / (1+c)**2
    gs = (dx(f) + b*dx(t)) * derivative
    gb = t*dx(b) * derivative
    g = gs + gb
    box = lambda a: cv2.boxFilter(a, -1, (3, 3))
    numerator = box(np.sum(g*gs, axis=2))
    denominator = box(np.sum(g*g, axis=2))
    fit = np.clip(numerator / np.maximum(denominator, 1e-10), 0, 1)
    fit = np.where(denominator > 1e-8, fit, alpha)
    values = dict(alpha=alpha, edge_alpha=edge_alpha, directional_fit=fit)
    result = {'background': s['opaque'].astype(np.float32)}
    for name, value in values.items():
        value = np.where(cov, value, 0).astype(np.float32)
        value[s['opaque']] = 1
        result[name+'_mv'] = value
        result[name+'_depth'] = value
    return result


def show_field(weight, mv, title, path):
    # Red is rightward current-to-previous displacement; blue is leftward.
    h, w = weight.shape
    m = cv2.resize(mv[..., 0].astype(np.float32), (w, h), interpolation=cv2.INTER_NEAREST) * w
    color = np.zeros((h, w, 3), np.float32)
    color[..., 0] = np.clip(m/40, 0, 1)
    color[..., 2] = np.clip(-m/40, 0, 1)
    color[..., 1] = np.clip(np.abs(m)/160, 0, .25)
    canvas = Image.new('RGB', (w*2, h+24))
    canvas.paste(Image.fromarray(np.uint8(np.clip(weight, 0, 1)*255)), (0, 24))
    canvas.paste(Image.fromarray(np.uint8(color*255)), (w, 24))
    ImageDraw.Draw(canvas).text((8, 5), title+' : weight / actual uploaded MV (left = blue)', fill='white')
    canvas.save(path)


def main():
    global VARIANTS
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--material', type=Path, required=True)
    p.add_argument('--audit', type=Path, required=True)
    p.add_argument('--template', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--analyze-only', action='store_true')
    p.add_argument('--selector-family', choices=('continuous', 'hard'), default='continuous')
    p.add_argument('--opacity-threshold', type=float, default=.9)
    p.add_argument('--edge-width-mv-pixels', type=float, default=1.)
    args = p.parse_args()
    if not 0 <= args.opacity_threshold <= 1 or not 0 < args.edge_width_mv_pixels <= 4:
        p.error('Opacity must be in [0,1] and inner edge width in (0,4] MV pixels')
    if args.selector_family == 'hard':
        VARIANTS = ('background', 'opaque_only_mv', 'edge_only_mv', 'edge_opaque_mv', 'edge_opaque_depth')
    out = args.output.resolve()
    base = json.loads(args.template.read_text())
    base.pop('overrides', None)
    if base['generatedCount'] != 3 or base['frames'] < 12:
        raise ValueError('This experiment requires at least 12 rendered frames and three generated phases')
    for key in ('provider', 'unlock', 'arguments'):
        base[key] = str((args.template.resolve().parent / base[key]).resolve(strict=True))
    fixture = fixture_module.Fixture(args.material, json.loads(args.audit.read_text()), base)
    w, h, rw, rh = fixture.w, fixture.h, fixture.rw, fixture.rh
    replay = ROOT/'run.py'
    if not args.analyze_only:
        out.mkdir(parents=True, exist_ok=False)
        (out/'provenance.json').write_text(json.dumps(dict(materialHashes=fixture.hashes,
            fixtureSha256=hashlib.sha256(FIXTURE_FILE.read_bytes()).hexdigest(),
            scriptSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            syntheticMotion=True, syntheticDepth=True, syntheticBackground=True,
            syntheticToneCurve=True, separatedBackgroundOracle=True,
            capturedGameSequence=False, glassRefraction=False, gameQualityAccepted=False,
            selectorFamily=args.selector_family, opacityThreshold=args.opacity_threshold,
            edgeWidthMotionPixels=args.edge_width_mv_pixels,
            depthBlend='Linear interpolation of reverse-Z, explicitly a heuristic for independent layer motion'), indent=2))
        for scene in ('stationary', 'relative_translation'):
            target = out/scene
            target.mkdir()
            capture = target/'capture'
            capture.mkdir()
            for variant in VARIANTS:
                (target/variant).mkdir()
            input_rows = []
            for frame in range(fixture.n):
                s = fixture.scene(scene, frame)
                all_weights = (hard_weights(s, (w, h), (rw, rh), args.opacity_threshold, args.edge_width_mv_pixels)
                               if args.selector_family == 'hard' else weights(s))
                rgba = np.concatenate((np.uint8(np.clip(s['c'], 0, 1)*255+.5),
                                       np.full((h, w, 1), 255, np.uint8)), axis=2)
                for slot in (0, 7):
                    rgba.tofile(capture/f'frame-{frame:02d}-index-{slot}.bin')
                baseline_mv = None
                for variant in VARIANTS:
                    weight = all_weights[variant]
                    mv = np.zeros((h, w, 4), np.float32)
                    mv[..., 0] = -(s['vb'] + weight*(s['vs']-s['vb'])) / w
                    dw = weight if variant.endswith('_depth') else s['opaque'].astype(np.float32)
                    d = s['depth'] + dw*(.02/3-s['depth'])
                    if args.selector_family == 'hard':
                        mv[..., 0] = -np.where(weight > 0, s['vs'], s['vb']) / w
                        d = s['depth'].copy()
                        d[dw > 0] = .02/3
                    mv = cv2.resize(mv, (rw, rh), interpolation=cv2.INTER_NEAREST).astype('<f2')
                    d = cv2.resize(d, (rw, rh), interpolation=cv2.INTER_NEAREST).astype('<f4')
                    assert np.isfinite(mv).all() and np.isfinite(d).all()
                    for slot, value in ((4, mv), (5, d)):
                        value.tofile(target/variant/f'frame-{frame:02d}-index-{slot}.bin')
                        if variant == 'background':
                            value.tofile(capture/f'frame-{frame:02d}-index-{slot}.bin')
                    if variant == 'background':
                        baseline_mv = mv.copy()
                    delta = np.abs(mv[..., 0].astype(np.float32)-baseline_mv[..., 0].astype(np.float32))*w
                    changed = delta > 0
                    input_rows.append(dict(frame=frame, variant=variant,
                        changedMVPixels=int(changed.sum()),
                        maxDeltaOutputPixels=float(delta.max()),
                        medianDeltaOutputPixels=float(np.median(delta[changed])) if changed.any() else 0,
                        weightMeanOnCoverage=float(weight[s['cov']].mean()),
                        interpolatedWeights=int(((weight>0)&(weight<1)&s['cov']).sum())))
                    if frame == 8:
                        show_field(weight, mv, variant, target/(variant+'-inputs-08.png'))
                if frame == 8:
                    Image.fromarray(rgba).save(target/'input-08.png')
            (target/'inputs.json').write_text(json.dumps(input_rows, indent=2))
            for variant in VARIANTS:
                config = {**base, 'capture': str(capture), 'overrides': str(target/variant),
                          'output': str(target/(variant+'-fg')),
                          'fixture': 'Continuous material weight; frozen captured F/T, synthetic scene; no game replay'}
                manifest = target/(variant+'.json')
                manifest.write_text(json.dumps(config, indent=2))
                print('FG', scene, variant, flush=True)
                result = subprocess.run([sys.executable, str(replay), str(manifest),
                    '--executable', str(args.executable.resolve())], capture_output=True, text=True)
                (target/(variant+'-runner.log')).write_text(result.stdout+result.stderr)
                result.check_returncode()
    rows = []
    for scene in ('stationary', 'relative_translation'):
        target = out/scene
        for frame in range(4, fixture.n):
            previous, current = fixture.scene(scene, frame-1), fixture.scene(scene, frame)
            for phase in (25, 50, 75):
                truth = fixture.scene(scene, frame-1+phase/100)
                cov = (truth['cov'] | previous['cov'] | current['cov'])
                cov &= ~(truth['opaque'] | previous['opaque'] | current['opaque'])
                footprint = cv2.dilate(cov.astype(np.uint8), np.ones((7, 39), np.uint8)) > 0
                cup = (fixture.y > h*.56) & (fixture.y < h*.76)
                rim = (cv2.dilate(truth['cov'].astype(np.uint8), np.ones((3, 3), np.uint8)) !=
                       cv2.erode(truth['cov'].astype(np.uint8), np.ones((3, 3), np.uint8)))
                masks = dict(cups=footprint&cup, cup_boundary=rim&cup,
                             upper_material=footprint&(fixture.y<h*.45), outside=~footprint)
                if scene == 'stationary':
                    masks['cups_crossed_by_person'] = footprint & cup & (truth['figure'] | previous['figure'] | current['figure'])
                # A moving background must not erase the fixed material effect.
                # This is an oracle diagnostic in a controlled scene, not recovery
                # of real F/T from the FG image or a deployment admission test.
                bare = fixture_module.encode(truth['b'])
                material_effect = truth['c']-bare
                material_energy = np.sum(material_effect**2, axis=2)
                visible_material = cov & cup & (material_energy > 3*(4/255)**2)
                crossed_material = visible_material.copy()
                if scene == 'stationary':
                    crossed_material &= truth['figure'] | previous['figure'] | current['figure']
                panels = [('analytic truth', truth['c'])]
                for variant in VARIANTS:
                    raw = np.fromfile(target/(variant+'-fg')/f'frame-{frame:02d}-generated-{phase:02d}.bin', np.uint8)
                    rgb = raw.reshape(h, w, 4)[..., :3].astype(np.float32)/255
                    error = np.mean(np.abs(rgb-truth['c']), axis=2)*255
                    retained = np.sum((rgb-bare)*material_effect, axis=2) / np.maximum(material_energy, 1e-12)
                    erased = (retained < .25) & crossed_material
                    rows.append(dict(scene=scene, frame=frame, phase=phase, variant=variant,
                        **{k: float(error[m].mean()) if m.any() else None for k, m in masks.items()},
                        material_effect_erased_fraction=float(erased.sum()/max(1, crossed_material.sum())),
                        material_effect_retained_energy=float(np.sum((rgb-bare)[crossed_material]*material_effect[crossed_material]) /
                            max(1e-12, np.sum(material_energy[crossed_material])))))
                    panels.append((variant, rgb))
                if frame == 8 and phase == 50:
                    sheet = Image.new('RGB', (w*2, (h+24)*((len(panels)+1)//2)))
                    draw = ImageDraw.Draw(sheet)
                    for i, (label, rgb) in enumerate(panels):
                        im = Image.fromarray(np.uint8(np.clip(rgb, 0, 1)*255+.5))
                        im.save(target/(label.replace(' ', '-')+'-08-50.png'))
                        ix, iy = i%2*w, i//2*(h+24)
                        sheet.paste(im, (ix, iy+24))
                        draw.text((ix+8, iy+4), label, fill='white')
                    sheet.save(target/'comparison-08-50.png')
    summary = {}
    for scene in ('stationary', 'relative_translation'):
        summary[scene] = {}
        for variant in VARIANTS:
            subset = [r for r in rows if r['scene']==scene and r['variant']==variant]
            keys = [k for k in subset[0] if k not in ('scene', 'frame', 'phase', 'variant')]
            summary[scene][variant] = {k: float(np.mean([r[k] for r in subset if r[k] is not None])) for k in keys}
    (out/'analysis.json').write_text(json.dumps(dict(summary=summary, rows=rows,
        materialHashes=fixture.hashes, actualFG=True, analyticTruth=True, gameQualityAccepted=False), indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
