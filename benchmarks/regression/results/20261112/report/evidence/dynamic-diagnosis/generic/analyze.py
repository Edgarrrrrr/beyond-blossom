#!/usr/bin/env python3
"""Replay completed diagnostic summaries, retaining paired process differences."""
from pathlib import Path
import csv,hashlib,json,statistics
BASE=Path(__file__).resolve().parent.parent
RUN=BASE/'full-r2-frozen-01'
OUT=Path(__file__).resolve().parent
OLD=Path('/tmp/beyond-build/generic-final-fast/summary.json')
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def stats(v):return {'median':statistics.median(v),'minimum':min(v),'maximum':max(v),'values':v}
def main():
    rows=json.loads((RUN/'process-medians.json').read_text());groups={}
    for r in rows:groups.setdefault(r['arm'],{})[r['matrix_repeat']]=r
    assert len(rows)==72 and len(groups)==12 and all(set(x)==set(range(6)) for x in groups.values())
    def pair(name,baseline,test,metric):
        a=[groups[baseline][i][metric] for i in range(6)];b=[groups[test][i][metric] for i in range(6)]
        d=[y-x for x,y in zip(a,b)]
        return {'comparison':name,'baseline':baseline,'test':test,'metric':metric,'baseline_process_medians':stats(a),
            'test_process_medians':stats(b),'paired_differences':stats(d),'positive_pairs':sum(v>0 for v in d),
            'ratio_of_medians_percent':100*(statistics.median(b)/statistics.median(a)-1) if min(a)>0 else None,
            'paired_percent':stats([100*(y/x-1) for x,y in zip(a,b)]) if min(a)>0 else None}
    comparisons=[]
    for binary,variant in [('candidate','off'),('candidate','on'),('source241','reference'),('official','reference')]:
        a=f'{binary}-normal-predecode_{variant}-pmu_'
        comparisons.append(pair('PMU boundary disturbance',a+'off',a+'on','latency_ns_per_shot'))
    for v in ['off','on']:
        for name,a,b in [('Copied-driver control','normal','split'),('Clocked-stage control','split','stages')]:
            for metric in ['latency_ns_per_shot','cycles_per_shot','instructions_per_shot']:
                comparisons.append(pair(name,f'candidate-{a}-predecode_{v}-pmu_on',f'candidate-{b}-predecode_{v}-pmu_on',metric))
    comparisons.append(pair('Normal predecode without PMU','candidate-normal-predecode_off-pmu_off','candidate-normal-predecode_on-pmu_off','latency_ns_per_shot'))
    for mode in ['normal','split','stages']:
        for metric in ['latency_ns_per_shot','cycles_per_shot','instructions_per_shot']:
            comparisons.append(pair('Predecode paired difference',f'candidate-{mode}-predecode_off-pmu_on',f'candidate-{mode}-predecode_on-pmu_on',metric))
    for phase in ['common','prepare','gate','roots','prefix','solve','extract','tail']:
        comparisons.append(pair('Clocked phase difference','candidate-stages-predecode_off-pmu_on','candidate-stages-predecode_on-pmu_on',phase+'_ns_per_shot'))
    for base in ['source241','official']:
        for pmu in ['off','on']:
            for v in ['off','on']:
                comparisons.append(pair('Reference adapter comparison',f'{base}-normal-predecode_reference-pmu_{pmu}',f'candidate-normal-predecode_{v}-pmu_{pmu}','latency_ns_per_shot'))
    old=next(r for r in json.loads(OLD.read_text()) if r['workload']=='holdout_hubs_sparse')
    original_pairs={}
    for base in ['baseline','off']:
        a=old['process_medians_ns_per_shot'][base];b=old['process_medians_ns_per_shot']['on']
        original_pairs[base]={'ratio_of_medians_percent':100*(statistics.median(b)/statistics.median(a)-1),
            'paired_percent':stats([100*(y/x-1) for x,y in zip(a,b)]),'positive_pairs':sum(y>x for x,y in zip(a,b))}
    plan=json.loads((RUN/'plan.json').read_text());audits=[json.loads((RUN/c['output_relative']/'audit.json').read_text()) for c in plan['calls']]
    aggregate={'processes':len(audits),'native_batches':sum(len(a['batches']) for a in audits),
        'measured_shot_records':sum(a['results'] for a in audits),'candidate_exact_weight_and_observable_records':sum(a['results'] for a in audits if a['receipt']['variant']!='reference'),
        'reference_weight_records':sum(a['results'] for a in audits if a['receipt']['variant']=='reference'),
        'reference_observable_differences':sum(a['observable_differences'] for a in audits if a['receipt']['variant']=='reference'),
        'pmu_processes':sum(a['receipt']['pmu_executed'] for a in audits),'pmu_snapshots':sum(2*len(a['batches']) for a in audits if a['receipt']['pmu_executed']),
        'clock_calibration_raw_values':10000*len(audits),'batch_repeats_are_not_independent_inputs':True,'distinct_saved_shots':200}
    result={'complete':True,'scope':'Exploratory diagnosis, not a replacement for sealed formal performance results','aggregate':aggregate,
        'original_formal_hubs':old,'original_paired_comparisons':original_pairs,'diagnostic_comparisons':comparisons,
        'files':{str(p):sha(p) for p in [RUN/'plan.json',RUN/'summary.json',RUN/'process-medians.json',RUN/'raw.csv',RUN/'complete.json',RUN/'output-manifest.json',
            RUN/'assets/collector.cc',RUN/'assets/build.json',RUN/'controller/verify_run.py',OLD,Path(__file__)]}}
    (OUT/'analysis.json').write_text(json.dumps(result,indent=2,sort_keys=True)+'\n')
    with (OUT/'paired-comparisons.csv').open('w',newline='') as f:
        fields=['comparison','baseline','test','metric','ratio_of_medians_percent','median_difference','minimum_difference','maximum_difference','positive_pairs']
        w=csv.DictWriter(f,fieldnames=fields);w.writeheader()
        for c in comparisons:w.writerow({**{k:c[k] for k in fields[:5]},**{k+'_difference':c['paired_differences'][k] for k in ['median','minimum','maximum']},'positive_pairs':c['positive_pairs']})
    print(json.dumps(aggregate))
    for c in comparisons:
        if c['comparison'] in ['Normal predecode without PMU','Clocked phase difference'] or (c['comparison']=='Predecode paired difference' and c['metric']!='latency_ns_per_shot'):
            print(c['comparison'],c['test'],c['metric'],json.dumps(c['paired_differences']),c['positive_pairs'],c['ratio_of_medians_percent'])
if __name__=='__main__':main()
