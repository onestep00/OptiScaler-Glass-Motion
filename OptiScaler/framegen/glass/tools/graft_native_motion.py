"""Transplant native previous-position DAGs into matching transparent vertex shaders.

Preserves original outputs. Shares identical current-position subexpressions.
Only b7 motion rows are relocated; no new motion/deformation arithmetic is invented.
Generated shaders remain offline candidates until their live b7 supply is admitted.

Each match also gets a camera-only variant (<sha>.camera.ll/.dxil) for instanced
array draws: the native previous camera multiply (b1 rows 16..19 or 12..15)
copied node for node onto the target's own current world position. It reads no
b7 row and needs no module state.

Every other transparent VS (no native current-position twin) gets only the
camera-only variant (index.json generic_camera): the canonical native template's
previous camera multiply on the target's own world position, one template per
camera layout. Targets whose clip is not a per-vertex view-projection multiply
of a world position are recorded as unsupported with the reason.

Twin selection is factory-consistent. A target whose vertex factories
(all-cache-techniques.json) include a skinned one (MeshSkinned, MeshExtSkinned,
Garment*, SingleBone, SkinnedVehicle, DestructibleSkinned, LightBlockers) never
takes its root graft from a twin of another vertex factory whose previous graph
is root-only (class 1: MotionMatrix, camera rows and vertex attributes only).
Such a twin matches when the target's current position collapses to a rigid
path; its previous graph would supply the proxy root transform instead of
per-vertex deformation. A twin sharing a vertex factory with the target is the
engine's own velocity route for that factory and stays eligible even when it is
root-only, as do twins reading skinning inputs/t10 (class 2) or preskinned t9/b3
history (class 4). If none validates, the root status is factory_mismatch and
only the camera-only variant is written.

--skinned-previous-world current (default motion) writes an experimental catalog
to <workspace>/native-grafted-skinned-current instead of native-grafted. A root
graft whose twin's previous graph is class 2 (skinning inputs or t10 bones) then
reads the target's own current world transform instead of the engine
MotionMatrix: every use of MotionMatrix row start+k (b7, relocated to 24..26 by
default) reads INSTANCE_TRANSFORM k, the input row its current position reads.
The previous translation chain of each row (w bitcast, b1 row 38 subtraction when
present, sitofp, 2^-17) must equal the current path's chain. Previous bones (t10
at the previous skinning offset) and the previous camera stay the twin's. Such a
record needs no engine motion row (required_engine_motion_rows []); its b7
declaration is neither enlarged nor added unless the previous graph reads another
b7 modifier row. It records previous_world "current" and previous_world_rows
[[b7 row, "INSTANCE_TRANSFORM", k], ...]. Class 1 root grafts, vehicle targets
(below) and camera variants are unchanged.

Vehicle targets move with their vehicle's transform. The engine data of the VS
marks them: the VEHICLE_DMG_POS vertex input (vehicle damage deformation), the
vehicle damage-grid modifiers MatMod_VehicleGridCorners and
MatMod_VehicleMeshPivotInGridSpace in its modifier metadata
(shader-modifier-contracts.json), or vehicle vertex factories only
(MeshStaticVehicle, MeshSkinnedVehicle in all-cache-techniques.json). Their rows
(twin and generic) record that evidence as "vehicle". The engine's own velocity
route for vehicle meshes reads the MotionMatrix (vehicle_destr_blendshape
MeshStaticVehicle c5783867... reads b7 rows 4..6, MatMod_MotionMatrix row 4), so
a vehicle root graft keeps the MotionMatrix previous world in both modes, and
its camera-only variant, which carries camera motion only, never serves as the
VS's only record (export_native_grafts.py refuses it).
"""
from pathlib import Path
from collections import Counter,defaultdict
from concurrent.futures import ThreadPoolExecutor
import hashlib,json,re,subprocess
from match_shared_native_motion import Shader,var,modifier_key,native_previous

import argparse
_parser=argparse.ArgumentParser(description=__doc__)
_parser.add_argument('--workspace', type=Path, required=True)
_parser.add_argument('--generic-extra', type=Path, action='append', default=[],
    help='JSON list of {sha256, vertex_factory} observed VS outside the transparent inventory; '
         'their disassembly is read from <workspace>/extended-position-inputs')
_parser.add_argument('--skinned-previous-world', choices=('motion','current'), default='motion',
    help='previous world of class-2 (skinning) root grafts: motion = engine MotionMatrix (b7 rows 24..26), '
         'current = the target\'s own INSTANCE_TRANSFORM rows; current writes <workspace>/native-grafted-skinned-current')
_args=_parser.parse_args()
p=_args.workspace.resolve(strict=True);_extra=_args.generic_extra
GRAFTED='native-grafted-skinned-current' if _args.skinned_previous_world=='current' else 'native-grafted'

# Engine data marking a VS that draws vehicle geometry (module docstring).
VEHICLE_FACTORIES={'MeshStaticVehicle','MeshSkinnedVehicle'}
VEHICLE_MODIFIERS={'MatMod_VehicleGridCorners','MatMod_VehicleMeshPivotInGridSpace'}
VEHICLE_INPUT=re.compile(r'^!\d+ = !\{i32 \d+, !"VEHICLE_DMG_POS", ',re.M)

def vehicle_evidence(text,factories,contract):
    """The engine data that marks a VS as drawing vehicle geometry; [] when none does."""
    evidence=['VEHICLE_DMG_POS input'] if VEHICLE_INPUT.search(text) else []
    evidence+=sorted({s['name']+' modifier' for c in contract or [] for s in c['slots'] if s['name'] in VEHICLE_MODIFIERS})
    if factories and set(factories)<=VEHICLE_FACTORIES:evidence.append('vehicle vertex factories only')
    return evidence

