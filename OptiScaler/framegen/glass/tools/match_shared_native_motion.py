"""Match original position arithmetic across transparent and native velocity VS.

Uses semantic inputs and referenced resource contracts, not material names.
Compares position and control dependencies. Matches are offline graft candidates;
they do not prove live resource contents, grouped identity, or temporal validity.
"""
from pathlib import Path
from collections import Counter, defaultdict
import hashlib, json, re,struct,zlib

import argparse
_parser=argparse.ArgumentParser(description=__doc__)
_parser.add_argument('--workspace', type=Path, required=True)
p=_parser.parse_args().workspace.resolve(strict=True)
var=re.compile(r'%(?:\d+|graft[\w.]*)\b')

def modifier_key(contracts,row):
    if not contracts:return ('RAW',row)
    variants=[]
    for contract in contracts:
        earlier=[s for s in contract['slots'] if s['row']<=row]
        if not earlier:variants.append(('RAW',row));continue
        start=max(s['row'] for s in earlier)
        variants.append(tuple(sorted((s['name'],s['kind'],row-start) for s in earlier if s['row']==start)))
    if len(set(variants))!=1:raise ValueError('ambiguous named material slot')
    return variants[0]

class Shader:
    def __init__(self,text,modifiers=None,specializations=None):
        self.text=text
        self.modifiers=modifiers
        self.specializations=specializations or {}
        self.md=dict(re.findall(r'^!(\d+) = !\{(.*)\}$',text,re.M))
        entry=re.search(r'!dx.entryPoints = !\{!(\d+)\}',text)[1]
        sig=re.search(r'!"[^"]+", !(\d+),',self.md[entry])[1]
        ins,outs=re.findall(r'!(\d+)',self.md[sig])[:2]
        def signature(node):
            return {int(self.md[r].split(', ')[0][4:]):self.md[r].split(', ')
                    for r in re.findall(r'!(\d+)',self.md[node])}
        self.ins,self.outs=signature(ins),signature(outs)
        self.defs=dict(re.findall(r'^  (%(?:\d+|graft[\w.]*)) = (.*?)(?:\s+;.*)?$',text,re.M))
        resources=re.search(r'!dx.resources = !\{!(\d+)\}',text)
        self.resources={}
        if resources:
            for kind,lst in enumerate(self.md[resources[1]].split(', ')):
                if lst=='null':continue
                for r in re.findall(r'!(\d+)',self.md[lst[1:]]):
                    fields=self.md[r].split(', ')
                    def expand(value):
                        return re.sub(r'!(\d+)',lambda m:'{'+self.md[m[1]]+'}',value)
                    # Retain register space, lower bound, extent, kind/size and
                    # resource properties. Local range ids are assigned per VS.
                    self.resources[kind,int(fields[0][4:])]=tuple(expand(x) for x in fields[3:])
        self.roots=defaultdict(dict)
        for m in re.finditer(r'@dx.op.storeOutput.f32\(i32 5, i32 (\d+), i32 0, i8 (\d+), float ([^)]+)\)',text):
            oid,col=int(m[1]),int(m[2])
            if col in self.roots[oid]:raise ValueError('multiple stores to one output component')
            self.roots[oid][col]=m[3]
        self.cache={};self.active=set();self.handles={}
        self.blocks={};self.predecessors=defaultdict(list);self.value_block={}
        self.reach_cache={};self.reach_active=set()
        self.entry_block='0'
        self.return_blocks=set()
        block='0'
        for line in text.splitlines():
            label=re.match(r'; <label>:(\d+)',line) or re.match(r'(graft\w+):',line)
            if label:block=label[1]
            value=re.match(r'  (%(?:\d+|graft[\w.]*)) = ',line)
            if value:self.value_block[value[1]]=block
            if line.strip()=='ret void':self.return_blocks.add(block)
            branch=re.match(r'  br i1 ([^,]+), label %([\w.]+), label %([\w.]+)',line)
            direct=re.match(r'  br label %([\w.]+)',line)
            if branch:
                self.blocks[block]=[(branch[2],branch[1],True),(branch[3],branch[1],False)]
            elif direct:self.blocks[block]=[(direct[1],None,True)]
        for src,edges in self.blocks.items():
            for dst,condition,positive in edges:self.predecessors[dst].append((src,condition,positive))
        for v,rhs in self.defs.items():
            m=re.search(r'@dx.op.createHandle\(i32 57, i8 (\d+), i32 (\d+), i32 (\d+), i1 (true|false)\)',rhs)
            if m:self.handles[v]=(int(m[1]),int(m[2]),int(m[3]),m[4])

    def exit_dominators(self):
        blocks={'0'}|set(self.blocks)|set(self.predecessors)|self.return_blocks
        dom={b:({'0'} if b=='0' else set(blocks)) for b in blocks}
        changed=True
        while changed:
            changed=False
            for block in blocks-{'0'}:
                parents=[dom[src] for src,_,_ in self.predecessors.get(block,[])]
                value={block}|(set.intersection(*parents) if parents else set())
                if value!=dom[block]:dom[block]=value;changed=True
        if not self.return_blocks:raise ValueError('no shader exit')
        return set.intersection(*(dom[b] for b in self.return_blocks))

    def named_material_load(self,rhs):
        m=re.search(r'@dx.op.cbufferLoadLegacy.\w+\(i32 59, %dx.types.Handle (%(?:\d+|graft[\w.]*)), i32 (\d+)\)',rhs)
        if m and self.handles.get(m[1],())[:1]==(2,) and self.handles[m[1]][2]==7:
            rhs=rhs[:m.start(2)]+'MODIFIER'+repr(modifier_key(self.modifiers,int(m[2])))+rhs[m.end(2):]
        return rhs

    def normalized_value(self,v):
        if v in self.specializations:return self.specializations[v]
        rhs=self.defs.get(v,'')
        m=re.fullmatch(r'fmul fast float ([^,]+), (.+)',rhs)
        if m:
            left=self.specializations.get(m[1],m[1]);right=self.specializations.get(m[2],m[2])
            if left=='1.000000e+00':return right
            if right=='1.000000e+00':return left
        return v

    def digest(self,v):
        if not v.startswith('%'):return v
        if v in self.cache:return self.cache[v]
        if v in self.active or v not in self.defs:raise ValueError('unresolved or cyclic position dependency')
        self.active.add(v);rhs=self.defs[v]
        if rhs.startswith('phi '):
            incoming=re.findall(r'\[ ([^,\]]+), %(\d+) \]',rhs)
            if not incoming:raise ValueError('unsupported phi syntax')
            values=[self.digest(value) for value,_ in incoming]
            if len(set(values))==1:
                self.cache[v]=values[0];self.active.remove(v);return values[0]
            terms=[]
            for (value,src),digest in zip(incoming,values):
                edges=[e for e in self.blocks.get(src,[]) if e[0]==self.value_block[v]]
                if len(edges)!=1:raise ValueError('ambiguous phi predecessor edge')
                _,cond,positive=edges[0]
                terms.append((digest,self.edge_condition(src,cond,positive)))
            rhs='PHI'+rhs.split(' [',1)[0][3:]+repr(sorted(terms))
        elif v in self.handles:
            kind,rid,index,nonuniform=self.handles[v]
            rhs='HANDLE'+repr((kind,self.resources[kind,rid],index,nonuniform))
        else:
            m=re.search(r'@dx.op.loadInput\.\w+\(i32 4, i32 (\d+), i32 (\d+), i8 (\d+),',rhs)
            if m:
                sid,row,col=map(int,m.groups());f=self.ins[sid]
                indices=re.findall(r'i32 (\d+)',self.md[f[4][1:]])
                if row>=len(indices) or col>=int(f[7][3:]):raise ValueError('invalid semantic component')
                contract=(f[1],f[2],f[3],indices[row],col)
                rhs=rhs[:m.start()]+'INPUT'+repr(contract)+rhs[m.end():]
            rhs=self.named_material_load(rhs)
            rhs=var.sub(lambda m:'['+self.digest(m[0])+']',rhs)
        value=hashlib.sha256(rhs.encode()).hexdigest()
        self.active.remove(v);self.cache[v]=value
        return value

    def edge_condition(self,src,condition,positive):
        reach=self.reach(src)
        if condition is None:return reach
        value=self.digest(condition)
        return hashlib.sha256(repr(('AND',reach,value,positive)).encode()).hexdigest()

    def reach(self,block):
        if block=='0':return 'ENTRY'
        if block in self.reach_cache:return self.reach_cache[block]
        if block in self.reach_active:raise ValueError('cyclic control flow requires loop mapping')
        self.reach_active.add(block)
        incoming=self.predecessors.get(block,[])
        if not incoming:raise ValueError('unknown block predecessor')
        terms=sorted(set(self.edge_condition(*edge) for edge in incoming))
        value=terms[0] if len(terms)==1 else hashlib.sha256(repr(('OR',terms)).encode()).hexdigest()
        self.reach_active.remove(block);self.reach_cache[block]=value
        return value

    def position(self):
        oid=next(i for i,f in self.outs.items() if f[1]=='!"SV_Position"')
        roots=self.roots[oid]
        if set(roots)!={0,1,2,3}:raise ValueError('incomplete position')
        return tuple(self.digest(roots[i]) for i in range(4))

    def uncollapsed_position(self):
        """Recognize a common terminal coverage collapse without removing it.

        All four original output phis must choose the same geometry predecessor
        or the literal clip position (0,0,0,1). The original shader, branches and
        outputs stay intact; only the pre-collapse geometry is compared.
        """
        oid=next(i for i,f in self.outs.items() if f[1]=='!"SV_Position"')
        roots=self.roots[oid]
        if set(roots)!={0,1,2,3}:raise ValueError('incomplete position')
        geometry=[];pair=None;join=None
        for col in range(4):
            value=roots[col];rhs=self.defs.get(value,'')
            if not rhs.startswith('phi float '):raise ValueError('no terminal position collapse')
            incoming=re.findall(r'\[ ([^,\]]+), %([\w.]+) \]',rhs)
            literal='1.000000e+00' if col==3 else '0.000000e+00'
            collapsed=[(v,src) for v,src in incoming if v==literal]
            visible=[(v,src) for v,src in incoming if v.startswith('%')]
            if len(incoming)!=2 or len(collapsed)!=1 or len(visible)!=1:
                raise ValueError('position collapse has nonliteral or multiple branches')
            current_pair=(collapsed[0][1],visible[0][1]);current_join=self.value_block[value]
            if pair is not None and (pair!=current_pair or join!=current_join):
                raise ValueError('position components have different coverage decisions')
            pair,join=current_pair,current_join;geometry.append(visible[0][0])
        collapsed,visible=pair
        if self.blocks.get(collapsed)!=[(join,None,True)]:
            raise ValueError('collapse predecessor has additional control flow')
        edges=self.blocks.get(visible,[])
        if len(edges)!=2 or {dst for dst,_,_ in edges}!={collapsed,join}:
            raise ValueError('coverage decision is not one terminal branch')
        conditions={condition for _,condition,_ in edges}
        if len(conditions)!=1 or None in conditions:
            raise ValueError('coverage branch condition is ambiguous')
        # Geometry roots must be available on the visible predecessor, not
        # computed only on an unrelated path behind the output phi.
        if any(self.value_block.get(v)!=visible for v in geometry):
            raise ValueError('coverage geometry is not defined before its decision')
        return geometry,dict(condition=next(iter(conditions)),visible_predecessor=visible,
                             collapsed_predecessor=collapsed,join=join,
                             original_coverage_preserved=True)

    def position_graph(self,supplied_roots=None):
        """Exact graph with explicit predecessor edges, including loops.

        Canonical traversal renames nodes; it never unrolls or approximates a loop.
        Commutative operands and paired phi inputs have a canonical order.
        """
        if supplied_roots is None:
            oid=next(i for i,f in self.outs.items() if f[1]=='!"SV_Position"')
            if set(self.roots[oid])!={0,1,2,3}:raise ValueError('incomplete position')
            supplied_roots=[self.roots[oid][i] for i in range(4)]
        def describe(key):
            kind=key[0]
            if kind=='reach':
                block=key[1]
                if block in ('0',self.entry_block):return 'ENTRY',[]
                incoming=self.predecessors.get(block,[])
                if not incoming:raise ValueError('unknown block predecessor')
                return 'REACH',[('edge',src,block) for src,_,_ in incoming]
            if kind=='edge':
                src,dst=key[1:]
                edges=[e for e in self.blocks.get(src,[]) if e[0]==dst]
                if len(edges)!=1:raise ValueError('ambiguous control edge')
                _,condition,positive=edges[0]
                return ('EDGE',positive,condition is not None), [('reach',src)]+([('value',condition)] if condition is not None else [])
            v=key[1]
            if not v.startswith('%'):return ('LITERAL',v),[]
            if v not in self.defs:raise ValueError('unresolved value in position graph')
            rhs=self.defs[v]
            if rhs.startswith('phi '):
                incoming=re.findall(r'\[ ([^,\]]+), %([\w.]+) \]',rhs)
                if not incoming:raise ValueError('unsupported phi syntax')
                edges=[]
                for value,src in incoming:edges.extend([('value',value),('edge',src,self.value_block[v])])
                return ('PHI',rhs.split(' [',1)[0],len(incoming)),edges
            if v in self.handles:
                k,rid,index,nonuniform=self.handles[v]
                return ('HANDLE',k,self.resources[k,rid],index,nonuniform),[]
            m=re.search(r'@dx.op.loadInput\.\w+\(i32 4, i32 (\d+), i32 (\d+), i8 (\d+),',rhs)
            if m:
                sid,row,col=map(int,m.groups());f=self.ins[sid]
                indices=re.findall(r'i32 (\d+)',self.md[f[4][1:]])
                if row>=len(indices) or col>=int(f[7][3:]):raise ValueError('invalid semantic component')
                contract=(f[1],f[2],f[3],indices[row],col)
                rhs=rhs[:m.start()]+'INPUT'+repr(contract)+rhs[m.end():]
            rhs=self.named_material_load(rhs)
            binary=re.fullmatch(r'(f(?:add|mul|sub) fast float) ([^,]+), (.+)',rhs)
            if binary:
                return binary[1]+' @VALUE, @VALUE',[('value',binary[2]),('value',binary[3])]
            mad=re.fullmatch(r'(call float @dx.op.tertiary.f32\(i32 46), float ([^,]+), float ([^,]+), float ([^)]+)\)',rhs)
            if mad:
                return mad[1]+', float @VALUE, float @VALUE, float @VALUE)',[('value',mad[i]) for i in (2,3,4)]
            dependencies=var.findall(rhs)
            return var.sub('@VALUE',rhs),[('value',d) for d in dependencies]
        keys=[];ids={};nodes=[]
        def add(key):
            if key[0]=='value':
                value=self.normalized_value(key[1])
                if value!=key[1]:return add(('value',value))
            if key not in ids:
                ids[key]=len(keys);keys.append(key)
            return ids[key]
        roots=[add(('value',value)) for value in supplied_roots]
        index=0
        while index<len(keys):
            if len(keys)>100000:raise ValueError('position graph exceeds offline bound')
            label,edges=describe(keys[index]);nodes.append([label,[add(e) for e in edges]])
            index+=1
        # Order only semantically unordered operands/phi pairs. Fingerprints are
        # sorting hints; final matching still compares the complete canonical graph.
        labels=[json.dumps(n[0],separators=(',',':')).encode() for n in nodes]
        colors=[zlib.crc32(x) for x in labels]
        def ordered(label,edges,colors):
            if isinstance(label,(tuple,list)) and label[0]=='PHI':
                pairs=[edges[i:i+2] for i in range(0,len(edges),2)]
                return [x for pair in sorted(pairs,key=lambda pair:tuple(colors[v] for v in pair)) for x in pair]
            if label in ('fmul fast float @VALUE, @VALUE','fadd fast float @VALUE, @VALUE'):
                return sorted(edges,key=lambda v:colors[v])
            if isinstance(label,str) and label.startswith('call float @dx.op.tertiary.f32(i32 46,'):
                return sorted(edges[:2],key=lambda v:colors[v])+edges[2:]
            return edges
        for _ in range(16):
            colors=[zlib.crc32(labels[i]+b''.join(struct.pack('<I',colors[v]) for v in ordered(n[0],n[1],colors))) for i,n in enumerate(nodes)]
        nodes=[[label,ordered(label,edges,colors)] for label,edges in nodes]
        order=[];remap={}
        def include(old):
            if old not in remap:remap[old]=len(order);order.append(old)
            return remap[old]
        roots=[include(old) for old in roots];new_nodes=[];index=0
        while index<len(order):
            label,edges=nodes[order[index]];new_nodes.append([label,[include(v) for v in edges]]);index+=1
        keys=[keys[old] for old in order];nodes=new_nodes
        self.graph_data=(roots,nodes)
        return hashlib.sha256(json.dumps([roots,nodes],separators=(',',':')).encode()).hexdigest(),keys

    def dependencies(self,oid):
        return self.dependencies_values(self.roots[oid].values())

    def dependencies_values(self,roots):
        seen=set();todo=list(roots);cb=set();inputs=set();other=[]
        while todo:
            v=todo.pop()
            if v in seen or v not in self.defs:continue
            seen.add(v);rhs=self.defs[v];todo.extend(var.findall(rhs))
            m=re.search(r'@dx.op.cbufferLoadLegacy.\w+\(i32 59, %dx.types.Handle (%\d+), i32 (\d+)\)',rhs)
            if m:
                h=self.handles.get(m[1]);cb.add((h[2] if h else -1,int(m[2])))
            m=re.search(r'@dx.op.loadInput.\w+\(i32 4, i32 (\d+), i32 (\d+), i8 (\d+),',rhs)
            if m:inputs.add((self.ins[int(m[1])][1].strip('!"'),int(m[2]),int(m[3])))
            if '@dx.op.bufferLoad' in rhs or '@dx.op.sample' in rhs or '@dx.op.textureLoad' in rhs:other.append(rhs)
        return dict(cb_rows=sorted(cb),inputs=sorted(inputs),resource_reads=len(other),instructions=len(seen))

