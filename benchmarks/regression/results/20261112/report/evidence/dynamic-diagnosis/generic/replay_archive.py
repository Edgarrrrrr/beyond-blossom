#!/usr/bin/env python3
"""Replay archived summaries at a fresh temporary path; no native invocation."""
from pathlib import Path, PurePosixPath
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import tempfile


CONFIG={
    'structured':dict(directory='/home/fujy/dev/beyond_blossom/validation/20261112/dynamic-diagnosis',
                      subtree='dynamic-diagnosis/prepared-54-r4', controller='controller/run_window_pmu.py', outputs=['summary.json']),
    'generic':dict(directory='/home/fujy/dev/beyond_blossom/validation/20261112/generic-dynamic-diagnosis',
                   subtree='generic-dynamic-diagnosis/full-r2-frozen-01', controller='controller/run_diagnostics.py',
                   outputs=['summary.json','process-medians.json','raw.csv']),
}


def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for data in iter(lambda:f.read(1024*1024),b''):h.update(data)
    return h.hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--kind',choices=CONFIG,required=True);args=p.parse_args()
    config=CONFIG[args.kind];destination=Path(config['directory']);archive=destination/'archive.tar.gz'
    receipt=destination/'replay-verification.json';assert not receipt.exists(),'Previous replay receipt must not be overwritten'
    manifest=json.loads((destination/'manifest.json').read_text());assert sha(archive)==manifest['archive_sha256']
    index=json.loads((destination/'path-map.json').read_text());entries={x['archive_path']:x for x in index['files']}
    assert sha(destination/'path-map.json')==manifest['path_map_sha256']
    count=0;before={};after={};result=None;command=None
    with tempfile.TemporaryDirectory(prefix='beyond-'+args.kind+'-archive-replay-') as temporary:
        target=Path(temporary);prefix=config['subtree']
        # Extract only the already frozen run needed by summary-only. This skips
        # redundant older builds/source trees and is removed after verification.
        with tarfile.open(archive,'r|gz') as tar:
            for member in tar:
                if member.name!=prefix and not member.name.startswith(prefix+'/'):continue
                assert '..' not in PurePosixPath(member.name).parts and not member.name.startswith('/')
                entry=entries[member.name];path=target/member.name
                if member.isdir():path.mkdir(parents=True,exist_ok=True)
                elif member.isfile():
                    path.parent.mkdir(parents=True,exist_ok=True)
                    with path.open('xb') as f:shutil.copyfileobj(tar.extractfile(member),f)
                    path.chmod(member.mode)
                    assert path.stat().st_size==entry['bytes'] and sha(path)==entry['sha256'];count+=1
                else:raise ValueError('Unexpected link in replay-only subtree')
        run=target/prefix
        before={name:sha(run/name) for name in config['outputs']}
        command=['/usr/bin/python3',str(run/config['controller']),'--summarize-only','--output',str(run)]
        env=os.environ.copy();env['PYTHONDONTWRITEBYTECODE']='1'
        with (destination/'replay.log').open('x') as log:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env=env,timeout=180)
        after={name:sha(run/name) for name in config['outputs']}
        same=before==after
        report=dict(schema=1,kind=args.kind,complete=result.returncode==0 and same,created_utc=datetime.now(timezone.utc).isoformat(),
                    archive_sha256=manifest['archive_sha256'],archive_manifest_sha256=sha(destination/'manifest.json'),
                    command=command,returncode=result.returncode,extracted_regular_files=count,
                    before_sha256=before,after_sha256=after,byte_identical=same,native_execution=False,
                    extraction_scope='Only frozen acquisition subtree; temporary extraction removed after replay',
                    replay_script_sha256=sha(__file__),replay_log_sha256=sha(destination/'replay.log'))
    shutil.copyfile(__file__,destination/'replay_archive.py')
    with receipt.open('x') as f:json.dump(report,f,indent=2,allow_nan=False);f.write('\n')
    print(json.dumps(dict(receipt=str(receipt),receipt_sha256=sha(receipt),**report),indent=2))
    assert report['complete'],'Summary-only relocated replay failed; preserve receipt/log'


if __name__=='__main__':main()
