"""Build separate offline grafts requiring the original preskinning t9/b3 inputs.

Adds shader declarations only. Does not create, bind or invent engine history.
Candidates remain outside the pending runtime declaration export until the
original live resource and rendered-frame ownership are proved.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess

from graft_native_motion import graft, p
from match_shared_native_motion import Shader
from native_graft_checks import verify_graft


def add_preskinned_inputs(text):
    text = text.rstrip('\0\r\n') + '\n'
    shader = Shader(text)
    # These are the exact native resource contracts under investigation, not
    # permissive placeholders for arbitrary missing resources.
    contracts = ((0, 9, ('i32 0', 'i32 9', 'i32 1', 'i32 11', 'i32 0', 'null')),
                 (2, 3, ('i32 0', 'i32 3', 'i32 1', 'i32 176', 'null')))
    resource_root = re.search(r'!dx.resources = !\{!(\d+)\}', text)[1]
    lists = shader.md[resource_root].split(', ')
    # Control-flow hints use `distinct !{...}` and are absent from Shader.md.
    next_id = max(map(int, re.findall(r'^!(\d+) = ', text, re.M))) + 1
    definitions, handles, added = [], [], []
    for kind, binding, contract in contracts:
        existing = [value for (k, _), value in shader.resources.items()
                    if k == kind and value[0] == 'i32 0' and int(value[1][4:]) <= binding < int(value[1][4:]) + int(value[2][4:])]
        if existing:
            raise ValueError('preskinning input binding already declared; inspect instead of replacing')
        rid = max((rid for k, rid in shader.resources if k == kind), default=-1) + 1
        type_name = '%graftNativeRawInput' if kind == 0 else '%graftNativeMeshConstants'
        if type_name in text:
            raise ValueError('bridge type already exists')
        type_body = '{ i32 }' if kind == 0 else '{ [11 x <4 x float>] }'
        text = text.replace('define void @', type_name + ' = type ' + type_body + '\n\ndefine void @', 1)
        definition = next_id
        next_id += 1
        definitions.append(f'!{definition} = !{{i32 {rid}, {type_name}* undef, !"", ' + ', '.join(contract) + '}')
        if lists[kind] == 'null':
            lists[kind] = '!' + str(next_id)
            definitions.append(f'!{next_id} = !{{!{definition}}}')
            next_id += 1
        else:
            node = lists[kind][1:]
            text = text.replace(f'!{node} = !{{{shader.md[node]}}}', f'!{node} = !{{{shader.md[node]}, !{definition}}}')
        handles.append(f'  %graftNativeInput{kind} = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 {kind}, i32 {rid}, i32 {binding}, i1 false)')
        added.append(dict(kind=kind, binding=binding, space=0, contract=contract, engine_binding_verified=False))
    text = text.replace(f'!{resource_root} = !{{{shader.md[resource_root]}}}', f'!{resource_root} = !{{' + ', '.join(lists) + '}')
    # DXIL ShaderFlags bit 4 declares raw/structured-buffer use. The new native
    # t9 loads need it; copying unrelated native flags would alter other policy.
    entry = re.search(r'!dx.entryPoints = !\{!(\d+)\}', text)[1]
    entry_fields = shader.md[entry].split(', ')
    properties = entry_fields[-1]
    if properties == 'null':
        entry_fields[-1] = '!' + str(next_id)
        definitions.append(f'!{next_id} = !{{i32 0, i64 16}}')
        text = text.replace(f'!{entry} = !{{{shader.md[entry]}}}', f'!{entry} = !{{' + ', '.join(entry_fields) + '}')
    else:
        node = properties[1:]
        old = shader.md[node]
        updated, count = re.subn(r'(^|, )i32 0, i64 (\d+)(?=, |$)',
                                lambda m: m[1] + 'i32 0, i64 ' + str(int(m[2]) | 16), old)
        if count > 1:
            raise ValueError('duplicate shader flag property')
        if not count:
            updated = 'i32 0, i64 16, ' + old
        text = text.replace(f'!{node} = !{{{old}}}', f'!{node} = !{{{updated}}}')
    text, count = re.subn(r'(define void @[^\n]+\{\n)', lambda m: m[0] + '\n'.join(handles) + '\n', text, count=1)
    if count != 1:
        raise ValueError('unsupported shader entry declaration')
    return text + '\n' + '\n'.join(definitions) + '\n', added


def main():
    index = json.loads((p / 'native-grafted/index.json').read_text())
    missing = {r['sha256'] for r in index['shaders']
               if r['status'] == 'unsupported' and r['errors'] == ['missing native resource contract']}
    matches = json.loads((p / 'shared-native-motion-matches.json').read_text())['matches']
    contracts = json.loads((p / 'shader-modifier-contracts.json').read_text())['shaders']
    output = p / 'native-preskinned-grafted'
    output.mkdir(exist_ok=True)
    tool = p.parent / 'glass-native-material-buildcheck/GeometryShaderTool.exe'
    compiler = p.parent / 'glass-optiscaler-source/OptiScaler/shaders/shader_tools/dxcompiler.dll'
    rows = []
    for row in matches:
        if row['sha256'] not in missing:
            continue
        original = (p / 'all-transparent-vs' / (row['sha256'] + '.ll')).read_text()
        augmented, bindings = add_preskinned_inputs(original)
        errors = []
        for native in sorted(row['native_candidates'], key=lambda n: min(x['dependencies']['instructions'] for x in n['previous'])):
            try:
                native_text = (p / 'opaque-velocity-audit' / (native['sha256'] + '.ll')).read_text()
                generated, info = graft(augmented, native_text, native['previous'][0],
                    contracts.get(row['sha256']), contracts.get(native['sha256']),
                    native.get('specializations'), row.get('coverage_guard'))
                info.update(sha256=row['sha256'], native_sha256=native['sha256'],
                            specializations=native.get('specializations', {}))
                checks = verify_graft(original, generated, native_text, info, native, contracts)
                path = output / (row['sha256'] + '.ll')
                path.write_text(generated)
                run = subprocess.run([str(tool), str(compiler), 'assemble', str(path), str(path.with_suffix('.dxil')), '0'], capture_output=True, text=True)
                if run.returncode:
                    raise ValueError((run.stdout + run.stderr)[-1500:])
                rows.append(dict(status='offline_validated', required_native_inputs=bindings,
                    verification=checks, runtime_admitted=False, **info))
                break
            except (ValueError, KeyError, AssertionError, StopIteration) as error:
                errors.append(str(error))
        else:
            rows.append(dict(sha256=row['sha256'], status='unsupported', errors=sorted(set(errors))))
    summary = dict(candidates=len(rows), offline_validated=sum(r['status'] == 'offline_validated' for r in rows),
                   new_live_bindings=False, runtime_admitted=False, deployed=False)
    result = dict(summary=summary, tool_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), shaders=rows)
    (output / 'index.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
