"""Synthetic 3D layered scene and explicit native replay camera packets.

Uses caller-owned geometry, never game data. All matrices exclude jitter.
The private GFR1 adapter retains the caller's observed provider parameter ABI;
its pixel MV scales and degree FOV differ from public sl::Constants units.
"""
import struct
import sys
from pathlib import Path
import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_object_motion as geometry


def camera_world(scene, time):
    if scene == 'stationary':
        return np.eye(4)
    return geometry.transform((.055*(time-5), .008*(time-5), .014*(time-5)),
                              (.002*(time-5), -.006*(time-5), .001*(time-5)))


def objects_at(scene, time, width, height):
    projection = geometry.camera(width, height)
    cc = projection @ np.linalg.inv(camera_world(scene, time))
    pc = projection @ np.linalg.inv(camera_world(scene, time-1))
    quad = np.array([[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]], dtype='f4')

    def matrix(i, t):
        if i == 0:  # opaque wall
            return geometry.transform((0, 0, 15), scale=(18, 11, 1))
        if i == 1:  # independently moving opaque background object
            return geometry.transform((.24*(t-5), -.15, 8), scale=(.9, 1.25, 1))
        if i == 2:  # near transparent pane
            return geometry.transform((-.12, .05, 3.7), (.01, -.04, .012), (2.3, 1.34, 1))
        if i == 3:  # second transparent object underneath the pane
            dt = t-5 if scene != 'stationary' else 0
            return geometry.transform((.24+.035*dt, -.08+.012*dt, 5.1+.01*dt),
                                      (.04+.013*dt, .16+.037*dt, -.06+.018*dt), (.64, .82, 1))
        # Opaque phase/scale control, away from the transparent objects.
        return geometry.transform((-1.5+.045*(t-5), .97, 2.9), scale=(.16, .11, 1))

    return [dict(current=matrix(i, time).astype('f4').astype('f8'),
                 previous=matrix(i, time-1).astype('f4').astype('f8'),
                 current_camera=cc.astype('f4').astype('f8'), previous_camera=pc.astype('f4').astype('f8'),
                 cp=quad, pp=quad, uv=(quad[:, :2]+1)*.5, jitter=np.zeros(2),
                 opacity=(.08 if i == 2 else .15) if i in (2, 3) else 1., identity=101+i, hole=False)
            for i in range(5)]


def analytic_layers(objects, width, height):
    """Independent inverse projective-plane map, used for fractional-time truth."""
    y, x = np.mgrid[:height, :width]
    grid = np.stack(((x+.5)/width, (y+.5)/height, np.ones_like(x)), axis=-1)
    layers = []
    for obj in objects:
        matrix = obj['current_camera'] @ obj['current']
        h = np.stack((.5*(matrix[0]+matrix[3]), .5*(matrix[3]-matrix[1]), matrix[3]))[:, [0, 1, 3]]
        local = grid @ np.linalg.inv(h).T
        local = local[..., :2] / local[..., 2:3]
        points = np.concatenate((local, np.zeros((*local.shape[:2], 1))), axis=-1)
        current = geometry.project(points, matrix)
        previous = geometry.project(points, obj['previous_camera'] @ obj['previous'])
        mask = (np.max(np.abs(local), axis=-1) < 1) & (current[..., 3] > 0)
        motion = np.zeros((height, width, 4), 'f4')
        motion[..., :2] = geometry.screen(previous)-geometry.screen(current)
        motion[..., 2] = current[..., 2]/current[..., 3]
        motion[..., 3] = previous[..., 3] > 0
        coverage = np.zeros_like(motion)
        coverage[..., 0] = mask*obj['identity']
        coverage[..., 1] = mask*obj['opacity']
        coverage[..., 2:] = (local+1)*.5
        layers.append(dict(motion=motion, coverage=coverage))
    return layers


def inner_edge(mask, radius):
    kernel = np.ones((2*radius+1, 2*radius+1), 'uint8')
    return mask & (cv2.erode(mask.astype('uint8'), kernel, borderType=cv2.BORDER_CONSTANT, borderValue=1) == 0)


