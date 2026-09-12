"""Check exact original-output and transplanted native-expression preservation."""
from pathlib import Path
import json,re
from match_shared_native_motion import Shader
import argparse
_parser=argparse.ArgumentParser(description=__doc__)
_parser.add_argument('--workspace', type=Path, required=True)
p=_parser.parse_args().workspace.resolve(strict=True)
index=json.loads((p/'native-grafted/index.json').read_text())
matches={r['sha256']:r for r in json.loads((p/'shared-native-motion-matches.json').read_text())['matches']}
results=[]
for r in index['shaders']:
    if r['status']!='validated':continue
    original=(p/'all-transparent-vs'/(r['sha256']+'.ll')).read_text()
    grafted=(p/'native-grafted'/(r['sha256']+'.ll')).read_text()
    native=(p/'opaque-velocity-audit'/(r['native_sha256']+'.ll')).read_text()
    a,b=Shader(original),Shader(grafted)
    # Original definitions are unchanged, and each original store still exports
    # the very same SSA value. Metadata changes only add the required usage.
    assert all(b.defs.get(v)==rhs for v,rhs in a.defs.items())
    assert all(b.roots[oid]==roots for oid,roots in a.roots.items())
    src=Shader(native)
    handles={v for v,h in src.handles.items() if h[0]==2 and h[2]==7}
    def relocate(m):
        if m[2] not in handles:return m[0]
        row=int(m[3])
        if row not in r['original_motion_rows']:return m[0]
        return m[1]+m[2]+', i32 '+str(24+row-r['original_motion_rows'][0])+')'
    native=re.sub(r'(@dx.op.cbufferLoadLegacy.\w+\(i32 59, %dx.types.Handle )(%\d+), i32 (\d+)\)',relocate,native)
    reference=Shader(native)
    route=next(n for n in matches[r['sha256']]['native_candidates'] if n['sha256']==r['native_sha256'])
    oid=route['previous'][0]['output']
    expected=[reference.digest(reference.roots[oid][i]) for i in range(4)]
    actual=[b.digest(b.roots[r['previous_output']][i]) for i in range(4)]
    assert actual==expected, r['sha256']
    results.append(dict(sha256=r['sha256'],original_outputs_unchanged=True,native_previous_expression_identical=True))
result=dict(checked=len(results),passed=len(results),scope='Exact expression and original-output checks; not live material supply',results=results)
(p/'native-grafted/verification.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:result[k] for k in ('checked','passed','scope')}))