def emit_clip_outputs(a,text,native,node,lines,metadata,previous,input_cols,used_calls,dejittered=False):
    """Append de-jittered current clip and the given previous clip as two new outputs.

    Original definitions, stores and branches stay intact; only outputs, their
    signature metadata, required input usage and intrinsic declarations are added.
    dejittered: the target's SV_Position already is XY - b1[51].xy*W; it is
    exported unchanged instead of subtracting the jitter a second time.
    """
    # Reuse the native camera jitter convention already verified by the paired
    # original input experiment; current clip remains the target's own position.
    current_id=next(i for i,f in a.outs.items() if f[1]=='!"SV_Position"')
    current=[a.roots[current_id][i] for i in range(4)]
    if not dejittered:
        cam=next(v for v,h in a.handles.items() if h[0]==2 and h[2]==1)
        lines.append(f'  %graftJitter = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle {cam}, i32 51)')
        for i in range(2):
            lines.extend([f'  %graftJitter{i} = extractvalue %dx.types.CBufRet.f32 %graftJitter, {i}',
              f'  %graftJitterW{i} = fmul fast float %graftJitter{i}, {current[3]}',
              f'  %graftCurrent{i} = fsub fast float {current[i]}, %graftJitterW{i}'])
            current[i]=f'%graftCurrent{i}'
    first=max(a.outs)+1;register=max(int(f[8][4:])+int(f[6][4:]) for f in a.outs.values())
    entry=re.search(r'!dx.entryPoints = !\{!(\d+)\}',text)[1]
    sig=re.search(r'!"[^"]+", !(\d+),',a.md[entry])[1]
    outlist=re.findall(r'!(\d+)',a.md[sig])[1]
    zero,mask=node+2,node+3
    text=text.replace(f'!{outlist} = !{{{a.md[outlist]}}}',f'!{outlist} = !{{{a.md[outlist]}, !{node}, !{node+1}}}')
    for j,(name,values) in enumerate((('CURRENT',current),('PREVIOUS',previous))):
        metadata.append(f'!{node+j} = !{{i32 {first+j}, !"DIAGNOSTIC_{name}_CLIP", i8 9, i8 0, !{zero}, i8 2, i32 1, i8 4, i32 {register+j}, i8 0, !{mask}}}')
        for i,value in enumerate(values):lines.append(f'  call void @dx.op.storeOutput.f32(i32 5, i32 {first+j}, i32 0, i8 {i}, float {value})')
    metadata.extend([f'!{zero} = !{{i32 0}}',f'!{mask} = !{{i32 3, i32 15}}']);node+=4
    for sid,columns in input_cols.items():
        f=a.ins[sid];ref=f[-1];old=a.md.get(ref[1:],'')
        if old:
            flags=re.findall(r'i32 (\d+)',old)
            if len(flags)!=2 or flags[0]!='3':raise ValueError('unknown input usage metadata')
            columns|=int(flags[1])
        input_node=next(k for k,v in a.md.items() if v==', '.join(f))
        f=list(f);f[-1]='!'+str(node)
        text=text.replace(f'!{input_node} = !{{{a.md[input_node]}}}',f'!{input_node} = !{{'+', '.join(f)+'}')
        metadata.append(f'!{node} = !{{i32 3, i32 {columns}}}');node+=1
    for call in used_calls:
        if re.search(r'^declare .*@'+re.escape(call)+r'\(',text,re.M):continue
        decl=re.search(r'^declare .*@'+re.escape(call)+r'\(.*$',native,re.M)
        if not decl:raise ValueError('missing intrinsic declaration')
        text+='\n'+re.sub(r' #\d+$','',decl[0])+'\n'
    for typ,body in re.findall(r'^(%[\w.]+) = type (.*)$',native,re.M):
        if typ not in '\n'.join(lines):continue
        existing=re.search(r'^'+re.escape(typ)+r' = type (.*)$',text,re.M)
        if existing and existing[1]!=body:raise ValueError('incompatible DXIL type')
        if not existing:text=text.replace('define void @',typ+' = type '+body+'\n\ndefine void @',1)
    text=text.replace('  ret void','\n'.join(lines)+'\n  ret void')
    text=re.sub(r'^!dx.viewIdState = .*\n','',text,flags=re.M)
    text+='\n'+'\n'.join(metadata)+'\n'
    return text,first,len(lines)

FIXED_POINT='0x3EE0000000000000'   # 2^-17: fixed-point world translation (INSTANCE_TRANSFORM/MotionMatrix w)

def cone_values(s,roots):
    seen=set();todo=list(roots)
    while todo:
        v=todo.pop()
        if v in seen or v not in s.defs:continue
        seen.add(v);todo.extend(var.findall(s.defs[v]))
    return seen

def translation_chain(s,values,w):
    """Ops from a world row's w component to its float translation inside values: bitcast,
    the b1 row 38 subtraction when present, sitofp, 2^-17. Other operands are digests."""
    steps=[]
    while not re.fullmatch(r'fmul fast float %[\w.]+, '+FIXED_POINT,s.defs.get(w,'')):
        users=[u for u in values if w in var.findall(s.defs[u])]
        if len(users)!=1 or len(steps)==4:raise ValueError('world translation is not one fixed-point chain')
        steps.append(var.sub(lambda m,w=w:'@' if m[0]==w else s.digest(m[0]),s.defs[users[0]]));w=users[0]
    return steps

def current_world_rows(b,native,roots,motion_rows):
    """Native text whose previous clip reads the current world transform instead of the MotionMatrix.

    The engine MotionMatrix (b7 motion_rows) is the previous-frame object-to-world
    transform in the INSTANCE_TRANSFORM layout: row k = xyz and a fixed-point
    translation in w. Every use of MotionMatrix row motion_rows[k] component c
    becomes the INSTANCE_TRANSFORM k component c load of the native's current
    position, which the identical current graph shares with the target. Each
    row's previous translation chain must equal the current path's chain.
    Returns the text and [[b7 row, 'INSTANCE_TRANSFORM', k], ...].
    """
    oid=next(i for i,f in b.outs.items() if f[1]=='!"SV_Position"')
    current=cone_values(b,[b.roots[oid][i] for i in range(4)])
    world={}
    for v in current:
        m=re.search(r'@dx.op.loadInput\.\w+\(i32 4, i32 (\d+), i32 (\d+), i8 (\d+),',b.defs[v])
        if not m or b.ins[int(m[1])][1]!='!"INSTANCE_TRANSFORM"':continue
        f=b.ins[int(m[1])];k=int(re.findall(r'i32 (\d+)',b.md[f[4][1:]])[int(m[2])])
        if world.setdefault((k,int(m[3])),v)!=v:raise ValueError('current world component loaded twice')
    substitute={}
    for v in cone_values(b,roots):
        m=re.fullmatch(r'extractvalue %dx.types.CBufRet.f32 (%\d+), (\d)',b.defs[v])
        load=m and re.search(r'@dx.op.cbufferLoadLegacy.f32\(i32 59, %dx.types.Handle (%\d+), i32 (\d+)\)',b.defs.get(m[1],''))
        if not load or b.handles.get(load[1],())[:1]!=(2,) or b.handles[load[1]][2]!=7 or int(load[2]) not in motion_rows:continue
        key=(motion_rows.index(int(load[2])),int(m[2]))
        if key not in world:raise ValueError('motion matrix component without a current world input')
        substitute[v]=world[key]
    def replace(line):
        head=re.match(r'  %[\w.]+ = ',line);head=head[0] if head else ''
        return head+var.sub(lambda m:substitute.get(m[0],m[0]),line[len(head):])
    text='\n'.join(map(replace,native.split('\n')))
    c=Shader(text,b.modifiers,b.specializations)
    if any(binding==7 and row in motion_rows for binding,row in c.dependencies_values(roots)['cb_rows']):
        raise ValueError('previous clip still reads the motion matrix')
    previous=cone_values(c,roots)
    for k in range(3):
        if (k,3) not in world:raise ValueError('current world has no fixed-point translation row')
        if translation_chain(c,previous,world[k,3])!=translation_chain(c,current,world[k,3]):
            raise ValueError('previous world translation differs from the current path')
    return text,[[row,'INSTANCE_TRANSFORM',k] for k,row in enumerate(motion_rows)]

