#!/usr/bin/env python3
"""Resolve and verify archived generic inputs into a portable fresh runner manifest."""
from pathlib import Path
import argparse,hashlib,json
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=a.run.resolve();manifest=json.loads((root/'manifest.json').read_text());files=json.loads((root/'artifacts/input-files.json').read_text())
if a.output.exists():p.error('Use a fresh output directory')
by={(f['workload'],f['field']):f for f in files};cases=[]
for original in manifest['cases']:
 case=dict(original)
 for field in ('graph','shots'):
  record=by[case['name'],field];path=(root/record['archived_path']).resolve()
  if root not in path.parents:raise ValueError('Archived input escapes the run')
  if hashlib.sha256(path.read_bytes()).hexdigest()!=record['sha256']:raise ValueError('Archived input hash mismatch')
  case[field]=str(path)
 cases.append(case)
a.output.mkdir(parents=True);(a.output/'manifest.json').write_text(json.dumps(cases,indent=2)+'\n')
print(a.output/'manifest.json')