def compose(layers):
    """Display-linear scalar-alpha fixture. No refraction, SR, RR or tone map."""
    shape = layers[0]['motion'].shape[:2]
    color = np.zeros((*shape, 3), 'f4')
    depth = np.ones(shape, 'f4')
    mv = np.zeros((*shape, 2), 'f4')
    masks = [layer['coverage'][..., 0] != 0 for layer in layers]
    for i in (0, 1, 4):
        layer = layers[i]
        u, v = layer['coverage'][..., 2], layer['coverage'][..., 3]
        if i == 0:
            base = np.stack((.19+.065*np.sin(u*130), .23+.08*np.cos(v*98),
                             .28+.055*np.sin(u*92+v*53)), axis=-1)
            base += ((np.mod(u*46, 1) < .055) | (np.mod(v*34, 1) < .035))[..., None]*.19
        elif i == 1:
            base = np.stack((.12+.08*np.sin(u*32), .63+.13*np.cos(v*28),
                             .15+.065*np.sin(v*22+u*12)), axis=-1)
            base += (np.mod(u*9+v*.7, 1) < .07)[..., None]*.19
        else:
            base = np.broadcast_to([.95, .03, .88], (*shape, 3))
        visible = masks[i] & (layer['motion'][..., 2] < depth)
        color[visible], depth[visible], mv[visible] = base[visible], layer['motion'][visible, 2], layer['motion'][visible, :2]
    background = color.copy()
    source = np.zeros_like(color)
    transmission = np.ones(shape, 'f4')
    alphas, visible_masks = {}, {}
    for i in (3, 2):
        layer = layers[i]
        visible = masks[i] & (layer['motion'][..., 2] < depth)
        u, v = layer['coverage'][..., 2], layer['coverage'][..., 3]
        d = np.minimum.reduce([u, v, 1-u, 1-v])
        if i == 2:
            alpha = .08+.27*np.exp(-(d/.009)**2)
            tint = np.array([.3, .69, .89])
        else:
            alpha = .15+.45*np.exp(-(d/.022)**2)+.13*np.exp(-((v-.4)/.018)**2)
            tint = np.array([.94, .52, .18])
        alpha = (alpha*visible).astype('f4')
        color = alpha[..., None]*tint+(1-alpha[..., None])*color
        source = alpha[..., None]*tint+(1-alpha[..., None])*source
        transmission *= 1-alpha
        alphas[i], visible_masks[i] = alpha, visible
    assert np.max(np.abs(color-(source+transmission[..., None]*background))) < 1e-6
    return dict(color=np.clip(color, 0, 1), background=background, source=source, transmission=transmission,
                depth=depth, mv=mv, raw_masks=masks, masks=visible_masks, alphas=alphas,
                control=masks[4], figure=masks[1])


def select(layers, scene, radius, change_depth, interior_gain=1.):
    original = scene['mv']
    mv, depth = original.copy(), scene['depth'].copy()
    for i in (3, 2):
        w = np.clip(scene['alphas'][i][..., None]*interior_gain, 0, 1)
        mv += w*(layers[i]['motion'][..., :2]-mv)
    edges = {i: inner_edge(scene['raw_masks'][i], radius) & scene['masks'][i] for i in (2, 3)} if radius else {}
    count = sum(e.astype('uint8') for e in edges.values()) if edges else np.zeros(depth.shape, 'uint8')
    checks = []
    for i, edge in edges.items():
        selected = edge & (count == 1)
        mv[selected] = layers[i]['motion'][selected, :2]
        if change_depth:
            depth[selected] = layers[i]['motion'][selected, 2]
        assert np.array_equal(mv[selected], layers[i]['motion'][selected, :2])
        checks.append(int(selected.sum()))
    mv[count > 1], depth[count > 1] = original[count > 1], scene['depth'][count > 1]
    outside = ~(scene['masks'][2] | scene['masks'][3])
    assert np.array_equal(mv[outside], original[outside])
    assert np.array_equal(depth[outside], scene['depth'][outside])
    # Compare the exact half-precision values that will reach FG.
    half = mv.astype('<f2')
    edge_error = 0.
    for i, edge in edges.items():
        selected = edge & (count == 1)
        if selected.any():
            assert np.array_equal(half[selected], layers[i]['motion'][selected, :2].astype('<f2'))
            edge_error = max(edge_error, float(np.max(np.linalg.norm(
                (half[selected].astype('f4')-layers[i]['motion'][selected, :2])*depth.shape[::-1], axis=-1))))
    return half, depth, dict(edgePixels=checks, conflictingPixels=int((count > 1).sum()), interiorGain=interior_gain,
                            outsideExact=True, edgeHalfExact=True, halfMotionErrorPixels=edge_error,
                            changedPixels=int(np.any(half != original.astype('<f2'), axis=-1).sum()))


