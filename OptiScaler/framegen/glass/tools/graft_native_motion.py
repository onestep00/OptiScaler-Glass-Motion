"""Transplant native previous-position DAGs into matching transparent vertex shaders.

Preserves original outputs. Shares identical current-position subexpressions.
Only b7 motion rows are relocated; no new motion/deformation arithmetic is invented.
Generated shaders remain offline candidates until their live b7 supply is admitted.
"""
from pathlib import Path
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import hashlib,json,re,subprocess
from match_shared_native_motion import Shader,var,modifier_key

import argparse
_parser=argparse.ArgumentParser(description=__doc__)
_parser.add_argument('--workspace', type=Path, required=True)
p=_parser.parse_args().workspace.resolve(strict=True)

def graft(target,native,clip,target_contract=None,native_contract=None,specializations=None,coverage_guard=None):
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
    original_material_bytes=0
    if motion_handle:
        h=a.handles[motion_handle]
        original_material_bytes=int(a.resources[h[0],h[1]][3][4:])
        if not 0<original_material_bytes<=448:raise ValueError('existing b7 storage exceeds native uploader')
        if original_material_bytes!=448:
            resources=re.search(r'!dx.resources = !\{!(\d+)\}',text)[1]
            cblist=a.md[resources].split(', ')[2][1:]
            candidates=[ref for ref in re.findall(r'!(\d+)',a.md[cblist])
                        if a.md[ref].split(', ')[0]=='i32 '+str(h[1])]
            if len(candidates)!=1:raise ValueError('ambiguous b7 declaration')
            ref=candidates[0];fields=a.md[ref].split(', ')
            fields[1]='%GraftedMotionConstants* undef';fields[6]='i32 448'
            text=text.replace(f'!{ref} = !{{{a.md[ref]}}}',f'!{ref} = !{{'+', '.join(fields)+'}')
            text=text.replace('define void @','%GraftedMotionConstants = type { [28 x <4 x float>] }\n\ndefine void @',1)
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
            if h[0]==2 and h[2]==7:mapping[v]=motion_handle;return motion_handle
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
                if h[0]==2 and h[2]==7:mapping[v]=motion_handle
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
    # Reuse the native camera jitter convention already verified by the paired
    # original input experiment; current clip remains the target's own position.
    current_id=next(i for i,f in a.outs.items() if f[1]=='!"SV_Position"')
    current=[a.roots[current_id][i] for i in range(4)]
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
    return text,dict(current_output=first,previous_output=first+1,added_instructions=len(lines),
        shared_native_values=sum(v in shared.values() for v in mapping.values()),
        original_motion_rows=motion_rows,required_engine_motion_rows=[24,25,26],graft_mode=mode,
        coverage_guard=coverage_guard,original_material_bytes=original_material_bytes,
        required_material_bytes=448,gpu_verified=False)

def main():
    rows=json.loads((p/'shared-native-motion-matches.json').read_text())['matches']
    contracts=json.loads((p/'shader-modifier-contracts.json').read_text())['shaders']
    out=p/'native-grafted';out.mkdir(exist_ok=True)
    tool=p.parent/'glass-native-material-buildcheck/GeometryShaderTool.exe'
    compiler=p.parent/'glass-optiscaler-source/OptiScaler/shaders/shader_tools/dxcompiler.dll'
    def process(r):
        errors=[]
        candidates=sorted(r['native_candidates'],key=lambda n:min(x['dependencies']['instructions'] for x in n['previous']))
        for n in candidates:
            try:
                text,info=graft((p/'all-transparent-vs'/(r['sha256']+'.ll')).read_text(),
                    (p/'opaque-velocity-audit'/(n['sha256']+'.ll')).read_text(),n['previous'][0],
                    contracts.get(r['sha256']),contracts.get(n['sha256']),n.get('specializations'),r.get('coverage_guard'))
                ll=out/(r['sha256']+'.ll');ll.write_text(text)
                result=subprocess.run([str(tool),str(compiler),'assemble',str(ll),str(ll.with_suffix('.dxil')),'0'],capture_output=True,text=True)
                if result.returncode:raise ValueError((result.stdout+result.stderr)[-1800:])
                return dict(sha256=r['sha256'],native_sha256=n['sha256'],status='validated',specializations=n.get('specializations',{}),**info)
            except (ValueError,KeyError,StopIteration,AssertionError) as e:errors.append(str(e))
        return dict(sha256=r['sha256'],status='unsupported',errors=sorted(set(errors)))
    with ThreadPoolExecutor(max_workers=4) as pool:results=list(pool.map(process,rows))
    summary=dict(total=len(rows),statuses=dict(Counter(r['status'] for r in results)),
        copied_from_original_native_mv=True,live_admitted=False)
    (out/'index.json').write_text(json.dumps(dict(summary=summary,shaders=results),indent=2))
    print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
