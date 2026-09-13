"""Join pending exact shader pairs to inspected native modifier write spans.

Consumes a local, executable-specific native-writer-layouts.json inspection.
This is an offline direct-write audit, not recursive native-call verification,
runtime admission, or a deployment tool. Unresolved suppliers remain explicit.
"""
import argparse
from collections import Counter
import hashlib
import json
import mmap
from pathlib import Path
import struct

from export_motion_declarations import name_hash


def audit_slots(mask, slots, layouts):
    by_hash = {}
    for slot in slots:
        key = name_hash(slot['name'])
        if key in by_hash and by_hash[key] != slot:
            raise ValueError('conflicting VS/PS modifier name or row')
        if not 0 <= slot['row'] <= 255:
            raise ValueError('invalid native row byte')
        by_hash[key] = slot
    writes, resources, unresolved, omitted = [], [], [], []
    for enum in range(32):
        if not mask & (1 << enum):
            continue
        layout = layouts.get(enum)
        if layout is None:
            unresolved.append(dict(enum=enum, reason='uninspected constructor/supplier'))
            continue
        fields = layout['fields']
        present = [by_hash.get(int(field['name_hash'], 16)) for field in fields]
        # 0xff is the original absent-name sentinel, including when explicitly
        # returned by a metadata slot. Creation gates differ by modifier.
        enabled = [slot is not None and slot['row'] != 255 for slot in present]
        gate = layout['constructor_presence_gate']
        if gate not in ('all', 'any') or not fields:
            raise ValueError('unknown constructor gate')
        if not (all(enabled) if gate == 'all' else any(enabled)):
            omitted.append(enum)
            continue
        if not layout['direct_spans_reviewed']:
            unresolved.append(dict(enum=enum, reason='; '.join(layout['unresolved'])))
            continue
        for field, slot, exists in zip(fields, present, enabled):
            if not exists:
                continue
            span = field['rows']
            if not isinstance(span, int) or span < 0:
                raise ValueError('invalid direct writer span')
            row = slot['row']
            if span == 0:
                resources.append(dict(enum=enum, name=slot['name'], binding=row))
                continue
            # These suppliers sign-extend the byte. Do not reinterpret a
            # negative sentinel/index as valid material constant storage.
            if row >= 128 or row + span > 28:
                unresolved.append(dict(enum=enum, name=slot['name'], reason='constant range outside 448-byte block'))
                continue
            writes.append(dict(enum=enum, name=slot['name'], rows=list(range(row, row + span))))
    occupied = sorted({row for write in writes for row in write['rows']})
    return dict(writes=writes, resources=resources, omitted_modifiers=omitted,
                unresolved=unresolved, occupied_rows=occupied,
                motion_overlap=sorted(set(occupied) & {24, 25, 26, 27}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    args = parser.parse_args()
    p = args.workspace.resolve(strict=True)
    source_path = p / 'native-writer-layouts.json'
    source_bytes = source_path.read_bytes()
    native = json.loads(source_bytes)
    contracts = json.loads((p / 'shader-modifier-contracts.json').read_text())
    catalog = json.loads((p / 'all-cache-techniques.json').read_text())
    pending = p / 'pending-motion-declarations'
    manifest = json.loads((pending / 'manifest.json').read_text())
    cache_hash = catalog['cache_sha256']
    if any(value != cache_hash for value in (native['cache_sha256'], contracts['cache_sha256'], manifest['cache_sha256'])):
        raise ValueError('inspection/cache identity mismatch')
    pair_blob = (pending / 'shader-pairs.bin').read_bytes()
    if hashlib.sha256(pair_blob).hexdigest() != manifest['pair_sha256']:
        raise ValueError('pending pair identity mismatch')
    magic, count, reserved = struct.unpack_from('<8sII', pair_blob)
    if magic != b'GMSPAIR1' or reserved or len(pair_blob) != 16 + count * 24 or count != manifest['shader_pairs']:
        raise ValueError('invalid pair extent')
    aliases = {}
    for entries in contracts['shaders'].values():
        for alias in entries:
            key = int(alias['key'], 16)
            if key in aliases and aliases[key] != alias:
                raise ValueError('inconsistent metadata alias')
            aliases[key] = alias
    parents = {}
    with Path(catalog['cache_path']).open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if hashlib.sha256(data).hexdigest() != cache_hash:
            raise ValueError('cache file changed')
        records = struct.unpack_from('<I', data, len(data) - 112)[0]
        at = 0
        for _ in range(records):
            identity, parent, size = struct.unpack_from('<QQI', data, at)
            if identity in parents or at + 20 + size > len(data) - 112:
                raise ValueError('invalid binary cache record')
            parents[identity] = parent
            at += 20 + size
    layouts = {item['enum']: item for item in native['constructors']}
    if len(layouts) != len(native['constructors']):
        raise ValueError('duplicate native modifier layout')
    rows, seen, memo = [], set(), {}
    for vertex, pixel, parent in struct.iter_unpack('<QQQ', pair_blob[16:]):
        if (vertex, pixel) in seen or parents[vertex] != parent:
            raise ValueError('duplicate pair or wrong vertex metadata')
        seen.add((vertex, pixel))
        key = parent, parents[pixel]
        if key not in memo:
            vs, ps = (aliases[item] for item in key)
            memo[key] = audit_slots(vs['request_mask'] | ps['request_mask'], vs['slots'] + ps['slots'], layouts)
        rows.append(dict(vertex=hex(vertex), pixel=hex(pixel), vertex_metadata=hex(parent),
                         pixel_metadata=hex(key[1]), **memo[key]))
    unknown = Counter(problem['enum'] for row in rows for problem in row['unresolved'])
    summary = dict(pairs=len(rows), distinct_metadata_pairs=len(memo),
                   direct_span_clear_pairs=sum(not row['unresolved'] and not row['motion_overlap'] for row in rows),
                   overlapping_pairs=sum(bool(row['motion_overlap']) for row in rows),
                   unresolved_pairs=sum(bool(row['unresolved']) for row in rows),
                   unresolved_enums=dict(sorted(unknown.items())),
                   native_writer_spans_verified=False, runtime_admitted=False)
    result = dict(summary=summary, exe_sha256=native['exe_sha256'], cache_sha256=cache_hash,
                  pair_sha256=manifest['pair_sha256'], source_sha256=hashlib.sha256(source_bytes).hexdigest(),
                  tool_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  scope='Exact pending VS/PS metadata union; inspected direct spans only. Indirect callees, cold name registration and runtime supply are not certified.',
                  pairs=rows)
    (p / 'motion-writer-slot-audit.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
