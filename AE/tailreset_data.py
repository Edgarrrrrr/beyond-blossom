#!/usr/bin/env python3
"""Bundle or verify completed TailReset time/residual results without decoder dependencies."""
import argparse
import csv
import json
import math
from pathlib import Path
import shutil
import tempfile
import pmu_runtime as R

HERE=Path(__file__).resolve().parent
METHODS=('skip','coordinate','degree','Official')
DISTANCES=(9,13,17,21,25,29,33)
LARGE={'native_shots.csv','native_blocks.csv','input_features.csv','syndromes.csv'}


def need(value,message): R.need(value,message)


def relative(root,name):
    path=(root/name).resolve()
    need(root.resolve() in path.parents,'Bundle path escapes its root')
    return path


def verify(source):
    source=Path(source).resolve(); root=source.parent
    manifest=R.load(source)
    need(manifest['complete'] and manifest['schema']==1,'Incomplete TailReset bundle')
    files={}
    for original,ref in manifest['files'].items():
        path=relative(root,ref['path'])
        R.ref(path,ref['sha256']); files[original]=path
    def mapped(ref):
        need(ref['path'] in files,'Missing bundled evidence: '+ref['path'])
        path=files[ref['path']]
        need(R.sha(path)==ref['sha256'],'Evidence chain differs')
        return path
    plan=R.load(mapped(manifest['plan']))
    summary=R.load(mapped(manifest['summary']))
    freeze=R.load(files[manifest['run']+'/freeze.json'])
    need(freeze['plan']==manifest['plan'],'Plan freeze differs')
    complete=R.load(files[manifest['run']+'/complete.json'])
    need(complete['complete'] and complete['summary']==manifest['summary'],'Run completion differs')
    need(summary['complete'] and summary['formal_calls']==133 and summary['independent_inputs']==7_000_000 and summary['weight_differences']==0,'Incomplete formal matrix')
    need(summary['implementation_commit']==plan['implementation_commit'] and summary['acquisition_commit']==plan['acquisition_commit'],'Source identity differs')
    protocol=plan['protocol']
    need(protocol['shots']==1_000_000 and protocol['batch_repeats']==3 and protocol['latency_repeats']==1 and protocol['statistics_repeats']==1,'Wrong sample/repeat scope')
    need(protocol['lto']=='OFF' and protocol['timing_statistics_macro']==0 and protocol['statistics_macro']==1 and protocol['exclusive'],'Wrong build/measurement separation')
    need([c['d'] for c in plan['cases']]==list(DISTANCES) and all(c['p']==.003 and c['rounds']==c['d'] for c in plan['cases']),'Wrong settings')
    need(set(plan['build_nodes'].values()) <= {f'ares{i}' for i in range(1,7)},'Forbidden node')
    for policy,mode in [('degree',0),('skip',1),('coordinate',2)]:
        prefix=manifest['run']+'/build/'+policy+'/'
        ready=R.load(files[prefix+'ready.json'])
        built=R.load(files[prefix+'built.json'])
        need(ready['complete'] and built['complete'] and ready['policy']==built['policy']==policy,'Unfinished policy build')
        need(files[manifest['run']+'/exit-'+ready['job_id']+'.txt'].read_text().strip()=='0','Build exit differs')
        for label,stats in [('timing',False),('statistics',True)]:
            b=built['variants'][label]
            need(b['statistics'] is stats and b['policy_mode']==mode,'Wrong binary statistics/policy setting')
        need(len(ready['smoke'])==6 and {(x['d'],x['phase']) for x in ready['smoke']}==
             {(d,p) for d in [9,33] for p in ['batch','latency','statistics']},'Incomplete endpoint checks')
        for call in ready['smoke']:
            v=R.load(mapped(call['validation']))
            need(v['complete'] and v['shots']==1025 and v['weight_differences']==0,'Endpoint validation failed')
            for name,ref in v['files'].items():
                if name not in LARGE: mapped(ref)
    proof=R.load(files[manifest['run']+'/build/degree/disabled-instrumentation.json'])
    need(proof['complete'] and proof['statistics_symbols_absent'] and len(proof['comparisons'])==2 and
         all(c['token_stream_equal'] for c in proof['comparisons']),'Disabled instrumentation equivalence failed')
    all_calls={}; planned=set()
    for lane in plan['lanes']:
        for u in lane['units']:
            for m in METHODS[:-1] if u['phase']=='statistics' else METHODS:
                planned.add((u['case'],m,u['phase'],u['repeat']))
    for lane in summary['lanes']:
        done=R.load(mapped(lane['complete']))
        need(done['complete'] and mapped(lane['exit']).read_text().strip()=='0','Lane unfinished')
        mapped(done['plan'])
        for call in done['passes']:
            key=call['case'],call['method'],call['phase'],call['repeat']
            need(key not in all_calls and key in planned,'Duplicate/unplanned call')
            v=R.load(mapped(call['validation']))
            need(v['complete'] and v['shots']==1_000_000 and v['weight_differences']==0,'Invalid native validation')
            for name,ref in v['files'].items():
                if name not in LARGE: mapped(ref)
            warm=R.load(mapped(v['files']['warmup.json']))
            correctness=R.load(mapped(v['files']['correctness.json']))
            need(warm['requested_seconds']==2 and warm['actual_elapsed_ns']>=2_000_000_000,'Warmup too short')
            need(correctness['status']=='passed' and correctness['weight_differences']==0 and correctness['actual_candidate_witnesses']>=correctness['observable_differences'],'Native correctness failed')
            if call['phase']=='statistics':
                s=R.load(mapped(v['files']['syndromes.json']))
                need(not s['timed'] and s['components']==v['components'] and s['completed_shots']==v['completed_shots'],'Syndrome counts differ')
                need(v['normalized']==sum(c['normalized'] for c in v['components']) and v['residual']==sum(c['residual'] for c in v['components']),'Component sums differ')
                for c in v['components']:
                    need(c['normalized']==c['residual']+2*c['pairs']+c['boundaries'],'Seed conservation failed')
                if call['method']=='skip': need(v['normalized']==v['residual'],'Skip count differs')
            else:
                with mapped(v['files']['measurement.csv']).open(newline='') as stream: measures=list(csv.DictReader(stream))
                need(len(measures)==1 and int(measures[0]['elapsed_ns'])==v['elapsed_ns'] and int(measures[0]['shots'])==1_000_000,'Timing count differs')
            all_calls[key]=call
    need(set(all_calls)==planned and len(all_calls)==133,'Call coverage differs')
    need({(c['case'],c['method'],c['phase'],c['repeat']) for c in summary['calls']}==planned,'Summary coverage differs')
    for row in summary['rows']:
        case=next(c['id'] for c in plan['cases'] if c['d']==row['distance'])
        calls=[all_calls[(case,row['strategy'],'batch',r)] for r in range(1,4)]
        mean=sum(c['elapsed_ns'] for c in calls)/3e9
        need(row['mean_decoding_us_per_shot']==mean,'Rounded/stale timing summary')
        if row['strategy']!='Official':
            s=all_calls[(case,row['strategy'],'statistics',1)]
            need(row['remaining_syndromes_total']==s['residual'] and row['remaining_syndromes_per_shot']==s['residual']/1e6,'Rounded/stale residual summary')
    for ref in manifest['exports'].values():
        path=relative(root,ref['path']); R.ref(path,ref['sha256'])
    with relative(root,manifest['exports']['predecode.csv']['path']).open(newline='') as stream:
        metrics={r['metric']:r for r in csv.DictReader(stream)}
    need(len(metrics)==10,'Incomplete renderer data')
    for row in summary['rows']:
        if row['strategy']=='Official': continue
        legacy='current' if row['strategy']=='degree' else row['strategy']
        col=f'd{row["distance"]:02d}'
        need(float(metrics['speed_'+legacy][col])==row['mean_decoding_us_per_shot'],'Renderer timing differs')
        key='initial_mean' if legacy=='skip' else 'residual_'+legacy
        need(float(metrics[key][col])==row['remaining_syndromes_per_shot'],'Renderer residual differs')
    return summary


