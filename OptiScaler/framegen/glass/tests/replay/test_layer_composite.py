"""Independent GPU checks of layer composition and rejected correspondence.

Uses only Python's standard library. Does not call FG or access a game.
"""
import argparse
import json
import math
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--shader', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    width, height = 32, 16
    layers = [[] for _ in range(8)]
    phases = [.25, .5, .75]
    expected = {phase: [] for phase in phases}
    fallback = [-2., -3., -4., .375]
    for y in range(height):
        for x in range(width):
            f0, f1 = [.1, .2, .3, 1.], [.3, .4, .5, 1.]
            t0, t1 = [.5, .25, .75, 1.], [.25, .75, .5, 1.]
            bg = [.4, .3, .2, 1.]
            uv = [0., 0., 0., 0.]  # Normalized endpoint offsets from output view UV.
            valid = [1., 1., 0., 0.]
            reject = y < 6
            if y == 0: valid[0] = 0.
            if y == 1: uv[0] = math.nan
            if y == 2: uv[2] = 1.1
            if y == 3: t0[0] = -.1
            if y == 4: t1[2] = 1.1
            if y == 5: f0[0] = math.nan
            if y == 6: f1[:3] = [4., 3., 2.]
            if y == 7: t0[:3] = t1[:3] = [1., 1., 1.]
            if y == 8: t0[:3] = t1[:3] = [0., 0., 0.]
            if y == 9: bg[0] = math.inf; reject = True
            if y in (10, 11):
                f0[:3] = f1[:3] = [0., 0., 0.]
                t0[:3] = t1[:3] = [1., 1., 1.]
                valid[1] = 0.
                reject = True
                if y == 11: bg[0] = math.inf
            if y == 12:
                f0[:3] = f1[:3] = [1e-5, 0., 0.]
                t0[:3] = t1[:3] = [1., 1., 1.]
            if y == 13:
                # A stationary foreground stays fixed while background varies.
                f1 = f0.copy(); t1 = t0.copy()
                bg[:3] = [x/width, .7, (width-x)/width]
            if y == 14:
                f0[:3] = f1[:3] = [0., 0., 0.]
                t0[:3] = t1[:3] = [1., 1., 1.]
            if y == 15: valid[1] = 0.; reject = True
            for array, values in zip(layers, [f0, f1, t0, t1, bg, fallback, uv, valid]):
                array.extend(values)
            for phase in phases:
                expected[phase].extend(fallback if reject else [
                    (1-phase)*f0[c]+phase*f1[c]+((1-phase)*t0[c]+phase*t1[c])*bg[c]
                    for c in range(3)] + [fallback[3]])
    for i, data in enumerate(layers):
        (args.output / f'input-{i}.bin').write_bytes(struct.pack(f'<{len(data)}f', *data))
    failures = 0
    for phase in phases + [0.]:
        output = args.output / f'output-{phase}.bin'
        subprocess.run([str(args.executable.resolve()), str(args.shader.resolve()), str(args.output.resolve()),
                        str(width), str(height), str(phase), str(output.resolve()), str(int(phase != 0))], check=True)
        actual = struct.unpack(f'<{width*height*4}f', output.read_bytes())
        reference = expected[phase] if phase else fallback*(width*height)
        failures += sum(not math.isfinite(a) or abs(a-b) > 2e-6 for a,b in zip(actual, reference))
    report = {'pixels': width*height*4, 'failures': failures,
              'checks': ['no correspondence', 'nonfinite UV', 'out-of-bounds UV', 'invalid transmission',
                         'nonfinite source/background', 'HDR source', 'additive', 'opaque', 'alpha preserved',
                         'three generated phases', 'host admission rejection'],
              'gameAttachment': False, 'qualityAccepted': False}
    report['checks'] += ['outside footprint preserves original FG', 'rejected footprint needs no background',
                         'neutral layers restore background inside footprint', 'faint additive layer retained',
                         'stationary surface over varying background']
    (args.output/'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report))
    return int(failures != 0)


if __name__ == '__main__':
    raise SystemExit(main())
