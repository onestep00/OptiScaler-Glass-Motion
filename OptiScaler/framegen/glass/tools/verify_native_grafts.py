"""Check exact original-output and transplanted native-expression preservation."""
from pathlib import Path
import json
from native_graft_checks import verify_graft
import argparse
_parser=argparse.ArgumentParser(description=__doc__)
_parser.add_argument('--workspace', type=Path, required=True)
p=_parser.parse_args().workspace.resolve(strict=True)
index=json.loads((p/'native-grafted/index.json').read_text())
contracts=json.loads((p/'shader-modifier-contracts.json').read_text())['shaders']
matches={r['sha256']:r for r in json.loads((p/'shared-native-motion-matches.json').read_text())['matches']}
results=[]
for r in index['shaders']:
    if r['status']!='validated':continue
    original=(p/'all-transparent-vs'/(r['sha256']+'.ll')).read_text()
    grafted=(p/'native-grafted'/(r['sha256']+'.ll')).read_text()
    native=(p/'opaque-velocity-audit'/(r['native_sha256']+'.ll')).read_text()
    route=next(n for n in matches[r['sha256']]['native_candidates'] if n['sha256']==r['native_sha256'] and n.get('specializations',{})==r.get('specializations',{}))
    results.append(verify_graft(original, grafted, native, r, route, contracts))
result=dict(checked=len(results),passed=len(results),scope='Exact expression and original-output checks; not live material supply',results=results)
(p/'native-grafted/verification.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:result[k] for k in ('checked','passed','scope')}))
