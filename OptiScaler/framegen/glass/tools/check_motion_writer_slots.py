"""Owned negative checks for native write-range admission (no game access)."""
import json
from audit_motion_writer_slots import audit_slots
from export_motion_declarations import name_hash


def main():
    def field(name, span):
        return dict(name_hash=hex(name_hash(name)), rows=span)

    def layout(gate, *fields):
        return dict(constructor_presence_gate=gate, fields=fields,
                    direct_spans_reviewed=True, unresolved=[])

    layouts = {1: layout('any', field('matrix', 4)),
               2: layout('all', field('resource', 0), field('constant', 1))}

    def audit(mask, **slots):
        return audit_slots(mask, [dict(name=k, row=v) for k, v in slots.items()], layouts)

    assert audit(2, matrix=21)['motion_overlap'] == [24]
    assert audit(2, matrix=24)['motion_overlap'] == [24, 25, 26, 27]
    assert audit(2, matrix=25)['unresolved']
    assert audit(2, matrix=128)['unresolved']
    assert audit(2, matrix=255)['omitted_modifiers'] == [1]
    assert audit(4, resource=77, constant=3)['occupied_rows'] == [3]
    assert audit(4, constant=24)['omitted_modifiers'] == [2]
    assert not audit(0, matrix=24)['writes']
    assert audit(1)['unresolved'][0]['enum'] == 0
    layouts[0] = dict(constructor_presence_gate='any', fields=[field('virtual', None)],
                      direct_spans_reviewed=False, unresolved=['indirect target'])
    assert audit(1, virtual=2)['unresolved'][0]['reason'] == 'indirect target'
    assert audit_slots(2, [dict(name='matrix', row=1)] * 2, layouts)['occupied_rows'] == [1, 2, 3, 4]
    try:
        audit_slots(2, [dict(name='matrix', row=1), dict(name='matrix', row=2)], layouts)
    except ValueError:
        pass
    else:
        raise AssertionError('conflicting stage rows were accepted')
    print(json.dumps(dict(checks=12, passed=True, runtime_admitted=False)))


if __name__ == '__main__':
    main()