def graft(target,native,clip,target_contract=None,native_contract=None,specializations=None,coverage_guard=None,previous_world='motion'):
    a,b=Shader(target,target_contract),Shader(native,native_contract,specializations)
    geometric_roots=None
    if coverage_guard:
        geometric_roots,actual_guard=a.uncollapsed_position()
        if actual_guard!=coverage_guard:raise ValueError('coverage guard changed')
    ak,an=a.position_graph(geometric_roots);bk,bn=b.position_graph()
    if ak!=bk:raise ValueError('current-position arithmetic differs')
    roots=[b.roots[oid][col] for oid,col in clip['components']]
    deps=b.dependencies_values(roots)
    current_id=next(i for i,f in b.outs.items() if f[1]=='!"SV_Position"')
    current_cb=set(map(tuple,b.dependencies(current_id)['cb_rows']))
    motion_rows=sorted({r for binding,r in deps['cb_rows'] if binding==7 and (binding,r) not in current_cb})
    declared={s['row'] for c in native_contract or [] for s in c['slots'] if s['name']=='MatMod_MotionMatrix'}
    if len(declared)==1:
        start=next(iter(declared));motion_rows=[r for binding,r in deps['cb_rows'] if binding==7 and start<=r<start+3]
        motion_rows=sorted(set(motion_rows))
    if len(motion_rows)!=3 or motion_rows!=list(range(motion_rows[0],motion_rows[0]+3)):
        raise ValueError('native previous dependency is not a three-row motion matrix')
    world_rows=None
    if previous_world=='current':
        native,world_rows=current_world_rows(b,native,roots,motion_rows)
        b=Shader(native,native_contract,specializations)
    text=target.rstrip('\0\r\n')+'\n'
    if text.count('  ret void')!=1:raise ValueError('target needs a single exit')
    node=max(map(int,re.findall(r'^!(\d+) = ',text,re.M)))+1
    metadata=[];lines=[];mapping={};input_cols={};used_calls=set()
    shared={src[1]:dst[1] for src,dst in zip(bn,an)
            if src[0]==dst[0]=='value' and src[1] in b.defs and dst[1] in a.defs}
    dominators=a.exit_dominators()
    shared={src:dst for src,dst in shared.items() if a.value_block.get(dst) in dominators}
    material_dependent={}
    def relocate_row(row):
        if row in motion_rows:return 24+row-motion_rows[0]
        key=modifier_key(native_contract,row)
        candidates=[r for r in range(28) if modifier_key(target_contract,r)==key]
        if len(candidates)!=1:raise ValueError('unresolved previous-only material modifier')
        if 16*candidates[0]+16>material_bytes:raise ValueError('previous-only material modifier outside the declared b7')
        return candidates[0]
    def uses_motion(v):
        if v in material_dependent:return material_dependent[v]
        todo=[v];seen=set();result=False
        while todo:
            value=todo.pop()
            if value in seen or value not in b.defs:continue
            seen.add(value);rhs=b.defs[value]
            m=re.search(r'@dx.op.cbufferLoadLegacy.\w+\(i32 59, %dx.types.Handle (%\d+), i32 (\d+)\)',rhs)
            if m and b.handles.get(m[1],())[:1]==(2,) and b.handles[m[1]][2]==7 and int(m[2]) in motion_rows:
                result=True;break
            todo.extend(var.findall(rhs))
        material_dependent[v]=result;return result
    motion_handle=next((v for v,h in a.handles.items() if h[0]==2 and h[2]==7),None)
    original_material_bytes=0;material_bytes=448
    if motion_handle:
        h=a.handles[motion_handle]
        original_material_bytes=int(a.resources[h[0],h[1]][3][4:])
        if not 0<original_material_bytes<=448:raise ValueError('existing b7 storage exceeds native uploader')
        if world_rows:material_bytes=original_material_bytes
        elif original_material_bytes!=448:
            resources=re.search(r'!dx.resources = !\{!(\d+)\}',text)[1]
            cblist=a.md[resources].split(', ')[2][1:]
            candidates=[ref for ref in re.findall(r'!(\d+)',a.md[cblist])
                        if a.md[ref].split(', ')[0]=='i32 '+str(h[1])]
            if len(candidates)!=1:raise ValueError('ambiguous b7 declaration')
            ref=candidates[0];fields=a.md[ref].split(', ')
            fields[1]='%GraftedMotionConstants* undef';fields[6]='i32 448'
            text=text.replace(f'!{ref} = !{{{a.md[ref]}}}',f'!{ref} = !{{'+', '.join(fields)+'}')
            text=text.replace('define void @','%GraftedMotionConstants = type { [28 x <4 x float>] }\n\ndefine void @',1)
    elif world_rows and not any(binding==7 for binding,_ in b.dependencies_values(roots)['cb_rows']):material_bytes=0
    else:
        rid=max((rid for kind,rid in a.resources if kind==2),default=-1)+1
        resources=re.search(r'!dx.resources = !\{!(\d+)\}',text)[1]
        cblist=a.md[resources].split(', ')[2][1:]
        text=text.replace(f'!{cblist} = !{{{a.md[cblist]}}}',f'!{cblist} = !{{{a.md[cblist]}, !{node}}}')
        metadata.append(f'!{node} = !{{i32 {rid}, %GraftedMotionConstants* undef, !"", i32 0, i32 7, i32 1, i32 448, null}}')
        node+=1
        text=text.replace('define void @','%GraftedMotionConstants = type { [28 x <4 x float>] }\n\ndefine void @',1)
        motion_handle='%graftMotionHandle'
        lines.append(f'  {motion_handle} = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 {rid}, i32 7, i1 false)')
    def input_key(shader,sid,row):
        f=shader.ins[sid];indices=re.findall(r'i32 (\d+)',shader.md[f[4][1:]])
        return (f[1],f[2],f[3],indices[row])
    input_map={input_key(a,sid,row):(sid,row) for sid,f in a.ins.items() for row in range(int(f[6][4:]))}
    def rewrite(rhs):
        m=re.search(r'(@dx.op.loadInput\.\w+\(i32 4, i32 )(\d+), i32 (\d+), i8 (\d+),',rhs)
        if m:
            sid,row,col=map(int,m.groups()[1:]);dst,drow=input_map[input_key(b,sid,row)]
            if col>=int(a.ins[dst][7][3:]):raise ValueError('missing native input column')
            input_cols[dst]=input_cols.get(dst,0)|(1<<col)
            rhs=rhs[:m.start()]+m[1]+f'{dst}, i32 {drow}, i8 {col},'+rhs[m.end():]
        m=re.search(r'(@dx.op.cbufferLoadLegacy.\w+\(i32 59, %dx.types.Handle )(%\d+), i32 (\d+)\)',rhs)
        if m and b.handles.get(m[2],())[:1]==(2,) and b.handles[m[2]][2]==7:
            rhs=rhs[:m.start(3)]+str(relocate_row(int(m[3])))+rhs[m.end(3):]
        used_calls.update(re.findall(r'@(dx\.op\.[\w.]+)\(',rhs))
        return rhs
    def clone(v):
        resolved=b.normalized_value(v)
        if resolved!=v:return clone(resolved)
        if v not in b.defs:return v
        if v in mapping:return mapping[v]
        dep=uses_motion(v)
        if not dep and v in shared:mapping[v]=shared[v];return mapping[v]
        rhs=b.defs[v]
        if rhs.startswith('phi '):raise ValueError('native prior position needs new control-flow graft')
        if v in b.handles:
            h=b.handles[v]
            if h[0]==2 and h[2]==7:
                if not motion_handle:raise ValueError('previous graph reads b7 without a b7 declaration')
                mapping[v]=motion_handle;return motion_handle
            candidates=[x for x,q in a.handles.items() if q[0]==h[0] and q[2:]==h[2:] and a.resources[q[0],q[1]]==b.resources[h[0],h[1]]]
            if not candidates:raise ValueError('missing native resource contract')
            mapping[v]=candidates[0];return mapping[v]
        rhs=rewrite(rhs)
        replacements={x:clone(x) for x in var.findall(rhs)}
        rhs=var.sub(lambda m:replacements[m[0]],rhs)
        used_calls.update(re.findall(r'@(dx\.op\.[\w.]+)\(',rhs))
        mapping[v]='%graftPrevious'+v[1:]
        lines.append('  '+mapping[v]+' = '+rhs)
        return mapping[v]
    prefix=list(lines);mode='dag'
    try:previous=[clone(value) for value in roots]
    except ValueError as error:
        if str(error)!='native prior position needs new control-flow graft':raise
        mode='native-control-flow'
        if native.count('  ret void')!=1:raise ValueError('native needs a single exit')
        if re.search(r'@dx.op.(?:atomic|bufferStore|rawBufferStore|textureStore|barrier)',native):
            raise ValueError('native position route has side effects')
        # Keep original control flow and only its required SSA values. Shared
        # target entry values dominate the entire appended native region.
        mapping.clear();input_cols.clear();used_calls.clear();lines=list(prefix)
        needed=set();todo=list(roots)
        todo.extend(cond for edges in b.blocks.values() for _,cond,_ in edges if cond is not None)
        while todo:
            v=todo.pop()
            if v in needed or v not in b.defs:continue
            needed.add(v);todo.extend(re.findall(r'%\d+\b',b.defs[v]))
        reused=set()
        for v in needed:
            if v in b.specializations:
                mapping[v]=b.specializations[v];reused.add(v)
            elif v in b.handles:
                h=b.handles[v]
                if h[0]==2 and h[2]==7:
                    if not motion_handle:raise ValueError('previous graph reads b7 without a b7 declaration')
                    mapping[v]=motion_handle
                else:
                    matches=[x for x,q in a.handles.items() if q[0]==h[0] and q[2:]==h[2:] and a.resources[q[0],q[1]]==b.resources[h[0],h[1]]]
                    if not matches:raise ValueError('missing native control-flow resource')
                    mapping[v]=matches[0]
                reused.add(v)
            elif v in shared and not uses_motion(v):
                mapping[v]=shared[v];reused.add(v)
            else:mapping[v]='%graftPrevious'+v[1:]
        lines.extend(['  br label %graftBlock0','','graftBlock0:'])
        body=native.split('define void @',1)[1].split('\n}',1)[0].split('\n',1)[1]
        for line in body.splitlines():
            label=re.match(r'; <label>:(\d+)',line)
            if label:lines.extend(['','graftBlock'+label[1]+':']);continue
            definition=re.match(r'  (%\d+) = ',line)
            if definition:
                v=definition[1]
                if v not in needed or v in reused:continue
                rhs=rewrite(b.defs[v])
                if rhs.startswith('phi '):rhs=re.sub(r', %(\d+) \]',r', %graftBlock\1 ]',rhs)
                rhs=re.sub(r'%\d+\b',lambda m:mapping[m[0]],rhs)
                lines.append('  '+mapping[v]+' = '+rhs)
            elif re.match(r'  br ',line):
                line=re.sub(r'label %(\d+)',r'label %graftBlock\1',line)
                line=re.sub(r'%\d+\b',lambda m:mapping[m[0]],line)
                line=re.sub(r', !llvm.loop !\d+','',line)
                lines.append(line)
            elif re.match(r'  (?:switch|indirectbr|invoke) ',line):raise ValueError('unsupported native terminator')
        previous=[mapping.get(value,value) for value in roots]
    text,first,added=emit_clip_outputs(a,text,native,node,lines,metadata,previous,input_cols,used_calls)
    return text,dict(current_output=first,previous_output=first+1,added_instructions=added,
        shared_native_values=sum(v in shared.values() for v in mapping.values()),
        original_motion_rows=motion_rows,required_engine_motion_rows=[] if world_rows else [24,25,26],graft_mode=mode,
        coverage_guard=coverage_guard,original_material_bytes=original_material_bytes,
        required_material_bytes=material_bytes,gpu_verified=False,
        **(dict(previous_world='current',previous_world_rows=world_rows) if world_rows else {}))