def main():
    native=json.loads((p/'opaque-velocity-audit/index.json').read_text())['shaders']
    transparent=json.loads((p/'all-transparent-input-routes.json').read_text())['shaders']
    contracts=json.loads((p/'shader-modifier-contracts.json').read_text())['shaders']
    groups=defaultdict(list);reject=Counter();natives=[]
    for r in native:
        try:
            s=Shader(Path(r['disassembly']).read_text(),contracts.get(r['sha256']));key,graph_keys=s.position_graph()
            scalar=defaultdict(list)
            for oid,roots in s.roots.items():
                for col,value in roots.items():
                    d=s.dependencies_values([value]);cb=set(map(tuple,d['cb_rows']))
                    old={row for binding,row in cb if binding==1 and 16<=row<20}
                    current={row for binding,row in cb if binding==1 and 28<=row<32}
                    if len(old)==1 and not current:scalar[next(iter(old))-16].append((oid,col,value))
            if set(scalar)!={0,1,2,3}:raise ValueError('no complete native prior-projection output')
            if any(len({value for _,_,value in rows})!=1 for rows in scalar.values()):
                raise ValueError('ambiguous prior-projection component')
            components=[list(scalar[i][0][:2]) for i in range(4)]
            d=s.dependencies_values([scalar[i][0][2] for i in range(4)])
            prior=[dict(components=components,dependencies=d)]
            n=dict(sha256=r['sha256'],previous=prior,techniques=r['techniques'])
            groups[key].append(n);natives.append(n)
            # Specialize a native material amplitude to the target's literal one
            # only when the entire resulting position graph matches exactly.
            # No value is sampled or guessed from a frame/image.
            values={item[1] for item in graph_keys if item[0]=='value' and item[1] in s.defs}
            multiplied={v for value in values if s.defs[value].startswith('fmul fast float ')
                        for v in var.findall(s.defs[value])}
            eligible=[]
            for value in values&multiplied:
                m=re.fullmatch(r'extractvalue %dx.types.CBufRet.f32 (%\d+), [0-3]',s.defs[value])
                if not m:continue
                load=s.defs.get(m[1],'')
                h=re.search(r'@dx.op.cbufferLoadLegacy.f32\(i32 59, %dx.types.Handle (%\d+), i32 \d+\)',load)
                if h and s.handles.get(h[1],())[:1]==(2,) and s.handles[h[1]][2]==4:eligible.append(value)
            if len(eligible)>8:continue
            for value in sorted(eligible):
                s.specializations={value:'1.000000e+00'};special_key,_=s.position_graph()
                if special_key!=key:groups[special_key].append(dict(n,specializations=dict(s.specializations)))
        except (ValueError,KeyError,StopIteration,TypeError,AttributeError) as e:reject[str(e)]+=1
    matches=[];miss=[]
    for r in transparent:
        try:
            s=Shader((p/'all-transparent-vs'/(r['sha256']+'.ll')).read_text(),contracts.get(r['sha256']));key,_=s.position_graph()
            found=groups.get(key,[])
            coverage=None
            if not found:
                try:
                    geometry,coverage=s.uncollapsed_position();key,_=s.position_graph(geometry)
                    found=groups.get(key,[])
                except ValueError:pass
            if not found:raise ValueError('no exact native current-position match')
            matches.append(dict(sha256=r['sha256'],techniques=r['techniques'],native_candidates=found,
                                coverage_guard=coverage))
        except (ValueError,KeyError,StopIteration,TypeError,AttributeError) as e:
            miss.append(dict(sha256=r['sha256'],reason=str(e),techniques=r['techniques']))
    summary=dict(native_total=len(native),native_current_compared=len(natives),native_rejected=dict(reject),
        transparent_total=len(transparent),transparent_matched=len(matches),unmatched=dict(Counter(x['reason'] for x in miss)),
        native_templates=sum(map(len,groups.values())),
        coverage_preserved_matches=sum(bool(r['coverage_guard']) for r in matches),
        method='Exact position/control graph with named rows and validated unit-amplitude specialization; no material-name matching',
        live_bindings_verified=False,grafted=False)
    (p/'shared-native-motion-matches.json').write_text(json.dumps(dict(summary=summary,matches=matches,unmatched=miss),indent=2))
    print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
