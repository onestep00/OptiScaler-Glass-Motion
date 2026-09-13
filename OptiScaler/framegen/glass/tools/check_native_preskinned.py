"""Check bridge metadata isolation and negative native-expression preservation."""
import json
import re

from bridge_native_preskinned import add_preskinned_inputs, p
from match_shared_native_motion import Shader
from native_graft_checks import verify_graft


def main():
    index = json.loads((p / 'native-preskinned-grafted/index.json').read_text())
    checked = 0
    for row in index['shaders']:
        if row['status'] != 'offline_validated':
            continue
        original = (p / 'all-transparent-vs' / (row['sha256'] + '.ll')).read_text()
        augmented, required = add_preskinned_inputs(original)
        ids = re.findall(r'^!(\d+) = ', augmented, re.M)
        assert len(ids) == len(set(ids)), 'distinct metadata node reused'
        before, after = Shader(original), Shader(augmented)
        assert all(after.defs.get(key) == value for key, value in before.defs.items())
        assert after.roots == before.roots and after.blocks == before.blocks
        assert [(x['kind'], x['binding']) for x in required] == [(0, 9), (2, 3)]
        try:
            add_preskinned_inputs(augmented)
        except ValueError:
            pass
        else:
            raise AssertionError('existing native bindings were overwritten')
        checked += 1
    assert checked > 0
    row = next(r for r in index['shaders'] if r['status'] == 'offline_validated')
    original = (p / 'all-transparent-vs' / (row['sha256'] + '.ll')).read_text()
    shader = Shader(original)
    entry = re.search(r'!dx.entryPoints = !\{!(\d+)\}', original)[1]
    fields = shader.md[entry].split(', ')
    assert fields[-1] == 'null'
    node = max(map(int, re.findall(r'^!(\d+) = ', original, re.M))) + 1
    fields[-1] = '!' + str(node)
    flagged = original.replace(f'!{entry} = !{{{shader.md[entry]}}}', f'!{entry} = !{{' + ', '.join(fields) + '}')
    flagged += f'\n!{node} = !{{i32 0, i64 2}}\n'
    augmented, _ = add_preskinned_inputs(flagged)
    assert f'!{node} = !{{i32 0, i64 18}}' in augmented, 'unrelated shader flag lost'
    contracts = json.loads((p / 'shader-modifier-contracts.json').read_text())['shaders']
    match = next(x for x in json.loads((p / 'shared-native-motion-matches.json').read_text())['matches'] if x['sha256'] == row['sha256'])
    route = next(x for x in match['native_candidates'] if x['sha256'] == row['native_sha256'] and x.get('specializations', {}) == row['specializations'])
    native = (p / 'opaque-velocity-audit' / (row['native_sha256'] + '.ll')).read_text()
    generated = (p / 'native-preskinned-grafted' / (row['sha256'] + '.ll')).read_text()
    handles = {v for v, h in Shader(generated).handles.items() if h[0] == 2 and h[2] == 7}
    # Deliberately point one real prior-matrix read at the adjacent row.
    wrong, changed = re.subn(r'(@dx.op.cbufferLoadLegacy.\w+\(i32 59, %dx.types.Handle (\S+), i32 )24\)',
        lambda m: m[1] + ('25)' if m[2] in handles else '24)'), generated)
    assert changed and wrong != generated
    try:
        verify_graft(original, wrong, native, row, route, contracts)
    except AssertionError:
        pass
    else:
        raise AssertionError('wrong native previous matrix row accepted')
    print(json.dumps(dict(shaders=checked, metadata_and_flags=True,
                         negative_prior_row_rejected=True, runtime_admitted=False)))


if __name__ == '__main__':
    main()