CAMERA_LOAD=re.compile(r'@dx.op.cbufferLoadLegacy.\w+\(i32 59, %dx.types.Handle (%(?:\d+|graft[\w.]*)), i32 (\d+)\)')
COMMUTATIVE=('fmul fast float ','fadd fast float ','call float @dx.op.tertiary.f32(i32 46,')

def camera_projection(s,roots,start):
    """Values in the cone of roots that depend on a b1 row in [start, start+4)."""
    rows=range(start,start+4);memo={}
    def depends(v):
        if v in memo:return memo[v]
        if v not in s.defs:memo[v]=False;return False
        memo[v]=False
        rhs=s.defs[v];m=CAMERA_LOAD.search(rhs)
        if m and s.handles.get(m[1],())[:1]==(2,) and s.handles[m[1]][2]==1 and int(m[2]) in rows:result=True
        else:result=any([depends(x) for x in var.findall(rhs)])
        memo[v]=result;return result
    for v in roots:depends(v)
    return {v for v,x in memo.items() if x}

def camera_node(s,v,start):
    """Label with operands removed and camera rows relative to their block."""
    rhs=s.defs[v]
    if rhs.startswith('phi '):raise ValueError('camera projection contains control flow')
    m=CAMERA_LOAD.search(rhs)
    if m:return rhs[:m.start(1)]+'@, i32 ROW'+str(int(m[2])-start)+')'+rhs[m.end():],[m[1]],False
    label=var.sub('@',rhs)
    return label,var.findall(rhs),label.startswith(COMMUTATIVE)

def match_projection(b,b_region,b_start,x,a,a_region,a_start,y,mapping):
    """Structural match; operands outside the projection become a leaf bijection."""
    if (x in b_region)!=(y in a_region):return None
    if x not in b_region:
        if not x.startswith('%') or not y.startswith('%'):return mapping if x==y else None
        if x in mapping:return mapping if mapping[x]==y else None
        if y in mapping.values():return None
        return {**mapping,x:y}
    bl,bops,commutative=camera_node(b,x,b_start);al,aops,_=camera_node(a,y,a_start)
    if bl!=al or len(bops)!=len(aops):return None
    orders=[aops]+([[aops[1],aops[0]]+aops[2:]] if commutative else [])
    for order in orders:
        m=mapping
        for u,w in zip(bops,order):
            m=match_projection(b,b_region,b_start,u,a,a_region,a_start,w,m)
            if m is None:break
        if m is not None:return m
    return None

