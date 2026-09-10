"""Optional frozen-material fixture for independent FG experiments.

F/T/U are caller-supplied local data. Motion, depth, background and display
mapping are synthetic. This is not a game capture or a general material model.
Requires NumPy and OpenCV. No game binaries or buffers are distributed.
"""
import hashlib
import cv2
import numpy as np


def encode(c):
    c = np.maximum(c, 0)
    return (c / (1 + c)) ** (1 / 2.2)


class Fixture:
    def __init__(self, material, audit, manifest):
        self.w, self.h = manifest['outputWidth'], manifest['outputHeight']
        self.rw, self.rh = manifest['renderWidth'], manifest['renderHeight']
        self.n = manifest['frames']
        aw, ah = audit['extent']
        self.hashes = {}
        arrays = []
        for name in ('source', 'transmission', 'uncovered'):
            file = material / (name + '.rgba16f')
            raw = file.read_bytes()
            digest = hashlib.sha256(raw).hexdigest()
            assert digest == audit['rawSha256'][file.name]
            assert len(raw) == aw * ah * 8
            a = np.frombuffer(raw, '<f2').reshape(ah, aw, 4)[..., :3].astype(np.float32)
            assert np.isfinite(a).all()
            arrays.append(cv2.resize(a, (self.w, self.h), interpolation=cv2.INTER_AREA))
            self.hashes[file.name] = digest
        self.f, self.t, uncovered = arrays
        assert (self.f >= 0).all() and (self.t >= 0).all() and (self.t <= 1).all()
        self.coverage = (np.min(uncovered, axis=2) < 1).astype(np.float32)
        self.y, self.x = np.mgrid[:self.h, :self.w].astype(np.float32)

    def scene(self, name, time):
        x, y = self.x, self.y
        sx, sy = self.w / 1280, self.h / 720
        vs = 0 if name == 'stationary' else 18 * sx
        offset = vs * (time - 5)
        f = cv2.remap(self.f, x-offset, y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT)
        t = cv2.remap(self.t, x-offset, y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_CONSTANT,
                      borderValue=(1, 1, 1))
        cov = cv2.remap(self.coverage, x-offset, y, cv2.INTER_LINEAR,
                        borderMode=cv2.BORDER_CONSTANT) > 0
        if name == 'stationary':
            u, v = x / sx - (635 + 34 * (time - 5)), y / sy - 365
            head = (u/31)**2 + ((v+47)/38)**2 < 1
            figure = head | ((np.abs(u) < 88) & (v >= -15) & (v < 170))
            b = np.stack((.28+.08*np.sin(x/sx*.034), .35+.11*np.cos(y/sy*.04),
                          .4+.1*np.sin(x/sx*.027+y/sy*.019)), axis=2)
            shirt = np.stack((.09+.05*np.sin(u*.12), .55+.25*np.cos(v*.055),
                              .22+.09*np.sin(u*.08+v*.04)), axis=2)
            b[figure] = shirt[figure]
            b[head] = [.9, .4, .23]
            b[figure & (np.mod(u+v*.2, 19) < 2)] += .5
            vb = figure.astype(np.float32) * 34 * sx
            depth = np.where(figure, .02/8, .02/15).astype(np.float32)
        else:
            bx = x/sx - 6*time
            b = np.stack((.5+.3*np.sin(bx*.057)+.1*np.cos(y/sy*.067),
                          .6+.25*np.cos(bx*.036+y/sy*.023),
                          .7+.3*np.sin(bx*.022-y/sy*.041)), axis=2)
            b += (((np.mod(bx+100, 89) < 3) | (np.mod(y/sy+bx*.19, 73) < 2))*.6)[..., None]
            figure = np.zeros_like(cov)
            vb = np.full_like(x, 6*sx)
            depth = np.full_like(x, .02/15)
        opaque = (x >= 160*sx+offset) & (x < 230*sx+offset) & (y >= 45*sy) & (y < 92*sy)
        f[opaque], t[opaque], cov[opaque] = [2., .12, .03], 0, True
        return dict(c=encode(f+t*b), f=f, t=t, b=b, cov=cov, figure=figure,
                    vb=vb, vs=vs, depth=depth, opaque=opaque)
