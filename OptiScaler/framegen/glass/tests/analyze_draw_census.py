"""Summarize original draw observations; never infer image masks or object motion."""
import argparse
import collections
import csv
import json
from pathlib import Path
import struct


def analyze(folder: Path):
    metadata = dict(line.split('=', 1) for line in (folder / 'draw-census.done').read_text().splitlines() if '=' in line)
    if metadata.get('format') != '1' or metadata.get('target_record_bytes') != '160':
        raise ValueError('Unsupported census format')
    with (folder / 'draw-census.csv').open(newline='') as file:
        rows = list(csv.DictReader(file))
    if len(rows) != int(metadata['rows']):
        raise ValueError('CSV row count differs from completion record')
    targets = (folder / 'draw-census.targets.bin').read_bytes()
    if len(targets) != len(rows) * 9 * 160:
        raise ValueError('Target binary row count differs from CSV')
    groups = collections.defaultdict(lambda: {'calls': 0, 'mapped_calls': 0, 'unknown_frame_calls': 0, 'samples': []})
    identities = {}
    operations = collections.Counter()
    for index, row in enumerate(rows):
        sequence = int(row['sequence'])
        if sequence in identities:
            raise ValueError('Duplicate CPU observation sequence')
        identities[sequence] = row
        operations[int(row['operation'])] += 1
        views = []
        for binding in range(9):
            record = memoryview(targets)[(index * 9 + binding) * 160: (index * 9 + binding + 1) * 160]
            size, kind, default, null = struct.unpack_from('<4I', record)
            if not size:
                if int(row[f'target_{binding}_resource']) or int(row[f'target_{binding}_revision']):
                    raise ValueError('CSV refers to a missing target record')
                continue
            if size != 160:
                raise ValueError('Unsupported target POD size')
            handle, heap, revision, resource, address = struct.unpack_from('<5Q', record, 16)
            allocation_bytes, descriptor_bytes = struct.unpack_from('<2I', record, 56)
            if resource != int(row[f'target_{binding}_resource']) or revision != int(row[f'target_{binding}_revision']):
                raise ValueError('Target identity differs between binary and CSV')
            # D3D12_RESOURCE_DESC byte layout in the declared Windows x64 POD.
            if allocation_bytes not in (0, 56) or descriptor_bytes > 32:
                raise ValueError('Unsupported allocation/view descriptor layout')
            width, height = struct.unpack_from('<QI', record, 80) if allocation_bytes else (0, 0)
            views.append({'binding': binding, 'kind': kind, 'resource': resource, 'revision': revision,
                          'width': width, 'height': height, 'default': bool(default), 'null': bool(null),
                          'descriptor_words': list(struct.unpack_from('<8I', record, 128))})
        row['_views'] = views
        key = (int(row['operation']), int(row['pipeline_identity']), int(row['pipeline_address']),
               int(row['mesh']), int(row['chunk']), int(row['instances']),
               tuple((view['binding'], view['resource']) for view in views))
        group = groups[key]
        group.update(operation=key[0], pipeline_identity=key[1], pipeline_address=key[2], mesh=key[3], chunk=key[4],
                     instances=key[5], targets=views)
        group['calls'] += 1
        group['mapped_calls'] += int(row['object_entries']) > 0
        group['unknown_frame_calls'] += int(row['frame']) == 0
        if len(group['samples']) < 3:
            group['samples'].append(sequence)
    summary = {'rows': len(rows), 'operations': dict(operations),
               'contended': int(metadata['contended']), 'overflow': int(metadata['overflow']),
               'cpu_storage_bytes': int(metadata['cpu_storage_bytes']),
               'unmapped_calls': sum(int(row['object_entries']) == 0 for row in rows),
               'untracked_recordings': sum(int(row['recording']) == 0 for row in rows),
               'missing_capture_pipeline': sum(int(row['pipeline_identity']) == 0 for row in rows),
               'object_entries_not_saved': sum(int(row['object_entries']) - int(row['object_entries_saved']) for row in rows),
               'groups': sorted(groups.values(), key=lambda group: -group['calls']),
               'object_motion_produced': False, 'order': 'CPU observations, not GPU execution'}
    return summary, identities


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--sequence', type=int)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--binding', type=int, default=0, choices=range(9))
    parser.add_argument('--proxy', type=int, default=0)
    args = parser.parse_args()
    summary, rows = analyze(args.folder)
    if args.sequence is not None:
        row = rows[args.sequence]
        if not args.pid or int(row['operation']) != 1 or not int(row['pipeline_identity']) or not int(row['mesh']) or \
                not int(row['root_replayable']) or not int(row['raster_usable']) or not int(row['recording']) or \
                not 1 <= int(row['instances']) <= 32 or not int(row[f'target_{args.binding}_resource']):
            raise ValueError('Selected observation cannot provide a supported capture selector')
        if args.proxy:
            with (args.folder / 'draw-census.objects.csv').open(newline='') as file:
                known = any(int(obj['sequence']) == args.sequence and int(obj['proxy']) == args.proxy and
                            obj['mesh'] == row['mesh'] and int(obj['generation']) for obj in csv.DictReader(file))
            if not known:
                raise ValueError('Requested proxy is not a verified saved object entry for this observation')
        values = [args.pid, row['pipeline_identity'], args.binding, row[f'target_{args.binding}_resource'], row['mesh'],
                  row['chunk'], row['indices_or_vertices'], row['instances'], row['start_index_or_vertex'],
                  row['base_vertex'], row['start_instance'], args.proxy]
        summary['selector'] = 'select-v1 ' + ' '.join(map(str, values))
    serialized = json.dumps(summary, indent=2)
    if args.output:
        args.output.write_text(serialized + '\n', encoding='utf-8')
    else:
        print(serialized)


if __name__ == '__main__':
    main()