def camera_graft(target,native,clip,target_contract=None,native_contract=None,specializations=None,coverage_guard=None):
    """Previous clip = the native previous camera rows applied to the target's own current world position.

    The engine has no per-element previous transform for ordinary instanced
    arrays; its own velocity for them is camera-only reprojection. The native
    velocity VS's previous projection (b1 rows 16..19 or 12..15) is copied node
    for node. Its world-space operands are replaced by the operands the target
    multiplies with its current camera rows (28..31 or 0..3), paired by the
    structural position of each camera row/column. No b7 row, no module state.
    """
    a,b=Shader(target,target_contract),Shader(native,native_contract,specializations)
    if coverage_guard:
        roots,actual_guard=a.uncollapsed_position()
        if actual_guard!=coverage_guard:raise ValueError('coverage guard changed')
    else:
        oid=next(i for i,f in a.outs.items() if f[1]=='!"SV_Position"')
        if set(a.roots[oid])!={0,1,2,3}:raise ValueError('incomplete position')
        roots=[a.roots[oid][i] for i in range(4)]
    previous_start=clip['camera_rows'][0]
    if clip['camera_rows']!=list(range(previous_start,previous_start+4)):raise ValueError('native previous camera rows are not one block')
    previous_roots=[b.roots[oid][col] for oid,col in clip['components']]
    b_region=camera_projection(b,previous_roots,previous_start)
    found=[]
    for current_start in (28,0):
        a_region=camera_projection(a,roots,current_start)
        if not a_region:continue
        mapping={}
        for x,y in zip(previous_roots,roots):
            mapping=match_projection(b,b_region,previous_start,x,a,a_region,current_start,y,mapping)
            if mapping is None:break
        if mapping is not None:found.append((current_start,mapping))
    if len(found)!=1:
        raise ValueError('current projection does not match the native previous camera multiply' if not found
                         else 'ambiguous current camera rows')
    current_start,mapping=found[0]
    world={x:y for x,y in mapping.items() if x not in b.handles}
    cameras={x:y for x,y in mapping.items() if x in b.handles}
    if len(world)!=3:raise ValueError('world-space position is not three operands of the camera multiply')
    for x,y in cameras.items():
        if b.handles[x][0]!=2 or b.handles[x][2]!=1 or a.handles.get(y,())[:1]!=(2,) or a.handles[y][2]!=1:
            raise ValueError('camera multiply reads a non-camera resource')
        if a.resources[2,a.handles[y][1]]!=b.resources[2,b.handles[x][1]]:raise ValueError('camera binding contract differs')
    # Both previous and current world positions must be in the same camera-relative frame.
    previous_frame={r for binding,r in b.dependencies_values(list(world))['cb_rows'] if binding==1}
    current_frame={r for binding,r in a.dependencies_values(list(world.values()))['cb_rows'] if binding==1}
    if not previous_frame<=current_frame:raise ValueError('native previous world frame reads camera rows absent from current world')
    dominators=a.exit_dominators()
    if any(a.value_block.get(v) not in dominators for v in world.values()):raise ValueError('current world position does not dominate exit')
    text=target.rstrip('\0\r\n')+'\n'
    if text.count('  ret void')!=1:raise ValueError('target needs a single exit')
    node=max(map(int,re.findall(r'^!(\d+) = ',text,re.M)))+1
    lines=[];cloned={};used_calls=set()
    def clone(v):
        if v in mapping:return mapping[v]
        if v not in b_region:
            if v.startswith('%'):raise ValueError('unmapped native camera operand')
            return v
        if v in cloned:return cloned[v]
        rhs=b.defs[v]
        replacements={x:clone(x) for x in var.findall(rhs)}
        rhs=var.sub(lambda m:replacements[m[0]],rhs)
        used_calls.update(re.findall(r'@(dx\.op\.[\w.]+)\(',rhs))
        cloned[v]='%graftCamera'+v[1:]
        lines.append('  '+cloned[v]+' = '+rhs)
        return cloned[v]
    previous=[clone(v) for v in previous_roots]
    text,first,added=emit_clip_outputs(a,text,native,node,lines,[],previous,{},used_calls)
    return text,dict(camera_current_output=first,camera_previous_output=first+1,camera_added_instructions=added,
        camera_rows=clip['camera_rows'],camera_current_rows=list(range(current_start,current_start+4)),
        camera_world_operands=sorted(world.values(),key=lambda v:int(v[1:])),
        camera_world_frame_rows=sorted(current_frame),camera_supply_class=cone_supply_class(a,roots),camera_gpu_verified=False)

# Generic camera-only graft for transparent VS with no native current-position twin.
# The engine's velocity-init convention for a surface without object-motion supply:
# previous clip = previous view-projection x current world position.
CAMERA_LAYOUTS={28:16,0:12}   # target current VP block -> native previous VP block
ORIGIN_ROWS=(36,37,38)        # b1 camera-relative origin rows (camera position, fixed-point origin)
FLAGGED=re.compile(r'^f(?:add|sub|mul|div) (?:fast )?float\b')
INCOMING=re.compile(r'\[ ([^,\]]+), %([\w.]+) \]')
SELECT=re.compile(r'select i1 ([^,]+), float ([^,]+), float (.+)')

def flagless(label):
    return re.sub(r'^(f(?:add|sub|mul|div)) fast float\b',r'\1 float',label)

BINARY=re.compile(r'f(?:add|sub|mul|div) (?:fast )?float ([^,]+), ([^,]+)')
CALL_OPERAND=re.compile(r', float ([^,)]+)')

def region_node(s,v,start):
    """camera_node with every float operand lifted, literals included.

    Returns (label, operands, commutative, spans); spans locate the operands in
    s.defs[v], so a clone substitutes by position. A literal operand (a
    constant hide position) thus meets the other side's operand as a leaf.
    """
    rhs=s.defs[v]
    if rhs.startswith('phi '):raise ValueError('camera projection contains control flow')
    m=CAMERA_LOAD.search(rhs)
    if m:return rhs[:m.start(1)]+'@, i32 ROW'+str(int(m[2])-start)+')'+rhs[m.end():],[m[1]],False,[m.span(1)]
    m=BINARY.fullmatch(rhs)
    if m:spans=[m.span(1),m.span(2)]
    elif rhs.startswith('call float @dx.op.'):spans=[o.span(1) for o in CALL_OPERAND.finditer(rhs)]
    else:spans=[o.span() for o in var.finditer(rhs)]
    label=rhs
    for s0,e0 in reversed(spans):label=label[:s0]+'@'+label[e0:]
    return label,[rhs[s0:e0] for s0,e0 in spans],label.startswith(COMMUTATIVE),spans

def match_region(b,b_region,b_start,x,a,a_region,a_start,y,mapping,context=()):
    """match_projection that also accepts target branches inside its projection.

    DXC sinks a common multiply into the arms of a branch: clip = phi(VP*wA,
    VP*wB) + VP.w, or select(c, literal, VP*w). Each projected arm is matched
    against the same native node in its own context (a path of phi
    predecessors / select arms), so each arm has its own world vector. A
    literal arm (a collapsed vertex) is kept as it is. A world operand may be a
    literal (a constant hide position). Keys:
    ('leaf',context,native) -> target leaf (one bijection per context);
    ('pair',target) -> (native node, target operands in native operand order),
    or (native node, None) for a target phi/select.
    """
    if (x in b_region)!=(y in a_region):return None
    if x not in b_region:
        if not x.startswith('%'):return mapping if x==y else None
        if x in b.handles and not y.startswith('%'):return None
        key=('leaf',context,x)
        if key in mapping:return mapping if mapping[key]==y else None
        if y.startswith('%') and any(k[0]=='leaf' and k[1]==context and v==y for k,v in mapping.items()):return None
        return {**mapping,key:y}
    rhs=a.defs[y]
    arms=None
    if rhs.startswith('phi '):
        join=a.value_block[y]
        arms=[(value,('phi',join,pred)) for value,pred in INCOMING.findall(rhs)]
    elif rhs.startswith('select '):
        m=SELECT.fullmatch(rhs)
        if not m or m[1] in a_region:return None
        arms=[(m[2],('select',y,1)),(m[3],('select',y,2))]
    if arms is not None:
        m=mapping
        for value,step in arms:
            if value not in a_region and not value.startswith('%'):continue
            m=match_region(b,b_region,b_start,x,a,a_region,a_start,value,m,context+(step,))
            if m is None:return None
        return m if ('pair',y) in m else {**m,('pair',y):(x,None)}
    bl,bops,commutative,_=region_node(b,x,b_start);al,aops,_,_=region_node(a,y,a_start)
    if flagless(bl)!=flagless(al) or len(bops)!=len(aops):return None
    commutative=commutative or flagless(bl).startswith(('fmul float ','fadd float '))
    orders=[aops]+([[aops[1],aops[0]]+aops[2:]] if commutative else [])
    for order in orders:
        m=mapping
        for u,w in zip(bops,order):
            m=match_region(b,b_region,b_start,u,a,a_region,a_start,w,m,context)
            if m is None:break
        if m is not None:return m if ('pair',y) in m else {**m,('pair',y):(x,order)}
    return None