def write_packets(template, destination, scene, frames, width, height, rw, rh):
    raw = template.read_bytes()
    magic, available = struct.unpack_from('<II', raw)
    assert magic == 0x31524647 and available >= frames
    records, cursor = [], 8
    for frame in range(available):
        index, count = struct.unpack_from('<II', raw, cursor); cursor += 8
        assert index == frame
        rows = []
        for _ in range(count):
            slot, result, bits, length, kind, size = struct.unpack_from('<IIQIII', raw, cursor); cursor += 28
            key = raw[cursor:cursor+length].decode(); cursor += length
            payload = raw[cursor:cursor+size]; cursor += size
            rows.append((slot, result, bits, key, kind, payload))
        records.append(rows)
    assert cursor == len(raw)
    with destination.open('xb') as output:
        output.write(struct.pack('<II', magic, frames))
        for frame in range(frames):
            p = geometry.camera(width, height)
            cw, pw = camera_world(scene, frame), camera_world(scene, frame-1)
            cc, pc = p @ np.linalg.inv(cw), p @ np.linalg.inv(pw)
            ctp = pc @ np.linalg.inv(cc)
            matrices = {'ClipToPrevClip': ctp, 'PrevClipToClip': np.linalg.inv(ctp),
                        'CameraViewToClip': p, 'ClipToCameraView': np.linalg.inv(p)}
            floats = {'CameraNear': .1, 'CameraFar': 100., 'CameraFOV': 58., 'CameraAspectRatio': width/height,
                      'MvecScaleX': rw, 'MvecScaleY': rh, 'JitterOffsetX': 0, 'JitterOffsetY': 0}
            for name, vector in (('CameraPos', cw[:3, 3]), ('CameraRight', cw[:3, 0]),
                                 ('CameraUp', cw[:3, 1]), ('CameraFwd', cw[:3, 2])):
                floats.update({name+axis: value for axis, value in zip('XYZ', vector)})
            integers = {'DepthInverted': 0, 'Reset': int(frame == 0), 'CameraMotionIncluded': 1, 'MvecJittered': 0}
            for resource in ('Backbuffer', 'HUDLess', 'Depth', 'MVecs'):
                w, h = (rw, rh) if resource in ('Depth', 'MVecs') else (width, height)
                integers.update({resource+'SubrectWidth': w, resource+'SubrectHeight': h})
            output.write(struct.pack('<II', frame, len(records[frame])))
            changed = set()
            for slot, result, bits, key, kind, payload in records[frame]:
                name = key.removeprefix('DLSSG.')
                if result == 1:
                    if name in matrices:
                        payload = matrices[name].T.astype('<f4').tobytes(); changed.add(name)
                    elif name in floats:
                        bits = struct.unpack('<I', struct.pack('<f', floats[name]))[0]; changed.add(name)
                    elif name in integers:
                        bits = integers[name]; changed.add(name)
                encoded = key.encode()
                output.write(struct.pack('<IIQIII', slot, result, bits, len(encoded), kind, len(payload)))
                output.write(encoded); output.write(payload)
            assert changed == set(matrices) | set(floats) | set(integers)
