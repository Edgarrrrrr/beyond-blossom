#!/usr/bin/env python3
"""Restore pinned historical acquisition code outside the published source tree."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

HERE=Path(__file__).resolve().parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    source=p.add_mutually_exclusive_group(required=True)
    source.add_argument('--archive',type=Path,help='Source archive of the revision in historical_sources.json')
    source.add_argument('--checkout',type=Path,help='Local source repository containing the recorded revision')
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--ref',help='Local historical ref/tag; original per-file SHA-256 checks still apply')
    args=p.parse_args()
    if args.output.exists():p.error('Use a fresh output directory')
    target=args.output.resolve()
    if HERE.parent in target.parents:p.error('Keep recovered historical source outside the published repository')
    spec=json.loads((HERE/'historical_sources.json').read_text())
    content={}
    if args.archive:
        with tarfile.open(args.archive) as archive:
            members={'/'.join(m.name.split('/')[1:]):m for m in archive if m.isfile()}
            for f in spec['files']:
                content[f['path']]=archive.extractfile(members[f['path']]).read()
    else:
        for f in spec['files']:
            content[f['path']]=subprocess.check_output(['git','-C',str(args.checkout),'show',(args.ref or spec['revision'])+':'+f['path']])
    for f in spec['files']:
        if hashlib.sha256(content[f['path']]).hexdigest()!=f['sha256']:raise ValueError('Historical source hash mismatch: '+f['path'])
    target.mkdir(parents=True)
    for name,data in content.items():
        out=target/name;out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(data)
    print(target)

if __name__=='__main__':main()