def insert_block_lines(text,before_terminator,after_phis):
    """Insert lines at the end of blocks (before their terminator) and after the
    leading phis of blocks. Existing lines are unchanged."""
    out=[];block=None;pending=None
    for line in text.split('\n'):
        if line.startswith('define void @'):block='0'
        label=re.match(r'; <label>:(\d+)',line)
        if label:
            block=label[1];pending=after_phis.get(block)
            out.append(line);continue
        if pending and line.startswith('  ') and not re.match(r'  %[\w.]+ = phi ',line):
            out.extend(pending);pending=None
        if block and re.match(r'  (?:br|ret|switch) ',line):
            out.extend(before_terminator.get(block,[]))
        out.append(line)
    return '\n'.join(out)

def cone_supply_class(a,roots):
    """Supply class (GraftClass* bits) of the cone of roots in a: 2 skinning inputs or
    t10 bones, 4 preskinned t9/b3, else 1 root-only. Used for a target's own current
    position and for a native twin's previous clip."""
    skinning_inputs={'BLENDINDICES','BLENDWEIGHT','INSTANCE_SKINNING_DATA','BONEINDEX'}
    deps=a.dependencies_values(roots)
    skinning=any(name.upper() in skinning_inputs for name,_,_ in deps['inputs'])
    preskinned=any(binding==3 for binding,_ in deps['cb_rows'])
    seen=set();todo=list(roots)
    while todo:
        v=todo.pop()
        if v in seen or v not in a.defs:continue
        seen.add(v);rhs=a.defs[v];todo.extend(var.findall(rhs))
        m=re.search(r'@dx\.op\.(?:rawBufferLoad|bufferLoad|textureLoad|sample\w*)\.\w+\(i32 \d+, %dx.types.Handle (%\d+)',rhs)
        h=a.handles.get(m[1]) if m else None
        if h and h[0]==0:
            skinning|=h[2]==10;preskinned|=h[2]==9
    return (2 if skinning else 0)|(4 if preskinned else 0) or 1

def generic_camera_graft(target,templates):
    """Previous clip = the canonical native previous view-projection multiply on the target's own world position.

    For a VS without a native current-position twin. templates maps the
    target's current VP block (28 or 0) to (sha, native text, previous clip) of
    one validated native velocity VS; its previous camera region (b1 16..19 or
    12..15) must match the target's current projection node for node, with
    the target's world operands as the only leaves. The world operands are in
    the frame the target's current VP block takes (its own clip uses them);
    native_frames() checks that every native VS feeds the paired previous block
    world positions in that same frame.
    """
    a=Shader(target)
    oid=next((i for i,f in a.outs.items() if f[1]=='!"SV_Position"'),None)
    if oid is None:raise ValueError('no SV_Position output (not a rasterized vertex stage)')
    if set(a.roots[oid])!={0,1,2,3}:raise ValueError('incomplete position')
    roots=[a.roots[oid][i] for i in range(4)];coverage_guard=None
    try:roots,coverage_guard=a.uncollapsed_position()
    except ValueError:pass
    try:roots=a.before_jitter_subtraction(roots);dejittered=True
    except ValueError:dejittered=False
    blocks=[start for start in CAMERA_LAYOUTS if camera_projection(a,roots,start)]
    rows={r for binding,r in a.dependencies_values(roots)['cb_rows'] if binding==1}
    if not blocks:
        raise ValueError('clip reads no current view-projection rows (b1 28..31 or 0..3): '+
            ('screen-space position from the b1[48] viewport terms' if rows<={48} else f'b1 rows {sorted(rows)}'))
    found=[]
    for current_start in blocks:
        template_sha,native,clip=templates[current_start]
        b=Shader(native);previous_start=clip['camera_rows'][0]
        previous_roots=[b.roots[o][col] for o,col in clip['components']]
        b_region=camera_projection(b,previous_roots,previous_start)
        a_region=camera_projection(a,roots,current_start)
        mapping={}
        for x,y in zip(previous_roots,roots):
            mapping=match_region(b,b_region,previous_start,x,a,a_region,current_start,y,mapping)
            if mapping is None:break
        if mapping is not None:found.append((current_start,template_sha,native,clip,b,a_region,mapping))
    if not found:
        reason='current clip is not a per-vertex view-projection multiply of a world position'
        if rows>={8,9,10,11}:
            reason+=': a branch arm projects in two stages (b1 24..26 view rotation, then b1 8..11 projection)'
        elif any(a.defs.get(v,'').startswith('select ') and SELECT.fullmatch(a.defs[v]) and SELECT.fullmatch(a.defs[v])[1] in region
                 for start in blocks for region in [camera_projection(a,roots,start)] for v in region):
            reason+=': a vertex collapse condition reads the current view-projection (projected-size cull)'
        raise ValueError(reason)
    if len(found)!=1:raise ValueError('ambiguous current camera rows')
    current_start,template_sha,native,clip,b,a_region,mapping=found[0]
    for k,y in mapping.items():
        if k[0]!='leaf' or k[2] not in b.handles:continue
        x=k[2]
        if b.handles[x][0]!=2 or b.handles[x][2]!=1 or a.handles.get(y,())[:1]!=(2,) or a.handles[y][2]!=1:
            raise ValueError('camera multiply reads a non-camera resource')
        if a.resources[2,a.handles[y][1]]!=b.resources[2,b.handles[x][1]]:raise ValueError('camera binding contract differs')
    # One world vector per projected arm: along each maximal context path the
    # native multiply's three world operands map to exactly one target value each.
    template_world={k[2] for k in mapping if k[0]=='leaf' and k[2] not in b.handles}
    world_leaves=[(k[1],k[2],y) for k,y in mapping.items() if k[0]=='leaf' and k[2] not in b.handles]
    contexts={c for c,_,_ in world_leaves}
    paths=[c for c in contexts if not any(d!=c and d[:len(c)]==c for d in contexts)] or [()]
    world_sets=[]
    for path in sorted(paths,key=str):
        world={}
        for c,x,y in world_leaves:
            if path[:len(c)]!=c:continue
            if world.get(x,y)!=y:raise ValueError('world operand bound twice on one projection arm')
            world[x]=y
        if len(template_world)!=3 or set(world)!=template_world:
            raise ValueError('world-space position is not three operands of the camera multiply')
        world_sets.append(world)
    branches=[p for p in paths if p]
    frame_rows=[sorted({r for binding,r in a.dependencies_values(list(w.values()))['cb_rows'] if binding==1}) for w in world_sets]
    dominators=a.exit_dominators()
    if any(a.value_block.get(v) not in dominators for v in roots):
        raise ValueError('current clip is not computed on every path to the exit')
    text=target.rstrip('\0\r\n')+'\n'
    if text.count('  ret void')!=1:raise ValueError('target needs a single exit')
    node=max(map(int,re.findall(r'^!(\d+) = ',text,re.M)))+1
    ends=defaultdict(list);heads=defaultdict(list);cloned={};used_calls=set();adopted=0
    # Walk the target's matched projection. Each node becomes the matched native
    # instruction (previous camera row, native operand order) on the clones of
    # the target operands, placed at the end of the target node's own block, so
    # the previous graph shares exactly the nodes the current graph shares.
    def clone(y):
        nonlocal adopted
        if y not in a_region:return y
        if y in cloned:return cloned[y]
        x,order=mapping['pair',y]
        name='%graftCamera'+y[1:]
        if order is None and a.defs[y].startswith('select '):
            m=SELECT.fullmatch(a.defs[y])
            ends[a.value_block[y]].append(f'  {name} = select i1 {m[1]}, float {clone(m[2])}, float {clone(m[3])}')
        elif order is None:
            parts=[f'[ {clone(value)}, %{pred} ]' for value,pred in INCOMING.findall(a.defs[y])]
            heads[a.value_block[y]].append(f'  {name} = '+a.defs[y].split(' [',1)[0]+' '+', '.join(parts))
        else:
            rhs=b.defs[x];_,ops,_,spans=region_node(b,x,clip['camera_rows'][0])
            if len(ops)!=len(order):raise ValueError('native camera node operand count differs')
            values=[clone(w) for w in order]
            for (s0,e0),value in sorted(zip(spans,values),reverse=True):rhs=rhs[:s0]+value+rhs[e0:]
            native_flags,target_flags=FLAGGED.match(b.defs[x]),FLAGGED.match(a.defs[y])
            if native_flags and native_flags[0]!=target_flags[0]:
                rhs=FLAGGED.sub(target_flags[0],rhs,count=1);adopted+=1
            used_calls.update(re.findall(r'@(dx\.op\.[\w.]+)\(',rhs))
            ends[a.value_block[y]].append(f'  {name} = {rhs}')
        cloned[y]=name
        return name
    previous=[clone(v) for v in roots]
    text=insert_block_lines(text,ends,heads)
    inserted=sum(map(len,ends.values()))+sum(map(len,heads.values()))
    text,first,added=emit_clip_outputs(a,text,native,node,[],[],previous,{},used_calls,dejittered)
    return text,dict(camera_current_output=first,camera_previous_output=first+1,camera_added_instructions=added+inserted,
        camera_rows=clip['camera_rows'],camera_current_rows=list(range(current_start,current_start+4)),
        camera_template_sha256=template_sha,camera_branches=len(branches),
        camera_world_operands=[sorted(w.values(),key=str) for w in world_sets],
        camera_world_frame_rows=frame_rows,camera_current_dejittered=dejittered,coverage_guard=coverage_guard,
        camera_fast_math_adopted=adopted,
        camera_supply_class=cone_supply_class(a,roots),camera_gpu_verified=False)

