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


def verify_camera_graft(original, grafted, r):
    """Camera-only variant: previous clip is the target's own current projection
    with its current camera rows replaced by the native previous camera rows.

    Also the generic variant (no native twin): a target that already subtracts
    b1[51] jitter exports its SV_Position unchanged as current clip, otherwise
    the current clip is SV_Position - b1[51].xy*W."""
    a,b=Shader(original),Shader(grafted)
    assert all(b.defs.get(v)==rhs for v,rhs in a.defs.items())
    assert all(b.roots[oid]==roots for oid,roots in a.roots.items())
    assert all(b.blocks.get(block)==edges for block,edges in a.blocks.items())
    current_id=next(i for i,f in a.outs.items() if f[1]=='!"SV_Position"')
    original_current=[a.roots[current_id][i] for i in range(4)]
    recorded_current=[b.roots[r['camera_current_output']][i] for i in range(4)]
    if r.get('coverage_guard'):
        roots,guard=a.uncollapsed_position()
        assert guard==r['coverage_guard'],'coverage guard changed'
    else:roots=original_current
    if r.get('camera_current_dejittered'):
        assert recorded_current==original_current,'de-jittered current clip is not the original SV_Position'
        roots=a.before_jitter_subtraction(roots)
    else:
        assert b.before_jitter_subtraction(recorded_current)==original_current,'incorrect current jitter convention'
        try:a.before_jitter_subtraction(roots)
        except ValueError:pass
        else:raise AssertionError('original already subtracts jitter; graft would subtract twice')
    load=re.compile(r'(@dx.op.cbufferLoadLegacy.\w+\(i32 59, %dx.types.Handle )(%\d+), i32 (\d+)\)')
    for v,rhs in b.defs.items():
        if v in a.defs:continue
        m=load.search(rhs)
        assert not m or b.handles[m[2]][2]!=7,'camera variant reads b7'
    current_rows,previous_rows=r['camera_current_rows'],r['camera_rows']
    def relocate(m):
        h=a.handles.get(m[2])
        if not h or h[0]!=2 or h[2]!=1 or int(m[3]) not in current_rows:return m[0]
        return m[1]+m[2]+', i32 '+str(previous_rows[current_rows.index(int(m[3]))])+')'
    # Reference: every value of the clip cone that depends on a current camera
    # row is copied beside its original (same block) with the rows renamed;
    # other values are shared. A coverage/cull branch condition that reads the
    # current camera stays the current frame's decision in both graphs.
    region={}
    def depends(v):
        if v in region:return region[v]
        if v not in a.defs:return False
        region[v]=False
        m=load.search(a.defs[v]);h=a.handles.get(m[2]) if m else None
        result=bool(h and h[0]==2 and h[2]==1 and int(m[3]) in current_rows) or \
            any([depends(x) for x in re.findall(r'%\d+\b',a.defs[v])])
        region[v]=result;return result
    for v in roots:depends(v)
    lines=[]
    copy=lambda v:'%graftRef'+v[1:] if region.get(v) else v
    for line in original.split('\n'):
        lines.append(line)
        d=re.match(r'  (%\d+) = (.*?)(?:\s+;.*)?$',line)
        if d and region.get(d[1]):
            rhs=load.sub(relocate,d[2])
            rhs=re.sub(r'%\d+\b',lambda m:copy(m[0]) if m[0] in a.defs else m[0],rhs)
            lines.append(f'  {copy(d[1])} = {rhs}')
    reference=Shader('\n'.join(lines))
    expected,_=reference.position_graph([copy(v) for v in roots])
    actual,_=b.position_graph([b.roots[r['camera_previous_output']][i] for i in range(4)])
    assert expected==actual,'camera previous differs from current projection with previous camera rows'
    return dict(camera_original_outputs_unchanged=True,camera_no_b7=True,
                camera_previous_is_current_world_with_previous_rows=True,camera_current_clip_convention_verified=True)
