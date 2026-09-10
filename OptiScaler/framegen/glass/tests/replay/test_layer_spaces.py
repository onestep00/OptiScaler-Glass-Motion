"""Check independent layer sizes, valid subrectangles and normalized motion on a GPU.

Uses analytic linear fields as an independent reference, Python standard library
only, an independent D3D12 device, and no game or FG provider.
"""
import argparse
from array import array
import json
import math
from pathlib import Path
import struct
import subprocess
import sys


def field(index, u, v):
    if index == 0:
        return [0.1 + 0.8*u, 0.2 + 0.3*v, 0.1 + 0.4*u + 0.2*v, 1.]
    if index == 1:
        return [1.2 + 0.4*v, 0.3 + 0.7*u, 0.2 + 0.6*v, 1.]
    if index == 2:
        return [0.3 + 0.2*v, 0.4 + 0.3*u, 0.6 - 0.2*v, 1.]
    if index == 3:
        return [0.6 - 0.2*u, 0.2 + 0.1*v, 0.3 + 0.2*u, 1.]
    if index == 4:
        return [0.2 + 0.6*v, 0.1 + 0.8*u, 0.5 - 0.3*u, 1.]
    if index == 5:
        return [-2. - u, -3. - v, -4. + u, 0.25 + 0.125*v]
    raise ValueError(index)


def clamp_center(uv, size):
    return tuple(max(0.5/n, min(1. - 0.5/n, p)) for p, n in zip(uv, size))


def sample_field(index, uv, sizes):
    # Bilinear filtering reproduces an affine field exactly inside its sampled
    # extent. At borders, the first/last valid texel center bounds the field.
    return field(index, *clamp_center(uv, sizes[index]))


def endpoint_offsets(u, v, stationary):
    if stationary:
        return [0., 0., 0., 0.]
    return [0.023 + 0.004*v, -0.018 + 0.003*u,
            -0.016 + 0.002*v, 0.021 - 0.003*u]


def mask(x, y):
    return [float((x + 2*y) % 7 != 0), float((2*x + y) % 5 != 0), 0., 0.]


def pack(values):
    data = array('f', values)
    if sys.byteorder != 'little':
        data.byteswap()
    return data.tobytes()