def bundle(run,output):
    run=Path(run).resolve(); output=Path(output).resolve()
    need(not output.exists(),'Bundle destination must be fresh')
    final=R.load(run/'complete.json'); need(final['complete'],'Run incomplete')
    R.ref(final['summary']['path'],final['summary']['sha256'])
    summary=R.load(final['summary']['path']); plan=R.load(run/'plan.json')
    analysis=R.load(run/'submission-analysis.json')
    need(len(analysis['jobs'])==1 and (run/('exit-'+analysis['jobs'][0]['job_id']+'.txt')).read_text().strip()=='0','Analysis has not exited successfully')
    selected=set([run/'plan.json',run/'freeze.json',run/'complete.json',Path(final['summary']['path'])])
    for lane in summary['lanes']:
        selected.update(Path(ref['path']) for ref in lane.values())
        selected.add(Path(lane['complete']['path']).parent/'machine.json')
    for call in summary['calls']:
        selected.add(Path(call['validation']['path']))
        v=R.load(call['validation']['path'])
        selected.update(Path(ref['path']) for name,ref in v['files'].items() if name not in LARGE)
    for name in ['submission-build.json','submission-formal.json','submission-analysis.json']:
        selected.add(run/name)
    for policy in ['degree','skip','coordinate']:
        folder=run/'build'/policy
        for name in ['ready.json','built.json','machine.json','timing-policy-test.log','statistics-policy-test.log']:
            selected.add(folder/name)
        ready=R.load(folder/'ready.json'); selected.add(run/('exit-'+ready['job_id']+'.txt'))
        for call in ready['smoke']:
            selected.add(Path(call['validation']['path']))
            v=R.load(call['validation']['path'])
            selected.update(Path(ref['path']) for name,ref in v['files'].items() if name not in LARGE)
    selected.update([run/'build/degree/disabled-instrumentation.json',run/'build/degree/core-tests.log',
        run/('exit-'+analysis['jobs'][0]['job_id']+'.txt')])
    output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.tailreset-',dir=output.parent) as tmp:
        temporary=Path(tmp); files={}
        for p in sorted(selected):
            need(run in p.parents,'Evidence escaped the completed run')
            target=temporary/'evidence'/p.relative_to(run)
            target.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(p,target)
            files[str(p)]=dict(path=str(target.relative_to(temporary)),sha256=R.sha(target))
        exports={}
        for name in ['summary.csv','latency.csv','predecode.csv']:
            p=run/'analysis-r01'/name; target=temporary/name; shutil.copy2(p,target)
            exports[name]=dict(path=name,sha256=R.sha(target))
        manifest=dict(schema=1,complete=True,run=str(run),plan=R.ref(run/'plan.json'),summary=final['summary'],
            implementation_commit=plan['implementation_commit'],acquisition_commit=plan['acquisition_commit'],
            files=files,exports=exports,
            offline_scope='All 133 completed validation/correctness/warmup/time/residual metadata and integer-derived summaries; million-row raw CSVs, input data and binaries remain at their original hash-linked server paths. Offline verification does not re-decode or re-read excluded raw CSVs.')
        R.write(temporary/'SOURCE.json',manifest)
        verify(temporary/'SOURCE.json')
        temporary.rename(output)
    print(output/'SOURCE.json')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('action',choices=['bundle','verify'])
    p.add_argument('--run',type=Path)
    p.add_argument('--output',type=Path)
    p.add_argument('--source',type=Path,default=HERE/'data/tailreset/SOURCE.json')
    a=p.parse_args()
    if a.action=='bundle':
        need(a.run and a.output,'bundle requires --run and --output'); bundle(a.run,a.output)
    else:
        s=verify(a.source)
        print('Verified 133 million-shot calls; timing and syndrome statistics are separate')
        for row in s['rows']:
            if row['distance']==33 and row['strategy']!='Official':
                print(row['strategy'],row['mean_decoding_us_per_shot'],row['remaining_syndromes_per_shot'])


if __name__=='__main__': main()
