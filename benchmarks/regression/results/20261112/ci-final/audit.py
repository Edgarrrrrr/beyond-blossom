#!/usr/bin/env python3
"""Audit saved final GitHub status/log bytes without network access or tests."""
from pathlib import Path
import argparse
import hashlib
import json
import re

COMMIT='d2506bde8ee9a2eba7ad97523fbc948a97142676'
RUN=43094516453
JOB=130779786311

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def need(condition,message):
    if not condition:raise ValueError(message)

def audit(root):
    run=json.loads((root/'run.json').read_text())
    jobs=json.loads((root/'jobs.json').read_text())
    log=root/f'job-{JOB}.log'
    text=log.read_text();lines=text.splitlines()
    need(run['id']==RUN and run['head_sha']==COMMIT,'Run identity mismatch')
    need(run['status']=='completed' and run['conclusion']=='success','Run not final success')
    need(jobs['total_count']==len(jobs['jobs'])==1,'Unexpected job matrix')
    job=jobs['jobs'][0]
    need(job['id']==JOB and job['run_id']==RUN and job['head_sha']==COMMIT,'Job identity mismatch')
    need(job['status']=='completed' and job['conclusion']=='success','Job not final success')
    need(len(job['steps'])==12 and all(x['status']=='completed' and x['conclusion']=='success' for x in job['steps']),'Job step is not successful')
    def evidence(pattern):
        found=[{'line':i,'text':line} for i,line in enumerate(lines,1) if re.search(pattern,line)]
        need(len(found)==1,'Expected one log match: '+pattern)
        return found[0]
    checked_out_commit=evidence(r'^\S+ '+COMMIT+r'$')
    version=evidence(r'c\+\+ \(Ubuntu 15\.2\.0-16ubuntu1\) 15\.2\.0')
    target_command=evidence(r'c\+\+ -Q --help=target')
    arch=evidence(r'-march=\s+x86-64-v3\s*$')
    popcnt=evidence(r'-mpopcnt\s+\[enabled\]')
    python=evidence(r'platform linux -- Python 3\.11\.18, pytest-9\.2\.0')
    collected=evidence(r'collected 250 items')
    pytest=evidence(r'248 passed, 2 skipped, 3 warnings in 8\.83s')
    ctest=evidence(r'100% tests passed out of 356')
    elapsed=evidence(r'Total Test time \(real\) = 250\.08 sec')
    records=[]
    for i,line in enumerate(lines,1):
        m=re.search(r'\s(\d+)/356 Test\s+#(\d+):\s+(.*?)\s+\.{2,}\s*(Passed|\*\*\*Skipped)\s+([\d.]+) sec',line)
        if m:records.append({'line':i,'ordinal':int(m[1]),'test_number':int(m[2]),'name':m[3],'status':m[4],'seconds':float(m[5])})
    need(len(records)==356 and {r['ordinal'] for r in records}==set(range(1,357)) and
         {r['test_number'] for r in records}==set(range(1,357)),'Missing or duplicated CTest result rows')
    passed=[r for r in records if r['status']=='Passed'];skipped=[r for r in records if r['status']=='***Skipped']
    need(len(passed)==355 and len(skipped)==1 and skipped[0]['test_number']==20,'Unexpected CTest status accounting')
    skipped_footer=evidence(r'20 - DecoderStrategy\.ParallelSessionEmptyOddExceptionalStreamsAndCapabilityBoundaries \(Skipped\)')
    need(log.stat().st_size==188833,'Final full job log byte count differs')
    skip_reason_lines=[{'line':i,'text':line} for i,line in enumerate(lines,1) if re.search(r'\bSKIPPED\s*\[|short test summary info|skip reason',line,re.I)]
    return {
        'schema':1,'complete':True,'kind':'independent saved-evidence audit; no CI rerun',
        'run_id':RUN,'job_id':JOB,'head_sha':COMMIT,'url':run['html_url'],'log_checked_out_commit':checked_out_commit,
        'run':{'status':run['status'],'conclusion':run['conclusion'],'updated_at':run['updated_at']},
        'job':{'status':job['status'],'conclusion':job['conclusion'],'completed_at':job['completed_at'],'successful_steps':len(job['steps'])},
        'compiler':{'version':'GNU C++ 15.2.0 (Ubuntu 15.2.0-16ubuntu1)',
                    'default_target_query':'c++ -Q --help=target','default_march':'x86-64-v3','default_popcnt':'enabled',
                    'evidence':[version,target_command,arch,popcnt]},
        'python':{'version':'3.11.18','pytest':'9.2.0','collected':250,'passed':248,'skipped':2,'warnings':3,'elapsed_seconds':8.83,
                  'skip_reasons':'not reported in the captured job log; no reason or individual skipped test IDs inferred',
                  'skip_reason_lines':skip_reason_lines,'evidence':[python,collected,pytest]},
        'ctest':{'scheduled':356,'passed':355,'skipped':1,'failed':0,'elapsed_seconds':250.08,'individual_result_rows':len(records),
                 'evidence':[ctest,elapsed,skipped_footer],'skipped_tests':skipped,'skip_reason':'not reported in the captured job log',
                 'passed_test_numbers':[r['test_number'] for r in passed],
                 'summary_qualification':'The CTest footer says 100% tests passed out of 356, but explicitly lists test20 as did not run; do not describe this as 356 executed passes.'},
        'source_files':{p.name:{'bytes':p.stat().st_size,'sha256':sha(p)} for p in (root/'run.json',root/'jobs.json',log)},
        'scope':'This final hosted run is distinct from the earlier pending observation and the local 250-pass/zero-skip package audit. No performance measurement or general compiler-equivalence claim is made.'}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parent)
    parser.add_argument('--check',action='store_true',help='compare with audit.json instead of writing it')
    args=parser.parse_args();result=audit(args.root)
    if args.check:need(json.loads((args.root/'audit.json').read_text())==result,'Stored audit differs')
    else:(args.root/'audit.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'complete':True,'run':RUN,'job':JOB,'compiler':'GCC 15.2.0','python_passed':248,'python_skipped':2,'ctest_passed':355,'ctest_skipped':1,'raw_log_bytes':188833}))
