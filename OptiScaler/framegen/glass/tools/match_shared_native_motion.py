"""Match original position arithmetic across transparent and native velocity VS.

Uses semantic inputs and referenced resource contracts, not material names.
Rejects control-dependent position values. Matches are offline graft candidates;
they do not prove live resource contents, grouped identity, or temporal validity.
"""
from pathlib import Path
from collections import Counter, defaultdict
import hashlib, json, re

import argparse
_parser=argparse.ArgumentParser(description=__doc__)
_parser.add_argument('--workspace', type=Path, required=True)
p=_parser.parse_args().workspace.resolve(strict=True)
var=re.compile(r'%(?:\d+|graft[\w.]*)\b')

class Shader:
    def __init__(self,text):
        self.text=text
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
        block='0'
        for line in text.splitlines():
            label=re.match(r'; <label>:(\d+)',line)
            if label:block=label[1]
            value=re.match(r'  (%\d+) = ',line)
            if value:self.value_block[value[1]]=block
            branch=re.match(r'  br i1 ([^,]+), label %(\d+), label %(\d+)',line)
            direct=re.match(r'  br label %(\d+)',line)
            if branch:
                self.blocks[block]=[(branch[2],branch[1],True),(branch[3],branch[1],False)]
            elif direct:self.blocks[block]=[(direct[1],None,True)]
        for src,edges in self.blocks.items():
            for dst,condition,positive in edges:self.predecessors[dst].append((src,condition,positive))
        for v,rhs in self.defs.items():
            m=re.search(r'@dx.op.createHandle\(i32 57, i8 (\d+), i32 (\d+), i32 (\d+), i1 (true|false)\)',rhs)
            if m:self.handles[v]=(int(m[1]),int(m[2]),int(m[3]),m[4])

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

    def dependencies(self,oid):
        seen=set();todo=list(self.roots[oid].values());cb=set();inputs=set();other=[]
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
    groups=defaultdict(list);reject=Counter();natives=[]
    for r in native:
        try:
            s=Shader(Path(r['disassembly']).read_text());key=s.position()
            prior=[]
            for oid,roots in s.roots.items():
                if set(roots)!={0,1,2,3}:continue
                d=s.dependencies(oid)
                if all((1,row) in d['cb_rows'] for row in range(16,20)):
                    prior.append(dict(output=oid,semantic=s.outs[oid][1],dependencies=d))
            if not prior:raise ValueError('no complete native prior-projection output')
            n=dict(sha256=r['sha256'],previous=prior,techniques=r['techniques'])
            groups[key].append(n);natives.append(n)
        except (ValueError,KeyError,StopIteration,TypeError,AttributeError) as e:reject[str(e)]+=1
    matches=[];miss=[]
    for r in transparent:
        try:
            s=Shader((p/'all-transparent-vs'/(r['sha256']+'.ll')).read_text());key=s.position()
            found=groups.get(key,[])
            if not found:raise ValueError('no exact native current-position match')
            matches.append(dict(sha256=r['sha256'],techniques=r['techniques'],native_candidates=found))
        except (ValueError,KeyError,StopIteration,TypeError,AttributeError) as e:
            miss.append(dict(sha256=r['sha256'],reason=str(e),techniques=r['techniques']))
    summary=dict(native_total=len(native),native_current_compared=len(natives),native_rejected=dict(reject),
        transparent_total=len(transparent),transparent_matched=len(matches),unmatched=dict(Counter(x['reason'] for x in miss)),
        method='Exact referenced arithmetic after semantic/range id remapping; no material-name matching',
        live_bindings_verified=False,grafted=False)
    (p/'shared-native-motion-matches.json').write_text(json.dumps(dict(summary=summary,matches=matches,unmatched=miss),indent=2))
    print(json.dumps(summary,indent=2))

if __name__=='__main__':main()
