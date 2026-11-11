#!/usr/bin/env python3
"""Create the report's sole numeric source from sealed completed regression runs.

Checks small raw files and recomputes descriptive statistics. Large binary/input
hashes are retained from acquisition receipts; this analyzer does not re-read those
large files or rerun native correctness checks.
"""
from pathlib import Path
import argparse,csv,hashlib,json,math,shutil,statistics
ROOT=Path(__file__).resolve().parents[1]
IMPL='edd29b6780713dc78f6a99d67c4c4d192e9c52b5'
SOURCE='241160314087e5ae698c6e76c3c055a11c3ad422'
OFFICIAL='e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
STRUCTURED_COLLECTOR='0ad4fde55adaae41e337a6ee89974703c07bb8cd'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def read(path):return json.loads(Path(path).read_text())
def need(c,m):
 if not c:raise ValueError(m)
def close(a,b):return math.isclose(float(a),float(b),rel_tol=1e-11,abs_tol=1e-9)
def freeze(root,name,files):
 target=ROOT/'evidence'/name;target.mkdir(parents=True,exist_ok=True);refs=[]
 for rel in files:
  src=root/rel;dst=target/rel
  if not src.is_file():continue
  dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dst)
  refs.append({'path':str(dst.relative_to(ROOT)),'sha256':sha(dst),'acquisition_path':str(src.resolve())})
 return refs

def generic(root,name):
 plan=read(root/'manifest.json');reported=read(root/'summary.json')
 need(plan['status']=='complete','Incomplete generic run')
 need(plan['raw_sha256']==sha(root/'raw.csv'),'Generic raw hash changed')
 need(plan['baseline_revision']==SOURCE and IMPL in plan['candidate_identity'],'Unexpected generic source identities')
 rows=list(csv.DictReader((root/'raw.csv').open()));groups={};counts={};weights={}
 for r in rows:
  identity=(r['workload'],r['variant'],int(r['matrix_repeat']),int(r['native_repeat']))
  need(identity not in counts,'Duplicate generic row');counts[identity]=True
  n=int(r['shots']);elapsed=int(r['elapsed_ns']);need(n>0 and elapsed>0 and close(r['ns_per_shot'],elapsed/n),'Generic raw time/count mismatch')
  weights.setdefault(r['workload'],set()).add((r['weight_checksum'],r['weight_sequence_hash']))
  groups.setdefault(r['workload'],{}).setdefault(r['variant'],{}).setdefault(int(r['matrix_repeat']),[]).append(elapsed/n)
 expected={(c['name'],v,r,b) for c in plan['cases'] for v in ('baseline','off','on') for r in range(plan['repeats']) for b in range(plan['batches_per_process'])}
 need(set(counts)==expected,'Incomplete generic matrix');need(all(len(w)==1 for w in weights.values()),'Generic weight vectors differ')
 declared={r['workload']:r for r in reported};result=[]
 for case in plan['cases']:
  w=case['name'];v=groups[w];process={label:[statistics.median(v[label][k]) for k in sorted(v[label])] for label in ('baseline','off','on')}
  median={label:statistics.median(values) for label,values in process.items()}
  need(all(close(median[x],declared[w]['median_ns_per_shot'][x]) for x in median),'Generic summary disagrees with raw integers')
  shot_counts={int(r['shots']) for r in rows if r['workload']==w};need(len(shot_counts)==1,'Generic corpus count changed')
  source,candidate,off=median['baseline'],median['on'],median['off']
  result.append({'workload':w,'run':name,'source_ns':source,'candidate_ns':candidate,'predecode_off_ns':off,
    'source_over_candidate':source/candidate,'off_over_candidate':off/candidate,'candidate_change_percent':100*(candidate/source-1),
    'unique_input_records':shot_counts.pop(),'process_repeats':plan['repeats'],'batches_per_process':plan['batches_per_process'],
    'process_medians_ns':process,'process_range_ns':{k:[min(x),max(x)] for k,x in process.items()},
    'complete_shots':sorted({int(r['complete_shots']) for r in rows if r['workload']==w and r['variant']=='on'}),
    'skipped_shots':sorted({int(r['skipped_shots']) for r in rows if r['workload']==w and r['variant']=='on'}),
    'graph_sha256':case['graph_sha256'],'input_sha256':case['shots_sha256'],'expected_weights_sha256':case.get('expected_weights_sha256')})
 refs=freeze(root,name,['manifest.json','raw.csv','summary.json','artifacts/baseline-build.json','artifacts/candidate-build.json','artifacts/run_generic.py','validation-audit.json','artifacts/source-identity.json','artifacts/input-files.json','artifacts/final-hardware.json','artifacts/generic-final-protocol.json'])
 return {'name':name,'acquisition_scope':plan['scope'],'host':plan['host'],'timer':plan['timer'],'rows':result,'raw_rows':len(rows),'references':refs,
         'aggregation':'median of per-process medians of fixed-corpus batch ns/shot; range across processes, not a confidence interval',
         'primary_official_comparison':'not collected for this arbitrary-graph migration matrix'}

