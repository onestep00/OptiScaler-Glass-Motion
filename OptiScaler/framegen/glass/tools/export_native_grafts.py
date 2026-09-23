"""Export validated native grafts for the module's Glass/grafts catalog.

Reads <workspace>/native-grafted/index.json and verification.json and writes
<out>/grafts/index.bin plus one <sha256>.dxil per exported graft. index.bin is
"GGRAFT01", u32 count, u32 reserved (0), then count records sorted by sha:
{ u8 sha256[32]; u32 current_output; u32 previous_output; u32 supply_class }.
sha256 is the original VS DXBC container hash the module compares against.

A graft is exported only when its status is "validated", its verification row
has all four checks true and it requires engine motion rows [24, 25, 26].

supply_class comes from <workspace>/native-previous-supply-union.json, keyed by
the graft's native_sha256 (the original velocity VS whose previous-clip graph
the graft copied). The union's previous.dependencies and loads[] describe that
previous-clip graph only:
  bit 1 skinning   - previous inputs BLENDINDICES/BLENDWEIGHT/
                     INSTANCE_SKINNING_DATA/BONEINDEX, or a load from SRV t10
                     (bone buffer)
  bit 2 preskinned - a load from SRV t9 or constant buffer b3 (preskinned
                     previous vertices)
  bit 0 root-only  - neither: the previous graph reads the MotionMatrix and
                     camera rows plus the draw's own constants and vertex
                     attributes only
A graft without a union entry is refused.

The output contains extracted game shader code. Write it to an ignored local
directory;
never commit or publish it. GlassFg.props copies <repo>/artifacts/glass-grafts/
Glass/grafts beside the built DLL, so the usual call is
  --workspace ..\glass-native-material-v1 --out artifacts/glass-grafts/Glass
"""
from pathlib import Path
from collections import Counter
import argparse
import hashlib
import json
import shutil
import struct

CHECKS = ('original_outputs_unchanged', 'original_branches_unchanged',
          'native_previous_expression_identical', 'current_clip_convention_verified')
ROWS = [24, 25, 26]
SKINNING_INPUTS = {'BLENDINDICES', 'BLENDWEIGHT', 'INSTANCE_SKINNING_DATA', 'BONEINDEX'}
SRV, CBV = 0, 2
CLASS_NAMES = {1: 'root-only', 2: 'skinning', 4: 'preskinned', 6: 'skinning+preskinned'}


def supply_class(entry):
    dependencies = entry['previous']['dependencies']
    loads = [(load['binding'][0], load['binding'][2]) for load in entry['loads']]
    skinning = (any(str(name).upper() in SKINNING_INPUTS for name, *_ in dependencies['inputs'])
                or (SRV, 10) in loads)
    preskinned = (SRV, 9) in loads or (CBV, 3) in loads or any(row[0] == 3 for row in dependencies['cb_rows'])
    value = (2 if skinning else 0) | (4 if preskinned else 0)
    return value or 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    workspace = args.workspace.resolve(strict=True)
    grafted = workspace / 'native-grafted'
    index = json.loads((grafted / 'index.json').read_text(encoding='utf-8'))
    verification = {row['sha256']: row for row in
                    json.loads((grafted / 'verification.json').read_text(encoding='utf-8'))['results']}
    union = {row['sha256']: row for row in
             json.loads((workspace / 'native-previous-supply-union.json').read_text(encoding='utf-8'))['shaders']}

    refused = Counter()
    records = []
    for shader in index['shaders']:
        sha = shader['sha256']
        if shader['status'] != 'validated':
            refused['status'] += 1
            continue
        row = verification.get(sha)
        if not row or not all(row.get(check) is True for check in CHECKS):
            refused['verification'] += 1
            continue
        if shader['required_engine_motion_rows'] != ROWS:
            refused['rows'] += 1
            continue
        current, previous = int(shader['current_output']), int(shader['previous_output'])
        if current == previous or not (0 <= current < 32 and 0 <= previous < 32):
            refused['outputs'] += 1
            continue
        native = union.get(shader['native_sha256'])
        if native is None:
            refused['supply-union'] += 1
            continue
        source = grafted / f'{sha}.dxil'
        data = source.read_bytes()
        if data[:4] != b'DXBC':
            refused['container'] += 1
            continue
        records.append((bytes.fromhex(sha), current, previous, supply_class(native), source))

    records.sort(key=lambda record: record[0])
    if len({record[0] for record in records}) != len(records):
        raise ValueError('duplicate graft sha256')
    out = args.out.resolve() / 'grafts'
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    blob = bytearray(struct.pack('<8sII', b'GGRAFT01', len(records), 0))
    for digest, current, previous, supply, source in records:
        blob += struct.pack('<32sIII', digest, current, previous, supply)
        shutil.copyfile(source, out / f'{digest.hex()}.dxil')
    (out / 'index.bin').write_bytes(bytes(blob))

    histogram = Counter(record[3] for record in records)
    print(f'grafts={len(records)} index_bytes={len(blob)} out={out}')
    print('refused=' + (', '.join(f'{k}:{v}' for k, v in sorted(refused.items())) or 'none'))
    for value in sorted(histogram):
        print(f'class {value} ({CLASS_NAMES.get(value, "?")}): {histogram[value]}')
    print(f'index_sha256={hashlib.sha256(bytes(blob)).hexdigest()}')


if __name__ == '__main__':
    main()
