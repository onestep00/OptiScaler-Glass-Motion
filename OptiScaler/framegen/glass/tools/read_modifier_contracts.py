"""Read original named modifier slots for all catalogued shaders, offline."""
from pathlib import Path
from collections import defaultdict
import argparse,hashlib,json,mmap,struct
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--workspace',type=Path,required=True)
p=parser.parse_args().workspace.resolve(strict=True)
catalog=json.loads((p/'all-cache-techniques.json').read_text())
ids={int(v['cache_identity'],16):v['sha256'] for t in catalog['techniques'] for v in t['programs']}
with Path(catalog['cache_path']).open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
    assert hashlib.sha256(data).hexdigest()==catalog['cache_sha256']
    footer=struct.unpack_from('<8I9Q2I',data,len(data)-112)
    at=footer[14];parameters={}
    for _ in range(footer[2]):
        key,mask,count=struct.unpack_from('<QII',data,at);at+=16
        slots=[]
        for _ in range(count):
            length=data[at]&127;at+=1;name=data[at:at+length].decode();at+=length
            row,kind=data[at:at+2];at+=2
            slots.append(dict(name=name,row=row,kind=kind))
        parameters[key]=dict(key=hex(key),request_mask=mask,slots=slots)
    contracts=defaultdict(dict);at=0
    for _ in range(footer[0]):
        identity,parent,size=struct.unpack_from('<QQI',data,at);at+=20+size
        if identity in ids:
            if parent not in parameters:raise ValueError('missing cache parameter set')
            contracts[ids[identity]][parent]=parameters[parent]
result=dict(cache_sha256=catalog['cache_sha256'],shaders={sha:list(rows.values()) for sha,rows in contracts.items()},
    scope='Original named row metadata; runtime values and temporal ownership are separate')
(p/'shader-modifier-contracts.json').write_text(json.dumps(result,indent=2))
print(json.dumps(dict(shaders=len(contracts),parameter_sets=len(parameters),ambiguous=sum(len(x)>1 for x in contracts.values()))))