def main():
    rows=json.loads((p/'shared-native-motion-matches.json').read_text())['matches']
    contracts=json.loads((p/'shader-modifier-contracts.json').read_text())['shaders']
    factories=defaultdict(set)
    for t in json.loads((p/'all-cache-techniques.json').read_text())['techniques']:
        for program in t['programs']:
            if program['kind']=='vs':factories[program['sha256']].add(t['vertex_factory'])
    previous_classes={}
    def previous_class(n):
        """cone_supply_class of a native twin's previous clip."""
        if n['sha256'] not in previous_classes:
            b=Shader((p/'opaque-velocity-audit'/(n['sha256']+'.ll')).read_text())
            previous_classes[n['sha256']]=cone_supply_class(b,[b.roots[o][col] for o,col in n['previous'][0]['components']])
        return previous_classes[n['sha256']]
    out=p/GRAFTED;out.mkdir(exist_ok=True)
    tool=p.parent/'glass-native-material-buildcheck/GeometryShaderTool.exe'
    compiler=p.parent/'glass-optiscaler-source/OptiScaler/shaders/shader_tools/dxcompiler.dll'
    def assemble(ll,text):
        ll.write_text(text)
        result=subprocess.run([str(tool),str(compiler),'assemble',str(ll),str(ll.with_suffix('.dxil')),'0'],capture_output=True,text=True)
        if result.returncode:raise ValueError((result.stdout+result.stderr)[-1800:])
    def process(r):
        target=(p/'all-transparent-vs'/(r['sha256']+'.ll')).read_text()
        candidates=sorted(r['native_candidates'],key=lambda n:min(x['dependencies']['instructions'] for x in n['previous']))
        families=sorted(factories.get(r['sha256'],()))
        if not families:raise ValueError(r['sha256']+' has no technique in all-cache-techniques.json')
        vehicle=vehicle_evidence(target,families,contracts.get(r['sha256']))
        # Factory-consistent twins (module docstring): a skinned target refuses
        # root-only twins of another vertex factory.
        skinned=any('Skinned' in f for f in families)
        twins=[n for n in candidates if not skinned or previous_class(n)!=1
               or not factories.get(n['sha256'],set()).isdisjoint(families)]
        def attempt(build,path,candidates,options=lambda n:{}):
            errors=[]
            for n in candidates:
                try:
                    text,info=build(target,(p/'opaque-velocity-audit'/(n['sha256']+'.ll')).read_text(),n['previous'][0],
                        contracts.get(r['sha256']),contracts.get(n['sha256']),n.get('specializations'),r.get('coverage_guard'),**options(n))
                    assemble(path,text);return n,info,[]
                except (ValueError,KeyError,StopIteration,AssertionError) as e:errors.append(str(e))
            for stale in (path,path.with_suffix('.dxil')):stale.unlink(missing_ok=True)
            return None,None,sorted(set(errors))
        # --skinned-previous-world current: class-2 twins graft with the target's current world rows,
        # except vehicle targets, which keep the engine MotionMatrix (module docstring).
        n,info,errors=attempt(graft,out/(r['sha256']+'.ll'),twins,lambda n:dict(previous_world=
            'current' if _args.skinned_previous_world=='current' and previous_class(n)==2 and not vehicle else 'motion'))
        if n:row=dict(sha256=r['sha256'],families=families,native_sha256=n['sha256'],status='validated',
                      specializations=n.get('specializations',{}),**info)
        elif len(twins)<len(candidates):
            row=dict(sha256=r['sha256'],families=families,status='factory_mismatch',errors=[
                f'{len(candidates)-len(twins)} native twin(s) of another vertex factory with a root-only previous graph '
                '(no skinning input, t10 or preskinned t9/b3) refused for a skinned vertex factory']+errors)
        else:row=dict(sha256=r['sha256'],families=families,status='unsupported',errors=errors)
        # Camera-only variant: prefer the root graft's own native candidate.
        if n:candidates=[n]+[c for c in candidates if c is not n]
        c,info,errors=attempt(camera_graft,out/(r['sha256']+'.camera.ll'),candidates)
        if c:row.update(camera_status='validated',camera_native_sha256=c['sha256'],**info)
        else:row.update(camera_status='unsupported',camera_errors=errors)
        if vehicle:row['vehicle']=vehicle
        return row
    with ThreadPoolExecutor(max_workers=4) as pool:results=list(pool.map(process,rows))
    summary=dict(total=len(rows),statuses=dict(Counter(r['status'] for r in results)),
        camera_statuses=dict(Counter(r['camera_status'] for r in results)),
        camera_rows=dict(Counter(str(r['camera_rows']) for r in results if r['camera_status']=='validated')),
        copied_from_original_native_mv=True,live_admitted=False)
    if _args.skinned_previous_world=='current':
        summary['previous_world']=dict(Counter(r.get('previous_world','motion') for r in results if r['status']=='validated'))
    generic,generic_summary=generic_phase(rows,results,out,assemble,factories,contracts)
    summary['generic_camera']=generic_summary
    summary['vehicle']=dict(twin_statuses=dict(Counter(r['status'] for r in results if r.get('vehicle'))),
        twin_camera_only=sum(1 for r in results if r.get('vehicle') and r['status']!='validated' and r['camera_status']=='validated'),
        generic_camera_statuses=dict(Counter(r['camera_status'] for r in generic if r.get('vehicle'))))
    (out/'index.json').write_text(json.dumps(dict(summary=summary,shaders=results,generic_camera=generic),indent=2))
    print(json.dumps(summary,indent=2))

