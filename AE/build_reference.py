#!/usr/bin/env python3
"""Build the fixed independent reference from archived source and record its identity."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import platform
import subprocess
import sys
import tarfile

HERE=Path(__file__).resolve().parent
REVISION='e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    history=p.add_mutually_exclusive_group(required=True)
    history.add_argument('--history-archive',type=Path)
    history.add_argument('--history-checkout',type=Path)
    p.add_argument('--history-ref',help='Historical tag/ref in the supplied history checkout')
    p.add_argument('--upstream-repository',type=Path,required=True,help='Git repository containing the pinned upstream revision')
    p.add_argument('--stim-source',type=Path,required=True)
    p.add_argument('--stim-archive',type=Path,required=True)
    p.add_argument('--googletest-source',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cache-level',choices=['2','3'],default='3')
    p.add_argument('--jobs',type=int,default=4)
    p.add_argument('--cxx-flags',default='-O3')
    p.add_argument('--cmake-arg',action='append',default=[])
    args=p.parse_args()
    root=args.output.resolve()
    if root.exists():p.error('Use a fresh output directory')
    if HERE.parent in root.parents:p.error('Keep recovered historical source outside the published repository')
    for path in [args.stim_source/'CMakeLists.txt',args.stim_archive,args.googletest_source/'CMakeLists.txt']:
        if not path.is_file():p.error(f'Missing fixed dependency: {path}')
    stim_source_files={str(f.relative_to(args.stim_source)):digest(f) for f in sorted((args.stim_source/'src').rglob('*')) if f.is_file()}
    archive=subprocess.check_output(['git','-C',str(args.upstream_repository),'archive',REVISION])
    root.mkdir(parents=True)
    source=root/'upstream';source.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as t:
        for item in t:
            target=(source/item.name).resolve()
            if source not in target.parents:raise ValueError('Unsafe source path')
            if item.isdir():target.mkdir(parents=True,exist_ok=True)
            elif item.isfile():
                target.parent.mkdir(parents=True,exist_ok=True)
                target.write_bytes(t.extractfile(item).read())
    option='--archive' if args.history_archive else '--checkout'
    historical=args.history_archive or args.history_checkout
    subprocess.run([sys.executable,str(HERE/'recover_history.py'),option,str(historical),'--output',str(root/'tools'),*(['--ref',args.history_ref] if args.history_ref else [])],check=True)
    from portable_native import adapt
    native=root/'tools/AE/pmu_native'
    adapt(native)
    configure=['cmake','-S',str(native),'-B',str(root/'build'),
               '-DPYMATCHING_SOURCE_DIR='+str(source),'-DFETCHCONTENT_SOURCE_DIR_STIM='+str(args.stim_source.resolve()),
               '-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST='+str(args.googletest_source.resolve()),
               '-DCACHED_STIM_ARCHIVE='+str(args.stim_archive.resolve()),'-DSTENCIL_GENERIC_ONLY=ON',
               '-DEXPERIMENT_ENABLE_SHARED=OFF','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF',
               '-DCMAKE_CXX_FLAGS='+args.cxx_flags+' -DPM_PMU_CACHE_LEVEL='+args.cache_level,*args.cmake_arg]
    build=['cmake','--build',str(root/'build'),'--target','experiment_benchmark','-j',str(args.jobs)]
    for name,command in [('configure',configure),('build',build)]:
        (root/(name+'-command.json')).write_text(json.dumps(command,indent=2)+'\n')
        with (root/(name+'.log')).open('x') as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    binary=root/'build/bin/experiment_benchmark'
    if any(digest(args.stim_source/name)!=sha for name,sha in stim_source_files.items()):
        raise ValueError('Stim source changed during reference compilation')
    receipt=dict(schema=1,complete=True,upstream_revision=REVISION,source_archive_sha256=hashlib.sha256(archive).hexdigest(),
                 binary=dict(path=str(binary),sha256=digest(binary)),
                 matching_library=dict(path=str(root/"build/pymatching/libpymatching.a"),sha256=digest(root/"build/pymatching/libpymatching.a")),
                 source_files={str(f.relative_to(source)):digest(f) for f in sorted((source/"src").rglob("*")) if f.is_file()},
                 source_license_sha256=digest(source/"LICENSE"),
                 stim_archive_sha256=digest(args.stim_archive),
                 stim_source_files=stim_source_files,
                 historical_manifest_sha256=digest(HERE/'historical_sources.json'),
                 adapter_files={str(f.relative_to(native)):digest(f) for f in native.iterdir() if f.is_file()},
                 builder_sha256=digest(__file__),portability_sha256=digest(HERE/'portable_native.py'),
                 platform=platform.platform(),lto=False,cache_level=args.cache_level)
    (root/'reference.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(binary)
    print(root/'reference.json')

if __name__=='__main__':main()