def run_case(args, name, output_size, sizes, stationary=False):
    folder = args.output / name
    folder.mkdir()
    width, height = output_size
    layouts = []
    fallback_bytes = bytearray()
    # Distinct origins, allocation sizes, extents and conspicuous padding catch
    # sampling in allocation coordinates or clamping to the allocation border.
    for index, (w, h) in enumerate(sizes):
        ox, oy = 1 + index % 3, 2 + index % 2
        tw, th = w + ox + 3, h + oy + 2
        layouts.append([tw, th, ox, oy, w, h])
        data = array('f')
        for y in range(th):
            for x in range(tw):
                if not (ox <= x < ox + w and oy <= y < oy + h):
                    value = [1000., 2000., 3000., -10.]
                else:
                    lx, ly = x - ox, y - oy
                    u, v = (lx + .5)/w, (ly + .5)/h
                    value = (field(index, u, v) if index < 6 else
                             endpoint_offsets(u, v, stationary) if index == 6 else mask(lx, ly))
                    if index == 5:
                        fallback_bytes.extend(pack(value))
                data.extend(value)
        (folder / f'input-{index}.bin').write_bytes(pack(data))
    layout_path = folder / 'input-layout.txt'
    layout_text = '\n'.join(' '.join(map(str, row)) for row in layouts) + '\n'
    layout_path.write_text(layout_text, encoding='utf-8')
    phases = [0.25, 0.5, 0.75]
    case_report = {'name': name, 'outputSize': output_size, 'layouts': layouts, 'phases': []}
    for phase in phases:
        target = folder / f'output-{phase}.bin'
        subprocess.run([str(args.executable.resolve()), str(args.shader.resolve()), str(folder.resolve()),
                        str(width), str(height), str(phase), str(target.resolve())], check=True)
        raw = target.read_bytes()
        assert len(raw) == width*height*16, 'Incomplete GPU output'
        actual = struct.unpack(f'<{width*height*4}f', raw)
        preserved, composed, failures, max_error = 0, 0, 0, 0.
        for y in range(height):
            for x in range(width):
                pixel = y*width + x
                uv = ((x + .5)/width, (y + .5)/height)
                admission = mask(min(int(uv[0]*sizes[7][0]), sizes[7][0] - 1),
                                 min(int(uv[1]*sizes[7][1]), sizes[7][1] - 1))
                offsets = endpoint_offsets(*clamp_center(uv, sizes[6]), stationary)
                previous = (uv[0] + offsets[0], uv[1] + offsets[1])
                current = (uv[0] + offsets[2], uv[1] + offsets[3])
                reject = admission[:2] != [1., 1.] or any(p < 0 or p > 1 for p in previous + current)
                if reject:
                    preserved += 1
                    failures += raw[pixel*16:(pixel+1)*16] != fallback_bytes[pixel*16:(pixel+1)*16]
                    continue
                composed += 1
                f0, f1 = sample_field(0, previous, sizes), sample_field(1, current, sizes)
                t0, t1 = sample_field(2, previous, sizes), sample_field(3, current, sizes)
                bg = sample_field(4, uv, sizes)
                for channel in range(3):
                    expected = ((1 - phase)*f0[channel] + phase*f1[channel] +
                                ((1 - phase)*t0[channel] + phase*t1[channel])*bg[channel])
                    value = actual[pixel*4 + channel]
                    error = abs(value - expected)
                    max_error = max(max_error, error)
                    # Smooth fields also bound the sampler's subtexel precision
                    # error; this tolerance remains far below an 8-bit color step.
                    failures += not math.isfinite(value) or error > 2e-4
                failures += raw[pixel*16+12:(pixel+1)*16] != fallback_bytes[pixel*16+12:(pixel+1)*16]
        assert preserved and composed, 'Fixture must exercise both paths'
        case_report['phases'].append({'phase': phase, 'composed': composed,
                                      'exactFallback': preserved, 'maxError': max_error,
                                      'failures': failures})
    # The runner must reject an invalid region before recording GPU work, rather
    # than silently clamping a malformed host description into another view.
    invalid_layouts = []
    bad = [row.copy() for row in layouts]
    bad[0][2] = bad[0][0]
    invalid_layouts.append(('outside-allocation', bad, 'Invalid input layout'))
    bad = [row.copy() for row in layouts]
    bad[5][4] -= 1
    invalid_layouts.append(('fallback-size', bad, 'Fallback region must match output dimensions'))
    case_report['rejectedLayouts'] = []
    for label, bad, error in invalid_layouts:
        layout_path.write_text('\n'.join(' '.join(map(str, row)) for row in bad) + '\n', encoding='utf-8')
        target = folder / f'invalid-{label}.bin'
        result = subprocess.run([str(args.executable.resolve()), str(args.shader.resolve()), str(folder.resolve()),
                                 str(width), str(height), '.5', str(target.resolve())], capture_output=True, text=True)
        assert result.returncode != 0 and error in result.stdout and not target.exists(), result.stdout
        case_report['rejectedLayouts'].append(label)
    layout_path.write_text(layout_text, encoding='utf-8')
    return case_report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--shader', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = [
        ('upscale-changing-resolution', (192, 108),
         [(96, 54), (128, 72), (80, 45), (96, 54), (128, 72), (192, 108), (24, 14), (48, 27)], False),
        ('reversed-resolution-change', (192, 108),
         [(128, 72), (96, 54), (96, 54), (80, 45), (192, 108), (192, 108), (31, 17), (32, 18)], False),
        ('odd-extents-final-stage', (257, 145),
         [(97, 55), (129, 73), (113, 61), (173, 101), (193, 109), (257, 145), (29, 19), (51, 29)], False),
        ('downsample-intermediate-stage', (128, 72),
         [(192, 108), (256, 144), (173, 99), (211, 121), (257, 145), (128, 72), (48, 27), (32, 18)], False),
        ('stationary-coarse-motion-grid', (193, 109),
         [(79, 43), (127, 71), (97, 53), (137, 77), (113, 61), (193, 109), (1, 1), (39, 23)], True),
    ]
    results = [run_case(args, *case) for case in cases]
    report = {'cases': results,
              'pixels': sum(w*h*3 for _, (w, h), _, _ in cases),
              'failures': sum(phase['failures'] for case in results for phase in case['phases']),
              'gameAttachment': False, 'qualityAccepted': False}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({key: value for key, value in report.items() if key != 'cases'}))
    return int(report['failures'] != 0)


if __name__ == '__main__':
    raise SystemExit(main())
