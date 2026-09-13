"""Audit actual current/prior input dependencies for every native velocity VS.

Offline only. Input reads do not establish runtime values or object identity.
"""
from pathlib import Path
from collections import Counter
import hashlib
import json

from match_shared_native_motion import Shader, native_previous, p


def main():
    inventory = json.loads((p / 'opaque-velocity-audit/index.json').read_text())
    contracts = json.loads((p / 'shader-modifier-contracts.json').read_text())
    assert inventory['summary']['cache_sha256'] == contracts['cache_sha256']
    rows = []
    patterns = Counter()
    for entry in inventory['shaders']:
        sha = entry['sha256']
        source = Path(entry['disassembly']).read_text()
        row = dict(sha256=sha, disassembly_sha256=hashlib.sha256(source.encode()).hexdigest())
        try:
            shader = Shader(source, contracts['shaders'].get(sha))
            prior = native_previous(shader)[0]
            previous_roots = [shader.roots[o][c] for o, c in prior['components']]
            _, current_graph = shader.position_graph()
            _, previous_graph = shader.position_graph(previous_roots)
            for label, graph in [('current', current_graph), ('previous', previous_graph)]:
                values = [key[1] for key in graph if key[0] == 'value']
                row[label] = shader.dependencies_values(values)
            row['prior_camera_rows'] = prior['camera_rows']
            def has_instance(label):
                return any(i[0] == 'INSTANCE_TRANSFORM' for i in row[label]['inputs'])
            row['current_instance_transform'] = has_instance('current')
            row['previous_instance_transform'] = has_instance('previous')
            patterns[str((row['current_instance_transform'], row['previous_instance_transform']))] += 1
        except (ValueError, KeyError, TypeError, AttributeError) as error:
            row['error'] = str(error)
        rows.append(row)
    result = dict(cache_sha256=contracts['cache_sha256'], total=len(rows),
                  parsed=sum('error' not in r for r in rows),
                  instance_transform_current_previous=dict(patterns),
                  errors=dict(Counter(r['error'] for r in rows if 'error' in r)),
                  game_attached=False, shaders=rows,
                  limitation='Actual data/control dependency only; not grouped draw or N-1 input validity')
    (p / 'native-instance-input-audit.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != 'shaders'}))


if __name__ == '__main__':
    main()
