"""Check exact original-output and transplanted native-expression preservation."""
from pathlib import Path
import json
from native_graft_checks import verify_graft, verify_camera_graft
import argparse
_parser=argparse.ArgumentParser(description=__doc__)
_parser.add_argument('--workspace', type=Path, required=True)
_parser.add_argument('--skinned-previous-world', choices=('motion','current'), default='motion',
    help='current checks <workspace>/native-grafted-skinned-current (graft_native_motion.py --skinned-previous-world current)')
_args=_parser.parse_args()
p=_args.workspace.resolve(strict=True)
grafted=p/('native-grafted-skinned-current' if _args.skinned_previous_world=='current' else 'native-grafted')
index=json.loads((grafted/'index.json').read_text())
contracts=json.loads((p/'shader-modifier-contracts.json').read_text())['shaders']
matches={r['sha256']:r for r in json.loads((p/'shared-native-motion-matches.json').read_text())['matches']}
results=[];camera_results=[]
for r in index['shaders']:
    original=(p/'all-transparent-vs'/(r['sha256']+'.ll')).read_text()
    if r.get('camera_status')=='validated':
        camera=(grafted/(r['sha256']+'.camera.ll')).read_text()
        guard=matches[r['sha256']].get('coverage_guard')
        camera_results.append(dict(sha256=r['sha256'],**verify_camera_graft(original,camera,dict(r,coverage_guard=guard))))
    if r['status']!='validated':continue
    grafted_text=(grafted/(r['sha256']+'.ll')).read_text()
    native=(p/'opaque-velocity-audit'/(r['native_sha256']+'.ll')).read_text()
    route=next(n for n in matches[r['sha256']]['native_candidates'] if n['sha256']==r['native_sha256'] and n.get('specializations',{})==r.get('specializations',{}))
    results.append(verify_graft(original, grafted_text, native, r, route, contracts))
generic_results=[]
for r in index.get('generic_camera',[]):
    if r.get('camera_status')!='validated':continue
    original=(p/r['source']).read_text()
    camera=(grafted/(r['sha256']+'.camera.ll')).read_text()
    generic_results.append(dict(sha256=r['sha256'],**verify_camera_graft(original,camera,r)))
result=dict(checked=len(results),passed=len(results),camera_checked=len(camera_results),camera_passed=len(camera_results),
    generic_checked=len(generic_results),generic_passed=len(generic_results),
    scope='Exact expression and original-output checks; not live material supply',results=results,camera_results=camera_results,
    generic_results=generic_results)
(grafted/'verification.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:result[k] for k in ('checked','passed','camera_checked','camera_passed','generic_checked','generic_passed','scope')}))
