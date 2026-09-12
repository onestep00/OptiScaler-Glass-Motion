"""Audit shared shader declarations before extending the native b7 motion supply.

Offline only. Free shader-read space is not proof of native writer bounds,
proxy history, stage linkage, or runtime admission.
"""
from pathlib import Path
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import mmap
import re
import struct
import subprocess


def footprint(text):
    handles = {}
    for match in re.finditer(
            r'(%[\w.]+) = call %dx.types.Handle @dx.op.createHandle\('
            r'i32 57, i8 (\d+), i32 \d+, i32 (\d+), i1 (?:true|false)\)', text):
        handles[match[1]] = (int(match[2]), int(match[3]))
    rows, problems = set(), set()
    for line in text.splitlines():
        if '= call' not in line or '@dx.op.cbufferLoad' not in line:
            continue
        match = re.search(r'%dx.types.Handle (%[\w.]+), i32 ([^,)]+)', line)
        if not match or match[1] not in handles:
            problems.add('unresolved constant handle')
            continue
        if handles[match[1]] != (2, 7):
            continue
        if '@dx.op.cbufferLoadLegacy.' not in line or not match[2].isdigit():
            problems.add('non-static or non-legacy b7 load')
            continue
        row = int(match[2])
        if row >= 28:
            problems.add('b7 read exceeds known 448-byte uploader')
        rows.add(row)
    # An unrecognized handle creation scheme must not silently become no reads.
    if re.search(r'@dx.op.createHandle(?:From|For|\.)', text):
        problems.add('unsupported handle creation scheme')
    return dict(rows=sorted(rows), problems=sorted(problems))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--dxc', type=Path)
    parser.add_argument('--workers', type=int, default=8)
    args = parser.parse_args()
    p = args.workspace.resolve(strict=True)
    dxc = (args.dxc or p.parent / 'glass-dxc/bin/x64/dxc.exe').resolve(strict=True)
    catalog = json.loads((p / 'all-cache-techniques.json').read_text())
    contracts = json.loads((p / 'shader-modifier-contracts.json').read_text())
    if contracts['cache_sha256'] != catalog['cache_sha256']:
        raise ValueError('modifier and technique cache identities differ')
    shaders = contracts['shaders']
    targets = json.loads((p / 'all-transparent-input-routes.json').read_text())['shaders']
    keys = {r['key'] for t in targets for r in shaders[t['sha256']]}
    key_shaders = defaultdict(set)
    for sha, aliases in shaders.items():
        for alias in aliases:
            key_shaders[alias['key']].add(sha)
    affected = set().union(*(key_shaders[key] for key in keys))
    linked = [t for t in catalog['techniques']
              if any(s['sha256'] in affected for s in t['programs'])]
    programs = {s['sha256']: s for t in linked for s in t['programs']}
    output = p / 'motion-slot-audit'
    output.mkdir(exist_ok=True)
    # Cache analyses by shader content, compiler content, and parser source.
    identity = dict(cache=catalog['cache_sha256'],
                    compiler=hashlib.sha256(dxc.read_bytes()).hexdigest(),
                    parser=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    cache_file = output / 'footprints.json'
    saved = json.loads(cache_file.read_text()) if cache_file.exists() else {}
    results = saved.get('shaders', {}) if saved.get('identity') == identity else {}
    missing = set(programs) - results.keys()
    identities = {int(r['cache_identity'], 16): sha for sha, r in programs.items() if sha in missing}
    with Path(catalog['cache_path']).open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if hashlib.sha256(data).hexdigest() != catalog['cache_sha256']:
            raise ValueError('original shader cache changed')
        footer = struct.unpack_from('<8I9Q2I', data, len(data) - 112)
        at = 0
        for _ in range(footer[0]):
            key, _, size = struct.unpack_from('<QQI', data, at)
            at += 20
            if key in identities:
                sha = identities[key]
                blob = data[at:at + size]
                if hashlib.sha256(blob).hexdigest() != sha:
                    raise ValueError('shader binary identity mismatch')
                (output / (sha + '.dxbc')).write_bytes(blob)
            at += size

    def inspect(sha):
        binary = output / (sha + '.dxbc')
        result = subprocess.run([str(dxc), '-dumpbin', str(binary)], capture_output=True, timeout=60)
        if result.returncode:
            return sha, dict(rows=[], problems=['DXC disassembly failed'], error=result.stderr.decode(errors='replace')[-500:])
        text = result.stdout.decode('utf-8')
        (output / (sha + '.ll')).write_text(text, encoding='utf-8')
        return sha, footprint(text)

    with ThreadPoolExecutor(max_workers=max(1, min(16, args.workers))) as pool:
        for sha, result in pool.map(inspect, sorted(missing)):
            results[sha] = result
    cache_file.write_text(json.dumps(dict(identity=identity, shaders=results), indent=2))
    # A provider key can be shared by unrelated shaders. Include every stage of
    # every technique referencing any alias, not only the transparent VS/PS pair.
    contexts = defaultdict(list)
    for index, technique in enumerate(linked):
        technique_keys = {r['key'] for s in technique['programs'] for r in shaders[s['sha256']]}
        for key in technique_keys & keys:
            contexts[key].append(index)
    plans = []
    for key in sorted(keys):
        techniques = [linked[i] for i in contexts[key]]
        partners = {s['sha256'] for t in techniques for s in t['programs']}
        used = set().union(*(set(results[sha]['rows']) for sha in partners))
        problems = {problem for sha in partners for problem in results[sha]['problems']}
        declarations = [r for sha in partners for r in shaders[sha]]
        slots = [s for r in declarations for s in r['slots']]
        named = {s['row'] for s in slots}
        motion_rows = {s['row'] for s in slots if s['name'] == 'MatMod_MotionMatrix'}
        # Four output rows are written by the native motion supplier. Existing
        # declared motion blocks also reserve all four rows, not just read rows.
        reserved = used | named | {row + i for row in motion_rows for i in range(4)}
        free = [row for row in range(25) if not reserved.intersection(range(row, row + 4))]
        # Only tail candidates avoid crossing a later, incompletely sized named
        # modifier. Native producer write extents still need separate proof.
        tail = [row for row in free if row > max(named, default=-1)]
        own = next(r for sha in key_shaders[key] for r in shaders[sha] if r['key'] == key)
        own_motion = [s['row'] for s in own['slots'] if s['name'] == 'MatMod_MotionMatrix']
        plans.append(dict(key=key, shader_aliases=len(key_shaders[key]), techniques=len(techniques),
                          partner_shaders=len(partners), read_rows=sorted(used), declared_rows=sorted(named),
                          existing_motion_rows=sorted(motion_rows), own_motion_rows=own_motion,
                          free_read_blocks=free if not problems else [],
                          tail_candidates=tail if not problems else [], problems=sorted(problems),
                          native_writer_bounds_verified=False, runtime_admitted=False))
    summary = dict(transparent_vs=len(targets), metadata_keys=len(keys),
                   affected_shader_aliases=len(affected), joined_techniques=len(linked),
                   inspected_shaders=len(programs), newly_disassembled=len(missing),
                   shader_problems=dict(Counter(x for sha in programs for x in results[sha]['problems'])),
                   keys_with_read_free_tail=sum(bool(r['tail_candidates']) for r in plans),
                   keys_with_own_motion=sum(bool(r['own_motion_rows']) for r in plans),
                   keys_with_read_conflicts_at_24=sum(bool(set(r['read_rows']) & {24, 25, 26, 27}) for r in plans),
                   runtime_admitted=False)
    (p / 'motion-slot-plan.json').write_text(json.dumps(dict(identity=identity, summary=summary, keys=plans), indent=2))
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
