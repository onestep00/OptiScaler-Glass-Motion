"""Owned checks for split native clip components and invalid prior candidates."""
import json
from match_shared_native_motion import native_previous, Shader


class Fixture:
    def __init__(self, start, current=False, duplicate=False):
        # Match native packing across two output semantics, not one float4 name.
        self.roots = {5: {3: 'v0'}, 6: {i - 1: 'v' + str(i) for i in range(1, 4)}}
        self.rows = {'v' + str(i): [(1, start + i)] + ([(1, 28 + i)] if current else [])
                     for i in range(4)}
        if duplicate:
            self.roots[7] = {0: 'different'}
            self.rows['different'] = [(1, start)]

    def dependencies_values(self, values):
        return dict(cb_rows=list({row for v in values for row in self.rows[v]}))


def main():
    for start in (12, 16):
        result = native_previous(Fixture(start))
        assert result[0]['camera_rows'] == list(range(start, start + 4))
        assert result[0]['components'] == [[5, 3], [6, 0], [6, 1], [6, 2]]
        for invalid in (Fixture(start, current=True), Fixture(start, duplicate=True)):
            try:
                native_previous(invalid)
            except ValueError:
                pass
            else:
                raise AssertionError('ambiguous or current-dependent clip accepted')
    missing = Fixture(12)
    del missing.roots[6][2]
    try:
        native_previous(missing)
    except ValueError:
        pass
    else:
        raise AssertionError('incomplete prior clip accepted')
    shader=Shader.__new__(Shader)
    shader.defs={
        '%1':'fsub fast float %10, %3', '%2':'fsub fast float %11, %4',
        '%3':'fmul fast float %5, %13', '%4':'fmul fast float %13, %6',
        '%5':'extractvalue %dx.types.CBufRet.f32 %7, 0',
        '%6':'extractvalue %dx.types.CBufRet.f32 %7, 1',
        '%7':'call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %8, i32 51)'}
    shader.handles={'%8':(2,0,1,'false')};shader.resources={(2,0):('i32 0',)}
    roots=['%1','%2','%12','%13']
    assert shader.before_jitter_subtraction(roots)==['%10','%11','%12','%13']
    for value, invalid in (('%1','fadd fast float %10, %3'),('%3','fmul fast float %5, %14'),
                           ('%6','extractvalue %dx.types.CBufRet.f32 %7, 0'),
                           ('%7',shader.defs['%7'].replace('i32 51','i32 50'))):
        saved=shader.defs[value];shader.defs[value]=invalid
        try:shader.before_jitter_subtraction(roots)
        except ValueError:pass
        else:raise AssertionError('invalid jitter subtraction accepted')
        shader.defs[value]=saved
    print(json.dumps(dict(camera_layouts=2, split_outputs=True, explicit_jitter_subtraction=True,
                          invalid_rejected=True, game_attached=False)))


if __name__ == '__main__':
    main()
