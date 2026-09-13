"""Shared original-output and native-prior graph checks for offline grafts."""
import re
from match_shared_native_motion import Shader, modifier_key


def verify_graft(original, grafted, native, r, route, contracts):
    a,b=Shader(original),Shader(grafted)
    # Original definitions are unchanged, and each original store still exports
    # the very same SSA value. Metadata changes only add the required usage.
    assert all(b.defs.get(v)==rhs for v,rhs in a.defs.items())
    assert all(b.roots[oid]==roots for oid,roots in a.roots.items())
    assert all(b.blocks.get(block)==edges for block,edges in a.blocks.items())
    if r.get('coverage_guard'):
        _,guard=a.uncollapsed_position()
        assert guard==r['coverage_guard']
    current_id=next(i for i,f in a.outs.items() if f[1]=='!"SV_Position"')
    original_current=[a.roots[current_id][i] for i in range(4)]
    recorded_current=[b.roots[r['current_output']][i] for i in range(4)]
    assert b.before_jitter_subtraction(recorded_current)==original_current,'incorrect current jitter convention'
    geometry=a.uncollapsed_position()[0] if r.get('coverage_guard') else None
    try:a.before_jitter_subtraction(geometry)
    except ValueError:pass
    else:raise AssertionError('original already subtracts jitter; graft would subtract twice')
    src=Shader(native)
    handles={v for v,h in src.handles.items() if h[0]==2 and h[2]==7}
    def relocate(m):
        if m[2] not in handles:return m[0]
        row=int(m[3])
        if row in r['original_motion_rows']:dest=24+row-r['original_motion_rows'][0]
        else:
            key=modifier_key(contracts.get(r['native_sha256']),row)
            candidates=[i for i in range(28) if modifier_key(contracts.get(r['sha256']),i)==key]
            if len(candidates)!=1:return m[0]
            dest=candidates[0]
        return m[1]+m[2]+', i32 '+str(dest)+')'
    native=re.sub(r'(@dx.op.cbufferLoadLegacy.\w+\(i32 59, %dx.types.Handle )(%\d+), i32 (\d+)\)',relocate,native)
    reference=Shader(native,specializations=r.get('specializations'))
    components=route['previous'][0]['components']
    if r.get('graft_mode')=='native-control-flow':b.entry_block='graftBlock0'
    reference.position_graph([reference.roots[oid][col] for oid,col in components])
    b.position_graph([b.roots[r['previous_output']][i] for i in range(4)])
    ar,ag=reference.graph_data;br,bg=b.graph_data
    pending=list(zip(ar,br));seen=set()
    while pending:
        pair=pending.pop()
        if pair in seen:continue
        seen.add(pair);x,y=ag[pair[0]],bg[pair[1]]
        assert x[0]==y[0] and len(x[1])==len(y[1]), (r['sha256'],x[0],y[0])
        pending.extend(zip(x[1],y[1]))
        assert len(seen)<=100000,'expression comparison bound exceeded'
    return dict(sha256=r['sha256'], original_outputs_unchanged=True,
                original_branches_unchanged=True, native_previous_expression_identical=True,
                current_clip_convention_verified=True)