def camera_templates(results):
    """One canonical native template per camera layout: the native VS most used by validated twin camera grafts."""
    templates={}
    for current_start,previous_start in CAMERA_LAYOUTS.items():
        used=Counter(r['camera_native_sha256'] for r in results if r.get('camera_status')=='validated'
                     and r['camera_rows'][0]==previous_start and r['camera_current_rows'][0]==current_start)
        if not used:raise ValueError(f'no validated native template for b1 {current_start}->{previous_start}')
        sha=min(used,key=lambda s:(-used[s],s))
        native=(p/'opaque-velocity-audit'/(sha+'.ll')).read_text()
        b=Shader(native);clip=native_previous(b)[0]
        if clip['camera_rows'][0]!=previous_start:raise ValueError('template previous camera block differs')
        # The template's previous multiply is the same fixed 4x4 transform as its
        # own current multiply: node for node, rows renamed, three world leaves.
        oid=next(i for i,f in b.outs.items() if f[1]=='!"SV_Position"')
        current=[b.roots[oid][i] for i in range(4)]
        previous=[b.roots[o][col] for o,col in clip['components']]
        b_region=camera_projection(b,previous,previous_start);c_region=camera_projection(b,current,current_start)
        mapping={}
        for x,y in zip(previous,current):
            mapping=match_region(b,b_region,previous_start,x,b,c_region,current_start,y,mapping)
            if mapping is None:raise ValueError('template previous multiply differs from its current multiply')
        leaves=[k for k in mapping if k[0]=='leaf']
        if any(k[1] for k in leaves) or any(k[0]=='pair' and v[1] is None for k,v in mapping.items()) \
                or sum(k[2] not in b.handles for k in leaves)!=3:
            raise ValueError('template multiply is not one plain 4x4 transform of three world operands')
        templates[current_start]=(sha,native,clip)
    return templates

def native_frames():
    """Check that native VS feed each previous VP block world positions in the current block's frame.

    For every native velocity VS: (current VP block, previous VP block) and the
    b1 origin rows (36..38) its current and previous world positions read. The
    previous world's origin rows must be a subset of the current world's, so a
    previous block takes world positions in its paired current block's frame.
    Returns counts per (pair, current origin, previous origin).
    """
    frames=Counter()
    for r in json.loads((p/'opaque-velocity-audit/index.json').read_text())['shaders']:
        try:
            s=Shader(Path(r['disassembly']).read_text());clip=native_previous(s)[0]
        except (ValueError,KeyError,StopIteration):continue
        oid=next(i for i,f in s.outs.items() if f[1]=='!"SV_Position"')
        current={row for binding,row in s.dependencies(oid)['cb_rows'] if binding==1}
        previous={row for binding,row in s.dependencies_values([s.roots[o][col] for o,col in clip['components']])['cb_rows'] if binding==1}
        start=28 if 28 in current else 0 if 0 in current else None
        pair=(start,clip['camera_rows'][0])
        if pair in CAMERA_LAYOUTS.items() and not previous&set(ORIGIN_ROWS)<=current&set(ORIGIN_ROWS):
            raise ValueError(f'native {r["sha256"]} previous world origin differs from its current world origin')
        frames[pair,tuple(sorted(current&set(ORIGIN_ROWS))),tuple(sorted(previous&set(ORIGIN_ROWS)))]+=1
    return frames

def generic_phase(rows,results,out,assemble,factories,contracts):
    """Camera-only grafts for transparent VS outside the native twin matches.

    factories: vertex factories per VS from all-cache-techniques.json, the vehicle
    evidence's factory list (falls back to the families below for other VS).
    """
    templates=camera_templates(results);frames=native_frames()
    matched={r['sha256'] for r in rows}
    families={g['sha256']:g['families'] for g in json.loads((p/'native-motion-gaps.json').read_text())['shaders']}
    for path in _extra:
        for x in json.loads(Path(path).read_text()):families.setdefault(x['sha256'],[x['vertex_factory']])
    # The rest of the transparent inventory (e.g. glass_scope highlights, absent
    # from the gap audit): vertex factories from the technique catalogs.
    techniques={}
    for name in ('all-transparent-vs-catalog.json','scope-shader-catalog.json'):
        for sha,names in json.loads((p/name).read_text()).items():techniques.setdefault(sha,[]).extend(names)
    for f in (p/'all-transparent-vs').glob('*.ll'):
        if f.stem not in families:
            families[f.stem]=sorted({m[1] for t in techniques.get(f.stem,[]) for m in [re.search(r'VF: (\w+)',t)] if m}) or ['unknown']
    def process(sha):
        source=next((f for f in (p/'all-transparent-vs'/(sha+'.ll'),p/'extended-position-inputs'/(sha+'.ll')) if f.exists()),None)
        row=dict(sha256=sha,families=families[sha],source=str(source.relative_to(p)) if source else None)
        path=out/(sha+'.camera.ll')
        text=source.read_text() if source else ''
        try:
            if not source:raise ValueError('disassembly absent from the transparent inventory')
            graft_text,info=generic_camera_graft(text,templates)
            assemble(path,graft_text);row.update(camera_status='validated',**info)
        except (ValueError,KeyError,StopIteration,AssertionError) as e:
            for stale in (path,path.with_suffix('.dxil')):stale.unlink(missing_ok=True)
            row.update(camera_status='unsupported',camera_errors=[str(e)[:400]])
        vehicle=vehicle_evidence(text,sorted(factories.get(sha,())) or families[sha],contracts.get(sha))
        if vehicle:row['vehicle']=vehicle
        return row
    with ThreadPoolExecutor(max_workers=4) as pool:generic=list(pool.map(process,sorted(set(families)-matched)))
    per_family=defaultdict(Counter)
    for r in generic:
        for f in r['families']:per_family[f][r['camera_status']]+=1
    summary=dict(total=len(generic),statuses=dict(Counter(r['camera_status'] for r in generic)),
        templates={f'b1 {c}->{t[2]["camera_rows"][0]}':t[0] for c,t in templates.items()},
        native_frames={f'b1 {pair[0]}->{pair[1]} current origin {list(c)} previous origin {list(q)}':n for (pair,c,q),n in sorted(frames.items(),key=str)},
        errors=dict(Counter(r['camera_errors'][0].split('\n')[0][:120] for r in generic if r['camera_status']!='validated')),
        families={f:dict(c) for f,c in sorted(per_family.items())})
    return generic,summary

if __name__=='__main__':main()
