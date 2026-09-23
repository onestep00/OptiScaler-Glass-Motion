"""Export validated native grafts for the module's Glass/grafts catalog.

Reads <workspace>/native-grafted/index.json and verification.json and writes
<out>/grafts/index.bin plus one <sha256>.dxil per exported root graft and, when
its camera-only variant is verified, <sha256>.camera.dxil. index.bin is
"GGRAFT02", u32 count, u32 reserved (0), then count 52-byte records sorted by sha:
{ u8 sha256[32]; u32 current_output; u32 previous_output; u32 supply_class;
  u32 camera_current_output; u32 camera_previous_output }.
sha256 is the original VS DXBC container hash the module compares against.
The camera outputs are 0xFFFFFFFF when the camera variant is not exported; the
root outputs are 0xFFFFFFFF for a camera-only record (no <sha256>.dxil).

The camera-only variant applies the native previous camera rows to the target's
own current world position; it reads no b7 row. It is exported when
camera_status is "validated" and its camera verification row has all checks
true, beside its root graft or alone:
  - a matched shader whose root graft is not exported (status "unsupported",
    or "factory_mismatch": a skinned-factory target whose root-only twins of
    another vertex factory graft_native_motion.py refused and whose other
    twins did not validate);
  - a generic_camera shader (no native current-position twin: the canonical
    native previous view-projection multiply on its own world position).
A camera-only record's supply_class is the target's own current position class
(camera_supply_class), since no previous graph is copied from a native VS.

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
A root graft without a union entry is refused.

--skinned-previous-world current reads <workspace>/native-grafted-skinned-current
(graft_native_motion.py and verify_native_grafts.py with the same flag). There a
class-2 root graft may carry previous_world "current": it reads the target's own
current INSTANCE_TRANSFORM rows instead of the MotionMatrix and is exported with
required engine motion rows [] instead of [24, 25, 26]; its supply class must be
2. All other records follow the rules above.

The output contains extracted game shader code. Write it to an ignored local
directory;
never commit or publish it. GlassFg.props copies <repo>/artifacts/glass-grafts/
Glass/grafts beside the built DLL, so the usual call is
  --workspace ..\\glass-native-material-v1 --out artifacts/glass-grafts/Glass
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
CAMERA_CHECKS = ('camera_original_outputs_unchanged', 'camera_no_b7',
                 'camera_previous_is_current_world_with_previous_rows',
                 'camera_current_clip_convention_verified')
ROWS = [24, 25, 26]
NO_OUTPUT = 0xFFFFFFFF
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


def camera_outputs(shader, verified, source, refused):
    """(current, previous, dxil path) of a verified camera-only variant, else None."""
    if shader.get('camera_status') != 'validated':
        refused['status'] += 1
        return None
    if not verified or not all(verified.get(check) is True for check in CAMERA_CHECKS):
        refused['verification'] += 1
        return None
    current, previous = int(shader['camera_current_output']), int(shader['camera_previous_output'])
    if current == previous or not (0 <= current < 32 and 0 <= previous < 32):
        refused['outputs'] += 1
        return None
    if source.read_bytes()[:4] != b'DXBC':
        refused['container'] += 1
        return None
    return current, previous, source


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--skinned-previous-world', choices=('motion', 'current'), default='motion',
                        help='current exports <workspace>/native-grafted-skinned-current')
    args = parser.parse_args()
    workspace = args.workspace.resolve(strict=True)
    variant = args.skinned_previous_world == 'current'
    grafted = workspace / ('native-grafted-skinned-current' if variant else 'native-grafted')
    index = json.loads((grafted / 'index.json').read_text(encoding='utf-8'))
    checked = json.loads((grafted / 'verification.json').read_text(encoding='utf-8'))
    verification = {row['sha256']: row for row in checked['results']}
    camera_verification = {row['sha256']: row for row in checked.get('camera_results', [])}
    generic_verification = {row['sha256']: row for row in checked.get('generic_results', [])}
    union = {row['sha256']: row for row in
             json.loads((workspace / 'native-previous-supply-union.json').read_text(encoding='utf-8'))['shaders']}

    def root_outputs(shader):
        """(current, previous, supply class, dxil path) of an exportable root graft, else None."""
        sha = shader['sha256']
        if shader['status'] != 'validated':
            refused[shader['status']] += 1
            return None
        row = verification.get(sha)
        if not row or not all(row.get(check) is True for check in CHECKS):
            refused['verification'] += 1
            return None
        current_world = variant and shader.get('previous_world') == 'current'
        if shader['required_engine_motion_rows'] != ([] if current_world else ROWS):
            refused['rows'] += 1
            return None
        current, previous = int(shader['current_output']), int(shader['previous_output'])
        if current == previous or not (0 <= current < 32 and 0 <= previous < 32):
            refused['outputs'] += 1
            return None
        native = union.get(shader['native_sha256'])
        if native is None:
            refused['supply-union'] += 1
            return None
        if current_world and supply_class(native) != 2:
            refused['previous-world class'] += 1
            return None
        source = grafted / f'{sha}.dxil'
        if source.read_bytes()[:4] != b'DXBC':
            refused['container'] += 1
            return None
        return current, previous, supply_class(native), source

    index_rows = {}
    refused = Counter()
    camera_refused = Counter()
    records = []
    kinds = Counter()
    for shader in index['shaders']:
        sha = shader['sha256']
        index_rows[sha] = shader.get('camera_rows')
        root = root_outputs(shader)
        camera = camera_outputs(shader, camera_verification.get(sha), grafted / f'{sha}.camera.dxil', camera_refused)
        if root:
            records.append((bytes.fromhex(sha), *root, camera))
            kinds['root+camera' if camera else 'root only'] += 1
            if variant and shader.get('previous_world') == 'current':
                kinds['root with current previous world'] += 1
        elif camera:
            records.append((bytes.fromhex(sha), NO_OUTPUT, NO_OUTPUT, int(shader['camera_supply_class']), None, camera))
            kinds['camera only (native twin, root not exported)'] += 1
    for shader in index.get('generic_camera', []):
        sha = shader['sha256']
        index_rows[sha] = shader.get('camera_rows')
        camera = camera_outputs(shader, generic_verification.get(sha), grafted / f'{sha}.camera.dxil', camera_refused)
        if camera:
            records.append((bytes.fromhex(sha), NO_OUTPUT, NO_OUTPUT, int(shader['camera_supply_class']), None, camera))
            kinds['camera only (generic template)'] += 1

    records.sort(key=lambda record: record[0])
    if len({record[0] for record in records}) != len(records):
        raise ValueError('duplicate graft sha256')
    out = args.out.resolve() / 'grafts'
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    blob = bytearray(struct.pack('<8sII', b'GGRAFT02', len(records), 0))
    for digest, current, previous, supply, source, camera in records:
        camera_current, camera_previous, camera_source = camera or (NO_OUTPUT, NO_OUTPUT, None)
        blob += struct.pack('<32sIIIII', digest, current, previous, supply, camera_current, camera_previous)
        if source:
            shutil.copyfile(source, out / f'{digest.hex()}.dxil')
        if camera_source:
            shutil.copyfile(camera_source, out / f'{digest.hex()}.camera.dxil')
    (out / 'index.bin').write_bytes(bytes(blob))

    histogram = Counter(record[3] for record in records)
    print(f'grafts={len(records)} index_bytes={len(blob)} out={out}')
    for kind, count in sorted(kinds.items()):
        print(f'{kind}: {count}')
    print('root refused=' + (', '.join(f'{k}:{v}' for k, v in sorted(refused.items())) or 'none'))
    for value in sorted(histogram):
        print(f'class {value} ({CLASS_NAMES.get(value, "?")}): {histogram[value]}')
    print('camera refused=' + (', '.join(f'{k}:{v}' for k, v in sorted(camera_refused.items())) or 'none'))
    rows = Counter(str(index_rows[record[0].hex()]) for record in records if record[5])
    for value in sorted(rows):
        print(f'camera rows {value}: {rows[value]}')
    print(f'index_sha256={hashlib.sha256(bytes(blob)).hexdigest()}')


if __name__ == '__main__':
    main()
