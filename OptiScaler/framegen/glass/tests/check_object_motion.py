"""Rasterized synthetic geometry versus independent double-precision ray hits.

No game capture, engine history acquisition or FG-quality claim. NumPy/OpenCV/PIL
are optional test dependencies. Explicit per-object masks are an oracle input;
their production acquisition and overlapping-edge arbitration remain unresolved.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import cv2
import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent


def transform(position=(0, 0, 0), angles=(0, 0, 0), scale=(1, 1, 1)):
    x, y, z = angles
    rx = np.array([[1, 0, 0], [0, np.cos(x), -np.sin(x)], [0, np.sin(x), np.cos(x)]])
    ry = np.array([[np.cos(y), 0, np.sin(y)], [0, 1, 0], [-np.sin(y), 0, np.cos(y)]])
    rz = np.array([[np.cos(z), -np.sin(z), 0], [np.sin(z), np.cos(z), 0], [0, 0, 1]])
    result = np.eye(4)
    result[:3, :3] = rz @ ry @ rx @ np.diag(scale)
    result[:3, 3] = position
    return result


def camera(width, height, position=(0, 0, 0), angles=(0, 0, 0)):
    near, far, f = .1, 100., 1 / np.tan(np.deg2rad(58) / 2)
    projection = np.zeros((4, 4))
    projection[0, 0], projection[1, 1] = f * height / width, f
    projection[2, 2], projection[2, 3], projection[3, 2] = far / (far-near), -near*far/(far-near), 1
    return projection @ np.linalg.inv(transform(position, angles))


def homogeneous(points):
    return np.concatenate((points, np.ones((*points.shape[:-1], 1))), axis=-1)


def project(points, matrix):
    return homogeneous(points) @ matrix.T


def screen(clip):
    return clip[..., :2] / clip[..., 3:4] * [0.5, -0.5] + .5


def make_fixture(name, width, height):
    camera_moved = name in ('camera_only', 'combined', 'opaque_occlusion')
    cc = camera(width, height, (.13, .03, .06) if camera_moved else (0, 0, 0),
                (.015, .025, 0) if camera_moved else (0, 0, 0))
    pc = camera(width, height, (-.11, -.03, -.15) if camera_moved else (0, 0, 0),
                (-.012, -.035, 0) if camera_moved else (0, 0, 0))
    quad = np.array([[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]], dtype=np.float64)
    uv = (quad[:, :2] + 1) * .5
    objects = []
    for i in range(2):
        location = np.array([-.07, .02, 3.4]) if i == 0 else np.array([.25, -.06, 4.5])
        angles = np.array([.08, -.14, .025]) if i == 0 else np.array([-.17, .31, -.12])
        size = (2.45, 1.5, 1) if i == 0 else (.52, .76, 1)
        current = transform(location, angles, size)
        previous_location, previous_angles = location.copy(), angles.copy()
        if name in ('object_translation', 'combined', 'opaque_occlusion'):
            previous_location += [-.19 if i == 0 else .24, .07, -.09]
        if name in ('object_rotation', 'combined'):
            previous_angles += [.05, -.19 if i == 0 else .27, -.055]
        if name == 'invalid_previous':
            previous_location[2] = -4
        if name == 'previous_offscreen':
            previous_location[0] += 5
        previous = transform(previous_location, previous_angles, size)
        if name == 'edge_conflict' and i == 1:
            current = objects[0]['current'].copy()
            previous = current.copy()
            previous[0, 3] += .3
        cp, pp = quad.copy(), quad.copy()
        if name in ('deformation', 'combined') and i == 1:
            cp[2] += [.08, .06, .13]
            pp[2] += [-.09, -.03, -.22]
            pp[1] += [.03, -.06, .12]
        jitter = np.array([.375/width, -.25/height]) if name in ('jitter_only', 'combined') else np.zeros(2)
        # Float32 inputs are exactly those sent to the GPU. CPU ray math remains double precision.
        objects.append(dict(current=current.astype('f4').astype('f8'), previous=previous.astype('f4').astype('f8'),
                            current_camera=cc.astype('f4').astype('f8'), previous_camera=pc.astype('f4').astype('f8'),
                            cp=cp.astype('f4').astype('f8'), pp=pp.astype('f4').astype('f8'), uv=uv,
                            jitter=jitter.astype('f4').astype('f8'), opacity=.08 if i == 0 else .12,
                            identity=101+i, hole=name in ('deformation', 'combined') and i == 1))
    return objects


def write_fixture(path, objects, width, height):
    path.mkdir()
    (path/'config.txt').write_text(f'{width} {height} {len(objects)}\n')
    indices = [0, 1, 2, 0, 2, 3]
    for i, obj in enumerate(objects):
        constants = np.concatenate([obj[k].T.ravel() for k in
                                    ('current', 'previous', 'current_camera', 'previous_camera')] +
                                   [np.array([*obj['jitter'], obj['opacity'], obj['identity']]),
                                    np.array([obj['hole'], 1.7, -.08, 0])]).astype('<f4')
        vertices = np.concatenate((obj['cp'], obj['pp'], obj['uv']), axis=1)[indices].astype('<f4')
        constants.tofile(path/f'constants-{i}.bin')
        vertices.tofile(path/f'vertices-{i}.bin')


def ray_reference(obj, width, height):
    """Intersect current camera rays with world-space triangles, then follow the same material point."""
    y, x = np.mgrid[:height, :width]
    uv = np.stack(((x+.5)/width, (y+.5)/height), axis=-1) - obj['jitter']
    ndc = uv * [2, -2] + [-1, 1]
    inverse = np.linalg.inv(obj['current_camera'])
    near = np.concatenate((ndc, np.zeros((height, width, 1)), np.ones((height, width, 1))), axis=-1) @ inverse.T
    far = np.concatenate((ndc, np.ones((height, width, 2))), axis=-1) @ inverse.T
    origin, endpoint = near[..., :3]/near[..., 3:4], far[..., :3]/far[..., 3:4]
    direction = endpoint-origin
    wc = project(obj['cp'], obj['current'])[..., :3]
    wp = project(obj['pp'], obj['previous'])[..., :3]
    best = np.full((height, width), np.inf)
    expected = np.zeros((height, width, 4))
    expected_uv = np.zeros((height, width, 2))
    camera_only = np.zeros((height, width, 2))
    found = np.zeros((height, width), bool)
    for tri in ([0, 1, 2], [0, 2, 3]):
        a, b, c = wc[tri]
        e1, e2 = b-a, c-a
        normal = np.cross(e1, e2)
        denominator = direction @ normal
        distance = ((a-origin) @ normal) / denominator
        point = origin + distance[..., None]*direction
        q = point-a
        d00, d01, d11 = e1@e1, e1@e2, e2@e2
        det = d00*d11-d01*d01
        u = (d11*(q@e1)-d01*(q@e2))/det
        v = (d00*(q@e2)-d01*(q@e1))/det
        bary = np.stack((1-u-v, u, v), axis=-1)
        hit = (np.min(bary, axis=-1) >= -1e-4) & (distance > 0) & (distance < best)
        previous = bary @ wp[tri]
        pc = project(previous, obj['previous_camera'])
        cc = project(point, obj['current_camera'])
        puv = screen(pc)
        valid = pc[..., 3] > 1e-6
        mv = puv-screen(cc)
        values = np.concatenate((np.where(valid[..., None], mv, 0),
                                 (cc[..., 2]/cc[..., 3])[..., None], valid[..., None]), axis=-1)
        expected[hit] = values[hit]
        expected_uv[hit] = (bary @ obj['uv'][tri])[hit]
        # Wrong ablation: hold CURRENT world geometry fixed while changing only the camera.
        only = screen(project(point, obj['previous_camera']))-screen(cc)
        camera_only[hit] = only[hit]
        best[hit], found[hit] = distance[hit], True
    return expected, expected_uv, found, camera_only


def edge(mask):
    # Outside the viewport is unknown, not an invented object silhouette.
    inside = cv2.erode(mask.astype('uint8'), np.ones((3, 3), 'uint8'),
                       borderType=cv2.BORDER_CONSTANT, borderValue=1) != 0
    return mask & ~inside


def field(motion, width, height):
    pixels = motion * [width, height]
    angle = np.arctan2(pixels[..., 1], pixels[..., 0])
    hsv = np.empty((height, width, 3), 'uint8')
    hsv[..., 0] = np.uint8((angle+np.pi)/(2*np.pi)*179)
    hsv[..., 1] = 220
    hsv[..., 2] = np.uint8(np.clip(np.linalg.norm(pixels, axis=-1)/30, 0, 1)*230+15)
    return cv2.cvtColor(hsv, cv2.COLOR_HSV2RGB)


def analyze_case(path, objects, width, height):
    arrays = [{name: np.fromfile(path/'gpu'/f'{name}-{i}.bin', '<f4').reshape(height, width, 4)
               for name in ('motion', 'coverage', 'weights')} for i in range(len(objects))]
    rows = []
    for i, (obj, data) in enumerate(zip(objects, arrays)):
        covered = data['coverage'][..., 0] != 0
        assert np.all(data['coverage'][covered, 0] == obj['identity'])
        assert np.isfinite(data['motion']).all() and np.isfinite(data['coverage']).all()
        expected, expected_uv, found, camera_only = ray_reference(obj, width, height)
        # Fixed-point raster coverage can differ at a mathematical edge; report that subset explicitly.
        examined = covered & found
        assert examined.sum() > 100
        assert (covered & ~found).sum() <= max(4, .001*covered.sum())
        assert np.array_equal(data['motion'][examined, 3], expected[examined, 3])
        error_px = np.linalg.norm((data['motion'][..., :2]-expected[..., :2])*[width, height], axis=-1)
        depth_error = np.abs(data['motion'][..., 2]-expected[..., 2])
        uv_error = np.max(np.abs(data['coverage'][..., 2:4]-expected_uv), axis=-1)
        assert error_px[examined].max() < .003, (path.name, i, error_px[examined].max())
        assert depth_error[examined].max() < 2e-6
        # Rasterizer interpolation uses snapped primitive setup; the independent
        # ray intersection uses unsnapped double precision geometry. Keep this
        # material-coordinate residual separate from the tighter MV pixel test.
        assert uv_error[examined].max() < 1e-4
        interior = np.clip(obj['opacity']*1.7-.08, 0, 1)
        assert np.max(np.abs(data['weights'][covered, 0]-interior)) < 1e-6
        assert np.all(data['weights'][covered, 1] == 1)
        assert np.max(np.abs(data['weights'][covered, 2]-(interior+1)*.5)) < 1e-6
        if obj['hole']:
            assert np.all(np.linalg.norm(data['coverage'][covered, 2:4]-.5, axis=1) >= .14999)
        wrong_error = np.linalg.norm((camera_only-expected[..., :2])*[width, height], axis=-1)
        valid = examined & (expected[..., 3] == 1)
        y, x = np.mgrid[:height, :width]
        current_uv = np.stack(((x+.5)/width, (y+.5)/height), axis=-1) - obj['jitter']
        previous_uv = current_uv + expected[..., :2]
        outside_previous = valid & np.any((previous_uv < 0) | (previous_uv > 1), axis=-1)
        rows.append(dict(object=obj['identity'], coveredPixels=int(covered.sum()), checkedPixels=int(examined.sum()),
                         uncoveredRayEdgePixels=int((covered & ~found).sum()), validPreviousPixels=int(valid.sum()),
                         maxMotionErrorPixels=float(error_px[examined].max()),
                         maxDepthError=float(depth_error[examined].max()),
                         maxMaterialUVError=float(uv_error[examined].max()),
                         previousUVOutsidePixels=int(outside_previous.sum()),
                         cameraOnlyErrorPixelsP95=float(np.percentile(wrong_error[valid], 95)) if valid.any() else None))
    raw_masks = [a['coverage'][..., 0] != 0 for a in arrays]
    opaque_depth = np.ones((height, width))
    if path.name == 'opaque_occlusion':
        # Oracle opaque visibility, separate from transparent object silhouettes.
        # The GPU renders per-object geometry; this depth comparison is CPU-only.
        opaque_depth[int(height*.24):int(height*.76), int(width*.53):int(width*.61)] = .9
    masks = [m & (a['motion'][..., 2] <= opaque_depth) for m, a in zip(raw_masks, arrays)]
    edges = [edge(raw) & visible for raw, visible in zip(raw_masks, masks)]
    occlusion_cuts = np.logical_or.reduce([edge(visible) & ~edge(raw) for raw, visible in zip(raw_masks, masks)])
    union_edge = edge(np.logical_or.reduce(masks))
    individual_edge = np.logical_or.reduce(edges)
    lost = individual_edge & ~union_edge
    edge_count = np.sum(edges, axis=0)
    unambiguous = edge_count == 1
    background_mv = np.broadcast_to(np.array([-.025, .006], 'f4'), (height, width, 2)).copy()
    alpha_mv, result = background_mv.copy(), background_mv.copy()
    weights = np.zeros((height, width), 'f4')
    # Oracle fixture selection for illustration only: interiors back-to-front;
    # a single valid object edge overrides interior candidates from other layers.
    for i in reversed(range(len(objects))):
        valid = masks[i] & (arrays[i]['motion'][..., 3] == 1)
        opacity = objects[i]['opacity']
        alpha_mv[valid] = background_mv[valid] + opacity*(arrays[i]['motion'][valid, :2]-background_mv[valid])
        weight = arrays[i]['weights'][..., 0]
        result[valid] = background_mv[valid] + weight[valid, None]*(arrays[i]['motion'][valid, :2]-background_mv[valid])
        weights[valid] = weight[valid]
    checked_edges = 0
    for i, e in enumerate(edges):
        admitted = e & unambiguous & (arrays[i]['motion'][..., 3] == 1)
        result[admitted] = arrays[i]['motion'][admitted, :2]
        weights[admitted] = arrays[i]['weights'][admitted, 1]
        assert np.array_equal(result[admitted], arrays[i]['motion'][admitted, :2])
        checked_edges += int(admitted.sum())
    # A single MV cannot assert both different motions at a coincident edge.
    # Report and bypass this unresolved case instead of averaging the two edges.
    conflicts = edge_count > 1
    result[conflicts] = background_mv[conflicts]
    weights[conflicts] = 0
    outside = ~np.logical_or.reduce(masks)
    assert np.array_equal(result[outside], background_mv[outside])
    y, x = np.mgrid[:height, :width]
    scene = np.stack((.22+.1*np.sin(x*.1), .25+.15*np.cos(y*.12), .3+.13*np.sin((x+y)*.09)), axis=-1)
    for i in reversed(range(len(objects))):
        tint = np.array([.1, .75, .9]) if i == 0 else np.array([.95, .55, .1])
        alpha = masks[i][..., None]*objects[i]['opacity']
        scene = alpha*tint+(1-alpha)*scene
    scene[opaque_depth < 1] = [.36, .16, .08]
    old_edge_view = np.repeat(union_edge[..., None], 3, axis=2).astype('uint8')*255
    edge_view = np.zeros((height, width, 3), 'uint8')
    edge_view[edges[0]], edge_view[edges[1]] = [30, 210, 245], [255, 150, 30]
    panels = [('Synthetic raster scene (not game)', np.uint8(np.clip(scene, 0, 1)*255)),
              ('Merged mask boundary', old_edge_view), ('Separate object boundaries', edge_view),
              ('Alpha-only input MV', field(alpha_mv, width, height)),
              ('Object-edge input MV (not FG output)', field(result, width, height)),
              ('Surface MV weight', np.repeat(np.uint8(weights[..., None]*255), 3, axis=2))]
    sheet = Image.new('RGB', (width*3, (height+24)*2))
    draw = ImageDraw.Draw(sheet)
    for n, (label, rgb) in enumerate(panels):
        left, top = n % 3 * width, n // 3 * (height+24)
        sheet.paste(Image.fromarray(rgb), (left, top+24))
        draw.text((left+5, top+5), label, fill='white')
    sheet.save(path/'comparison.png')
    return dict(objects=rows, separateBoundaryPixels=int(individual_edge.sum()),
                boundariesLostByMergedMask=int(lost.sum()), ambiguousOverlappingEdgePixels=int((edge_count>1).sum()),
                rejectedOpaqueOcclusionCutPixels=int(occlusion_cuts.sum()),
                admittedBoundaryMVExactPixels=checked_edges, outsideOriginalExact=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--executable', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--width', type=int, default=384)
    p.add_argument('--height', type=int, default=216)
    args = p.parse_args()
    if not 64 <= args.width <= 1024 or not 64 <= args.height <= 1024:
        p.error('Fixture dimensions must be between 64 and 1024')
    args.output.mkdir(parents=True, exist_ok=False)
    results = {}
    for name in ('static', 'camera_only', 'object_translation', 'object_rotation', 'deformation', 'combined',
                 'jitter_only', 'previous_offscreen', 'invalid_previous', 'edge_conflict', 'opaque_occlusion'):
        path = args.output/name
        path.mkdir()
        objects = make_fixture(name, args.width, args.height)
        write_fixture(path/'input', objects, args.width, args.height)
        run = subprocess.run([str(args.executable.resolve()), str(ROOT/'ObjectMotion.hlsl'),
                              str((path/'input').resolve()), str((path/'gpu').resolve())],
                             capture_output=True, text=True)
        (path/'runner.log').write_text(run.stdout+run.stderr)
        run.check_returncode()
        results[name] = analyze_case(path, objects, args.width, args.height)
        print(name, json.dumps(results[name]), flush=True)
    for name in ('object_translation', 'object_rotation', 'deformation', 'combined'):
        assert max(o['cameraOnlyErrorPixelsP95'] or 0 for o in results[name]['objects']) > .2
    for name in ('static', 'jitter_only'):
        assert max(o['cameraOnlyErrorPixelsP95'] or 0 for o in results[name]['objects']) < 1e-8
    assert all(o['validPreviousPixels'] == 0 for o in results['invalid_previous']['objects'])
    assert sum(o['previousUVOutsidePixels'] for o in results['previous_offscreen']['objects']) > 100
    assert results['combined']['boundariesLostByMergedMask'] > 50
    assert results['edge_conflict']['ambiguousOverlappingEdgePixels'] > 100
    assert results['edge_conflict']['admittedBoundaryMVExactPixels'] == 0
    assert results['opaque_occlusion']['rejectedOpaqueOcclusionCutPixels'] > 50
    report = dict(width=args.width, height=args.height, cases=results,
                  shaderSha256=hashlib.sha256((ROOT/'ObjectMotion.hlsl').read_bytes()).hexdigest(),
                  helperSha256=hashlib.sha256((ROOT.parent/'GlassObjectMotion.hlsli').read_bytes()).hexdigest(),
                  executableSha256=hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                  gameCapture=False, engineObjectHistoryAcquired=False, actualFG=False, gameQualityAccepted=False,
                  edgeSelection='Oracle per-object masks; ambiguous overlapping edge pixels are NOT claimed solved')
    (args.output/'audit.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
