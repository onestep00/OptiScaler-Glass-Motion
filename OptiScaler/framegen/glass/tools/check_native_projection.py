"""Owned checks for split native clip components and invalid prior candidates."""
import json
from match_shared_native_motion import native_previous


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
    print(json.dumps(dict(camera_layouts=2, split_outputs=True, invalid_rejected=True, game_attached=False)))


if __name__ == '__main__':
    main()
