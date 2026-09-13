"""Prepare a bounded native declaration table from verified offline grafts.

The output is a pending diagnostic artifact, never an automatic game deployment.
Shader read footprints do not prove native writer spans or per-instance history.
"""
from pathlib import Path
import argparse
import hashlib
import json
import mmap
import struct


def name_hash(name):
    value = 0xcbf29ce484222325
    for byte in name.encode('utf-8'):
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    args = parser.parse_args()
    p = args.workspace.resolve(strict=True)
    source = json.loads((p / 'shader-modifier-contracts.json').read_text())
    slots = json.loads((p / 'motion-slot-plan.json').read_text())
    if slots['identity']['cache'] != source['cache_sha256']:
        raise ValueError('shader/declaration audit cache mismatch')
    footprints = {r['key']: r for r in slots['keys']}
    index = json.loads((p / 'native-grafted/index.json').read_text())
    verification = json.loads((p / 'native-grafted/verification.json').read_text())
    verified = {r['sha256'] for r in verification['results']
                if r['original_outputs_unchanged'] and r['original_branches_unchanged']
                and r['native_previous_expression_identical'] and r.get('current_clip_convention_verified')}
    selected = [r for r in index['shaders'] if r['status'] == 'validated']
    if {r['sha256'] for r in selected} != verified:
        raise ValueError('graft and preservation results disagree')
    contracts = source['shaders']
    catalog = json.loads((p / 'all-cache-techniques.json').read_text())
    if catalog['cache_sha256'] != source['cache_sha256']:
        raise ValueError('technique/declaration cache mismatch')
    declarations = {}
    for shader in selected:
        if shader['required_engine_motion_rows'] != [24, 25, 26] or shader['required_material_bytes'] != 448:
            raise ValueError('unsupported graft supply layout')
        for alias in contracts[shader['sha256']]:
            key = alias['key']
            audit = footprints[key]
            if audit['problems'] or 24 not in audit['free_read_blocks'] or audit['existing_motion_rows']:
                raise ValueError('unresolved shared-stage slot: ' + key)
            if key in declarations and declarations[key] != alias:
                raise ValueError('ambiguous provider declaration: ' + key)
            declarations[key] = alias
    if not 0 < len(declarations) <= 256:
        raise ValueError('declaration capacity exceeded')
    plans, names = bytearray(), bytearray()
    report = []
    for key, row in sorted(declarations.items(), key=lambda item: int(item[0], 16)):
        if len(row['slots']) >= 32:
            raise ValueError('name count exceeds native adapter bound')
        offset = len(names) // 16
        plans.extend(struct.pack('<QIHBB', int(key, 16), row['request_mask'], offset, len(row['slots']), 24))
        for slot in row['slots']:
            if slot['name'] == 'MatMod_MotionMatrix':
                raise ValueError('duplicate native motion declaration')
            names.extend(struct.pack('<QB7x', name_hash(slot['name']), slot['row']))
        report.append(dict(key=key, names=len(row['slots']), row=24))
    if len(names) // 16 > 4096:
        raise ValueError('name storage capacity exceeded')
    out = p / 'pending-motion-declarations'
    out.mkdir(exist_ok=True)
    blob = struct.pack('<8sII', b'GMDPLAN1', len(declarations), len(names) // 16) + plans + names
    (out / 'declarations.bin').write_bytes(blob)
    # Preserve the exact binary-to-declaration relation. Equal shader bytes can
    # have different cache identities and metadata aliases.
    binary_metadata = {}
    with Path(catalog['cache_path']).open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if hashlib.sha256(data).hexdigest() != catalog['cache_sha256']:
            raise ValueError('cache file changed')
        count = struct.unpack_from('<I', data, len(data) - 112)[0]
        at = 0
        for _ in range(count):
            identity, parent, size = struct.unpack_from('<QQI', data, at)
            if identity in binary_metadata:
                raise ValueError('duplicate cache identity')
            binary_metadata[identity] = parent
            at += 20 + size
            if at > len(data) - 112:
                raise ValueError('invalid shader record extent')
    selected_hashes = {s['sha256'] for s in selected}
    pairs = {}
    paired_hashes, omitted = set(), []
    for technique in catalog['techniques']:
        if not technique['transparency_route']:
            continue
        programs = technique['programs']
        vs = [s for s in programs if s['kind'] == 'vs' and s['sha256'] in selected_hashes]
        ps = [s for s in programs if s['kind'] == 'ps']
        if len(vs) != 1 or len(ps) != 1 or len(programs) != 2:
            if vs:
                omitted.append(dict(vertex_sha256=vs[0]['sha256'], stage=technique['pass'],
                                    reason='not a VS/PS pair', kinds=[s['kind'] for s in programs]))
            continue
        vertex, partner = int(vs[0]['cache_identity'], 16), int(ps[0]['cache_identity'], 16)
        metadata = binary_metadata[vertex]
        if hex(metadata) not in declarations:
            raise ValueError('pair references an unconfigured declaration')
        pairs[vertex, partner] = metadata
        paired_hashes.add(vs[0]['sha256'])
    if not pairs or len(pairs) > 4096:
        raise ValueError('shader pair capacity exceeded')
    pair_blob = struct.pack('<8sII', b'GMSPAIR1', len(pairs), 0)
    pair_blob += b''.join(struct.pack('<QQQ', vertex, partner, metadata)
                          for (vertex, partner), metadata in sorted(pairs.items()))
    (out / 'shader-pairs.bin').write_bytes(pair_blob)
    result = dict(cache_sha256=source['cache_sha256'], binary_sha256=hashlib.sha256(blob).hexdigest(),
                  bytes=len(blob), declarations=len(declarations), names=len(names) // 16,
                  grafted_shaders=len(selected), shader_read_slots_verified=True,
                  native_writer_spans_verified=False, grouped_history_verified=False,
                  runtime_admitted=False, deployed=False, plans=report)
    result.update(shader_pairs=len(pairs), pair_bytes=len(pair_blob),
                  paired_vertex_shaders=len(paired_hashes), unpaired_vertex_shaders=sorted(selected_hashes-paired_hashes),
                  omitted_techniques=omitted,
                  pair_sha256=hashlib.sha256(pair_blob).hexdigest(),
                  pair_scope='Exact cache VS/PS identities; live stage binding and native writer admission pending')
    writer_path = p / 'motion-writer-slot-audit.json'
    if writer_path.exists():
        writer = json.loads(writer_path.read_text())
        source_path = p / 'native-writer-layouts.json'
        writer_tool = Path(__file__).with_name('audit_motion_writer_slots.py')
        if (writer['cache_sha256'] != result['cache_sha256'] or
                writer['pair_sha256'] != result['pair_sha256'] or
                not source_path.exists() or
                writer['source_sha256'] != hashlib.sha256(source_path.read_bytes()).hexdigest() or
                writer['tool_sha256'] != hashlib.sha256(writer_tool.read_bytes()).hexdigest()):
            # A changed export needs a new audit. Do not inherit a stale result.
            result['direct_writer_audit'] = dict(state='stale; rerun audit_motion_writer_slots.py')
        else:
            if writer['summary']['overlapping_pairs']:
                raise ValueError('native direct writer overlaps reserved motion storage')
            result['direct_writer_audit'] = dict(state='inspected direct spans only',
                                                **writer['summary'])
        # Recursive callee, cold constructor and live/history proofs remain
        # separate even when every inspected direct span is clear.
    (out / 'manifest.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k not in ('plans', 'omitted_techniques', 'unpaired_vertex_shaders')}))


if __name__ == '__main__':
    main()
