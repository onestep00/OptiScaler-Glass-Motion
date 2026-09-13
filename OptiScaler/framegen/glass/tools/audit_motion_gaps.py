"""Group unresolved native-motion routes by actual position inputs, offline.

No shader rewrite, process attachment or material-name admission. Shader-family
labels are reporting metadata; decisions use parsed position/control dependencies.
"""
from collections import Counter, defaultdict
import hashlib
import json
import re
from match_shared_native_motion import Shader, p


def main():
    catalog = json.loads((p / 'all-cache-techniques.json').read_text())
    contracts = json.loads((p / 'shader-modifier-contracts.json').read_text())
    if contracts['cache_sha256'] != catalog['cache_sha256']:
        raise ValueError('cache identity mismatch')
    matches = json.loads((p / 'shared-native-motion-matches.json').read_text())
    grafts = json.loads((p / 'native-grafted/index.json').read_text())
    valid = {r['sha256'] for r in grafts['shaders'] if r['status'] == 'validated'}
    candidates = {r['sha256'] for r in matches['matches']}
    routes = json.loads((p / 'all-transparent-input-routes.json').read_text())['shaders']
    families = defaultdict(set)
    for technique in catalog['techniques']:
        for program in technique['programs']:
            if program['kind'] == 'vs':
                families[program['sha256']].add(technique['vertex_factory'])
    rows = []
    for route in routes:
        sha = route['sha256']
        if sha in valid:
            continue
        row = dict(sha256=sha, families=sorted(families[sha]),
                   native_candidate=sha in candidates, runtime_admitted=False)
        text = (p / 'all-transparent-vs' / (sha + '.ll')).read_text()
        row['disassembly_sha256'] = hashlib.sha256(text.encode()).hexdigest()
        try:
            shader = Shader(text, contracts['shaders'].get(sha))
            try:
                geometry, _ = shader.uncollapsed_position()
                row['terminal_coverage_guard'] = True
            except ValueError:
                geometry = None
                row['terminal_coverage_guard'] = False
            _, graph = shader.position_graph(geometry)
            values = [key[1] for key in graph if key[0] == 'value']
            dependencies = shader.dependencies_values(values)
            row['position_and_control_inputs'] = dependencies
            camera_rows = set()
            for value in values:
                load = re.search(r'@dx.op.cbufferLoadLegacy.f32\(i32 59, %dx.types.Handle (%\d+), i32 (\d+)\)',
                                 shader.defs.get(value, ''))
                if load and shader.handles.get(load[1], ())[:1] == (2,) and shader.handles[load[1]][2] == 1:
                    camera_rows.add(int(load[2]))
            row['camera_f32_rows'] = sorted(camera_rows)
            row['camera_0_to_3'] = set(range(4)) <= camera_rows
            row['camera_28_to_31'] = set(range(28, 32)) <= camera_rows
            row['material_b4_rows'] = [r for b, r in dependencies['cb_rows'] if b == 4]
            row['global_b0_rows'] = [r for b, r in dependencies['cb_rows'] if b == 0]
            row['instance_transform_input'] = any(i[0] == 'INSTANCE_TRANSFORM' for i in dependencies['inputs'])
            row['skinning_input'] = any(i[0] == 'INSTANCE_SKINNING_DATA' for i in dependencies['inputs'])
            row['note'] = 'Dependency presence only; camera equivalence and previous input supply are unproven'
        except (ValueError, KeyError, StopIteration, TypeError, AttributeError) as error:
            row['parse_error'] = str(error)
        rows.append(row)
    parsed = [r for r in rows if 'parse_error' not in r]
    summary = dict(total=len(routes), grafted=len(valid), remaining=len(rows), parsed=len(parsed),
                   parse_errors=dict(Counter(r['parse_error'] for r in rows if 'parse_error' in r)),
                   native_candidate_but_graft_rejected=sum(r['native_candidate'] for r in rows),
                   camera_0_to_3=sum(r['camera_0_to_3'] for r in parsed),
                   camera_28_to_31=sum(r['camera_28_to_31'] for r in parsed),
                   material_b4_dependencies=sum(bool(r['material_b4_rows']) for r in parsed),
                   global_b0_dependencies=sum(bool(r['global_b0_rows']) for r in parsed),
                   instance_transform_inputs=sum(r['instance_transform_input'] for r in parsed),
                   skinning_inputs=sum(r['skinning_input'] for r in parsed),
                   position_resource_reads=sum(r['position_and_control_inputs']['resource_reads'] > 0 for r in parsed),
                   family_counts=dict(Counter(f for r in rows for f in r['families'])))
    result = dict(cache_sha256=catalog['cache_sha256'], summary=summary, shaders=rows,
                  shader_modified=False, game_attached=False,
                  scope='Base-cache candidate inventory, not complete runtime world-transparency coverage')
    (p / 'native-motion-gaps.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