def structured(root):
 plan=read(root/'plan.json');done=read(root/'complete.json');reported=read(root/'summary.json')
 need(done['complete'] and done['plan_sha256']==sha(root/'plan.json') and done['raw_sha256']==sha(root/'raw.csv'),'Structured run incomplete or seal changed')
 need(plan['schema']==2 and reported['official_revision']==OFFICIAL and reported['source_revision']==SOURCE,'Structured baseline roles differ')
 need(plan['controller_identity']=={'commit':STRUCTURED_COLLECTOR,'status':''},'Unexpected structured collector identity')
 for role in ('source_identity','candidate_identity'):
  identity=plan[role]['record']
  need(identity['candidate_commit']==IMPL and identity['source_upstream_revision']==SOURCE,'Structured runtime/source identity differs')
  need(not identity['git_status_porcelain'] and not identity['candidate_src_diff_from_commit'] and not identity['candidate_src_diff_from_compiled_source09'] and not identity['source_src_diff_from_public_tag'],'Structured source observation has differences')
  need(all(not r['mismatched_receipt_inputs'] for r in identity['receipt_checks']),'Structured source/build receipt mismatch')
 for rel,digest in plan['build_artifacts'].items():
  need(sha(root/rel)==digest,'Structured build provenance changed: '+rel)
 need(plan['reference']['complete'] and plan['reference']['upstream_revision']==OFFICIAL and plan['reference']['binary']['sha256']==plan['binaries']['reference']['sha256'],'Structured official receipt does not match timed reference')
 rows=list(csv.DictReader((root/'raw.csv').open()));groups={};seen=set()
 input_hashes={item['case']:item['files'][f'inputs/{item["case"]}/dataset.bin'] for item in done['inputs']}
 for r in rows:
  identity=(r['case'],r['method'],int(r['repeat']),r['variant']);need(identity not in seen,'Duplicate structured row');seen.add(identity)
  n=int(r['shots']);elapsed=int(r['elapsed_ns']);need(n==plan['shots'] and elapsed>0 and close(r['ns_per_shot'],elapsed/n),'Invalid structured batch')
  method=plan['official_method'] if r['method']=='reference' else plan['methods'][r['method']]
  need(int(r['threads'])==method['threads'] and r['backend']==method.get('backend',plan['backend']),'Structured dispatch differs from plan')
  need(r['dataset_sha256']==input_hashes[r['case']],'Structured rows do not share sealed case inputs')
  need(int(r['observable_differences'])>=0 and int(r['actual_candidate_witnesses'])>=int(r['observable_differences']),'Observable tie has no candidate witness')
  groups.setdefault((r['case'],r['method']),{}).setdefault(r['variant'],{})[int(r['repeat'])]=elapsed/n
 expected={(c['case'],m,k,v) for c in plan['cases'] for m in plan['methods'] for k in range(1,plan['repeats']+1) for v in ('source','candidate')}
 expected|={(c['case'],'reference',k,'reference') for c in plan['cases'] for k in range(1,plan['repeats']+1)}
 need(seen==expected,'Incomplete structured matrix')
 results=[];declared={(x['case'],x['method']):x for x in reported['rows']}
 for case in plan['cases']:
  official=groups[case['case'],'reference']['reference'];om=statistics.median(official.values())
  for method in plan['methods']:
   group=groups[case['case'],method];source=group['source'];candidate=group['candidate'];sm=statistics.median(source.values());cm=statistics.median(candidate.values())
   check=declared[case['case'],method];need(all(close(a,b) for a,b in [(sm,check['source_median_ns']),(cm,check['candidate_median_ns']),(om,check['official_median_ns'])]),'Structured summary differs from raw')
   results.append({'case':case['case'],'distance':case['distance'],'error_rate':case['error_rate'],'method':method,'threads':plan['methods'][method]['threads'],
    'official_ns':om,'source_ns':sm,'candidate_ns':cm,'official_over_candidate':om/cm,'source_over_candidate':sm/cm,
    'candidate_change_percent':100*(cm/sm-1),'unique_input_records':plan['shots'],'repeats':plan['repeats'],
    'repeat_ns':{'official':list(official.values()),'source':list(source.values()),'candidate':list(candidate.values())},
    'paired_source_speedups':[source[k]/candidate[k] for k in sorted(source)],'paired_official_speedups':[official[k]/candidate[k] for k in sorted(source)]})
 overview=[]
 for method in plan['methods']:
  subset=[r for r in results if r['method']==method];official=[r['official_over_candidate'] for r in subset];migration=[r['source_over_candidate'] for r in subset]
  overview.append({'method':method,'cases':len(subset),'official_geomean_speedup':math.exp(statistics.mean(map(math.log,official))),
    'source_geomean_speedup':math.exp(statistics.mean(map(math.log,migration))),
    'source_speedup_range':[min(migration),max(migration)],'slower_than_source':sum(x<1 for x in migration),'slower_than_official':sum(x<1 for x in official)})
 files=['plan.json','complete.json','raw.csv','summary.json','summary.csv']+list(plan['build_artifacts'])
 refs=freeze(root,'structured',files)
 return {'host':plan['host'],'platform':plan['platform'],'backend':plan['backend'],'cpus':plan['cpus'],'input_sets':len(plan['cases']),
  'unique_inputs_per_setting':plan['shots'],'repeats':plan['repeats'],'raw_rows':len(rows),'rows':results,'overview':overview,
  'aggregation':'median of three complete-batch ns/shot values; ratio of medians; paired repeat ratios retained separately',
  'timing':plan['timing'],'scheduling':plan['scheduling'],'correctness':plan['correctness'],'scope':plan['scope'],'references':refs}

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--generic-fast',type=Path,required=True);p.add_argument('--generic-dense',type=Path,required=True);p.add_argument('--structured',type=Path);args=p.parse_args()
 runs=[generic(args.generic_fast,'generic-fast'),generic(args.generic_dense,'generic-dense')];st=structured(args.structured) if args.structured else None;gr=[r for run in runs for r in run['rows']]
 need(len({r['workload'] for r in gr})==len(gr),'Duplicate generic workloads')
 result={'schema':1,'status':'complete' if st else 'structured_collecting','implementation_commit':IMPL,'collector_commits':['edd29b6780713dc78f6a99d67c4c4d192e9c52b5','0ad4fde55adaae41e337a6ee89974703c07bb8cd'],
  'official_revision':OFFICIAL,'migration_source_revision':SOURCE,'structured':st,
  'generic':{'runs':runs,'rows':gr,'overview':{'workloads':len(gr),'source_geomean_speedup':math.exp(statistics.mean(math.log(r['source_over_candidate']) for r in gr)),
    'source_speedup_range':[min(r['source_over_candidate'] for r in gr),max(r['source_over_candidate'] for r in gr)],
    'slower_than_source':[{'workload':r['workload'],'candidate_change_percent':r['candidate_change_percent'],'source_ns':r['source_ns'],'candidate_ns':r['candidate_ns'],'process_range_ns':r['process_range_ns']} for r in gr if r['source_over_candidate']<1]},
    'official_comparison':'No same-workload fixed-official measurements; source241 comparison describes migration and predecoder increment only'},
  'analyzer_sha256':sha(__file__),'verification':'Raw CSV SHA, exact matrix keys, positive elapsed/count, generic weight-vector equality and recomputed summaries checked. Large input/binary hashes retained from collector receipts; native correctness not rerun.',
  'scope':'Frozen regression validation, distinct from archived million-shot paper acquisition; no per-shot latency quantiles inferred from batch times'}
 (ROOT/'summary.json').write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n');print(ROOT/'summary.json')
if __name__=='__main__':main()
