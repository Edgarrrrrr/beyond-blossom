#!/usr/bin/env python3
"""Independent read-only audit of completed main matrices; never reads giant inputs."""
from pathlib import Path
import csv, datetime, hashlib, io, json, math, collections
ROOT=Path('/home/fujy/dev/beyond_blossom/ae-runs/20261112-local-paper')
RUN=ROOT/'slurm-paper'
OUT=ROOT/'main-matrix-audit.json'
METHODS=['reference','structured','predecoded','component-parallel','pipelined','parallel-predecode']
EXTERNAL=['parity','fusion','parity-uf','unionfind','ldpc-uf']
CPUS=[2,4,6,8,10,12,14,16]
OFFICIAL='e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd'
issues=[]; hashes={}; native=[]; external=[]; cases=[]; correctness=[]; contexts=[]
counts=collections.Counter()
def need(ok, message, **details):
    if not ok: issues.append({'message':message,'context':contexts[-1] if contexts else None,**details})
def sha(data): return hashlib.sha256(data).hexdigest()
def read(path, expected=None):
    path=Path(path); data=path.read_bytes(); h=sha(data); hashes[str(path)]=h
    if expected is not None: need(h==expected,'file SHA mismatch',path=str(path),expected=expected,actual=h)
    return data

def js(path, expected=None): return json.loads(read(path,expected))
def table(data): return list(csv.DictReader(io.StringIO(data.decode())))
def near(a,b): return math.isclose(float(a),float(b),rel_tol=2e-12,abs_tol=1e-8)
def metrics(values):
    return {'count':len(values),'minimum':min(values),'maximum':max(values),'geometric_mean':math.exp(sum(map(math.log,values))/len(values))}
def source_ok(source,commit): return source['commit']==commit and not source.get('status') and not source.get('dirty',False)
def argument(cmd,key): return cmd[cmd.index(key)+1] if key in cmd else None
plan=js(RUN/'plan.json'); plan_sha=hashes[str(RUN/'plan.json')]
shared=js(plan['builds_manifest']); shared_sha=hashes[plan['builds_manifest']]
commit=plan['source_identity']['commit']
need(shared['complete'] and source_ok(shared['source_identity'],commit),'shared source/complete')
receipts={p:js(p,h) for p,h in shared['receipts'].items()}
candidate_receipt=receipts[str(Path(shared['native']['policy-0-l3'])/'build.json')]
reference_receipt=receipts[shared['reference_records']['3']]
external_receipt=receipts[str(Path(shared['external'])/'build.json')]
expected={(d,p) for d in [9,13,17,21,25,29,33] for p in [.0005,.001,.003]}
assigned=[(c['distance'],c['error_rate']) for s in plan['shards'] for c in s['cases']]
need(len(assigned)==21 and set(assigned)==expected,'exact 21-case assignment')
need(plan['native_shots_per_case']==1000000 and plan['external_shots']==10000 and plan['repeats']==3,'top-level sample protocol')
need(plan['cpus']==','.join(map(str,CPUS)),'top-level CPU list')
all_native_keys=set(); all_external_keys=set()
for shard in plan['shards']:
    worker=js(RUN/'workers'/str(shard['index'])/'placement.json')
    need(worker['node']==shard['node'] and worker['host']==shard['node'],'worker host assignment')
    need(worker['cpus']==CPUS and worker['plan_sha256']==plan_sha and worker['builds_manifest_sha256']==shared_sha,'worker frozen plan/build/cpu identity')
    for case in shard['cases']:
        name=case['name']; d=case['distance']; p=case['error_rate']; contexts.append(name)
        base=RUN/'cases'/name; conf=js(base/'config.json'); n=base/'runs/native-batch'; e=base/'runs/external'
        np=js(n/'plan.json'); nc=js(n/'complete.json')
        need(nc['complete'] and nc['plan_sha256']==hashes[str(n/'plan.json')],'native complete/plan binding')
        need(conf['placement']['host']==np['host']==worker['host'],'case/native/worker host')
        need(conf['placement']['slurm']['SLURM_JOB_ID']==worker['job_id'],'case/worker job identity')
        need(conf['cpus']==np['cpus']==CPUS,'case/native CPU list')
        need(conf['shots']==np['shots']==1000000 and conf['repeats']==np['repeats']==3 and conf['external_shots']==10000,'case/native sample protocol')
        need(np['distances']==[d] and np['error_rates']==[p] and conf['distances']==[d] and conf['error_rates']==[p],'case exact selection')
        need(source_ok(conf['source_identity'],commit) and source_ok(np['source'],commit),'case/native source')
        need(np['binary']['build_record']==candidate_receipt and np['reference']['build_record']==reference_receipt,'native embedded build records match shared sealed build')
        need(np['reference']['independent'] and reference_receipt['upstream_revision']==OFFICIAL,'official independent reference')
        need(set(np['methods'])==set(METHODS) and np['measurement']=='batch' and np['backend']==plan['backend'],'native exact methods/backend/scope')
        need(len(nc['inputs'])==1,'single native input per case')
        input_files=nc['inputs'][0]['files']; dataset=[(k,v) for k,v in input_files.items() if k.endswith('/dataset.bin')]; dem=[(k,v) for k,v in input_files.items() if k.endswith('/model.dem')]
        need(len(dataset)==len(dem)==1,'single dataset and DEM digest')
        dataset_path=str(n/dataset[0][0]); dem_path=str(n/dem[0][0]); groups=collections.defaultdict(list); syndrome_hashes=set(); ref_hashes=set(); seen=set(); obs_total=0
        need(len(nc['calls'])==18,'18 native calls per case')
        for call in nc['calls']:
            method=call['method']; repeat=call['repeat']; contexts.append(f'{name}/{method}/{repeat}')
            key=(method,repeat); need(key not in seen,'duplicate native method/repeat'); seen.add(key)
            need(method in METHODS and repeat in [1,2,3] and call['distance']==d and call['error_rate']==p,'native call identity')
            directory=n/call['directory']; contents={}
            for rel,h in call['files'].items():
                if Path(rel).name in {'native_shots.csv','input_features.csv'}:
                    counts['large_native_auxiliary_files_not_read']+=1; continue
                contents[Path(rel).name]=read(n/rel,h)
            measurement=table(contents['measurement.csv']); need(len(measurement)==1,'one measurement row'); row=measurement[0]
            co=json.loads(contents['correctness.json']); scope=json.loads(contents['measurement_scope.json']); placement=json.loads(contents['placement.json']); warmup=json.loads(contents['warmup.json'])
            elapsed=int(row['elapsed_ns']); shots=int(row['shots']); threads=np['methods'][method]['threads']; execution=np['methods'][method]['execution']
            need(shots==1000000 and elapsed>0 and near(row['ns_per_shot'],elapsed/shots),'native integer elapsed/mean')
            need(int(row['total_threads'])==threads and row['execution']==execution,'native execution configuration')
            need(co['status']=='passed' and co['shots']==shots and co['weight_differences']==int(row['weight_diff'])==0,'native exact integer optimum')
            need(co['observable_differences']==int(row['observable_diff']),'observable count agrees')
            need(co['actual_candidate_witnesses']>=co['observable_differences'],'observable differences independently witnessed')
            need(not scope['internal_instrumentation'] and scope['clock_pairs_per_shot']==0,'native uninstrumented batch timing')
            need(placement['physical_affinity_verified'] and warmup['requested_seconds']==2 and warmup['actual_elapsed_ns']>=2000000000,'native placement/warmup')
            blocks=table(contents['native_blocks.csv']); start=0; total=0
            for block in blocks:
                need(int(block['first_shot_index'])==start,'native contiguous input blocks'); start+=int(block['count']); total+=int(block['elapsed_ns'])
            need(start==shots and total==elapsed,'native integer block total equals elapsed')
            counts['native_blocks']+=len(blocks)
            execution_rows=table(contents['execution_threads.csv'])
            for boundary in ['before','after']:
                er=[x for x in execution_rows if x['boundary']==boundary]
                need(len(er)==threads and {int(x['cpu']) for x in er}==set(CPUS[:threads]),'native actual CPU placement',boundary=boundary)
                need(len({(x['socket'],x['physical_core']) for x in er})==threads and len({x['socket'] for x in er})==1,'native distinct physical cores on one socket',boundary=boundary)
                need(all(x['affinity_mask']==x['cpu'] for x in er),'native singleton thread affinity',boundary=boundary)
            command=js(directory.parent/'command.json')
            need(argument(command,'--dataset')==dataset_path and int(argument(command,'--shots'))==shots and argument(command,'--cpus')==','.join(map(str,CPUS[:threads])),'native invocation input/shot/cpu binding')
            need(command[0]==(np['reference']['path'] if method=='reference' else np['binary']['path']),'native invocation binary')
            groups[method].append({'repeat':repeat,'elapsed_ns':elapsed,'shots':shots,'mean_us_per_shot':elapsed/shots/1000,'observable_differences':co['observable_differences'],'actual_candidate_witnesses':co['actual_candidate_witnesses']})
            syndrome_hashes.add(row['syndrome_hash']); ref_hashes.add(row['reference_result_hash']); obs_total+=co['observable_differences']
            correctness.append({'case':name,'method':method,'repeat':repeat,**co})
            counts['native_calls']+=1; contexts.pop()
        need(seen=={(m,r) for m in METHODS for r in [1,2,3]},'complete native method-repeat set')
        need(len(syndrome_hashes)==len(ref_hashes)==1,'all native methods/repeats same ordered syndrome/reference hash')
        case_means={}
        for method in METHODS:
            rows=sorted(groups[method],key=lambda x:x['repeat']); elapsed=sum(x['elapsed_ns'] for x in rows); mean=elapsed/3000000/1000; case_means[method]=mean
            nr={'case':name,'distance':d,'error_rate':p,'host':worker['host'],'method':method,'threads':np['methods'][method]['threads'],'shots_per_repeat':1000000,'repeats':3,'total_elapsed_ns':elapsed,'mean_us_per_shot':mean,'repeat_mean_us_range':[min(x['mean_us_per_shot'] for x in rows),max(x['mean_us_per_shot'] for x in rows)],'repeated_execution_range_percent_of_mean':(max(x['mean_us_per_shot'] for x in rows)-min(x['mean_us_per_shot'] for x in rows))/mean*100,'runs':rows}
            native.append(nr); all_native_keys.add((d,p,method))
        best=min(METHODS[1:],key=case_means.get)
        case_entry={'case':name,'distance':d,'error_rate':p,'host':worker['host'],'job_id':worker['job_id'],'cpus':CPUS,'dataset_sha256_recorded':dataset[0][1],'dem_sha256_recorded':dem[0][1],'native_means_us':case_means,'best_candidate_method':best,'best_candidate_mean_us':case_means[best],'official_over_best_candidate':case_means['reference']/case_means[best],'native_observable_differences_repeated_executions':obs_total}
        ep=js(e/'plan.json'); ec=js(e/'complete.json'); es=js(e/'summary.json',ec['summary_sha256']); read(e/'summary.csv',ec['summary_csv_sha256'])
        need(ec['complete'] and ec['plan_sha256']==hashes[str(e/'plan.json')],'external complete/plan binding')
        need(ep['ae_run']==str(n) and ep['ae_plan_sha256']==hashes[str(n/'plan.json')] and ep['ae_complete_sha256']==hashes[str(n/'complete.json')],'external/native plan and complete binding')
        need(ep['external_build']==shared['external'] and ep['external_build_sha256']==shared['receipts'][str(Path(shared['external'])/'build.json')],'external sealed shared build')
        need(source_ok(ep['source'],commit) and ep['repeats']==3 and ep['requested_limit']==10000,'external source/sample protocol')
        need(len(ep['schedule'])==len(ec['groups'])==1,'one external group per case')
        scheduled=ep['schedule'][0]; need(scheduled['distance']==d and scheduled['error_rate']==p and scheduled['shots']==10000 and scheduled['source_shots']==1000000 and set(scheduled['methods'])==set(EXTERNAL),'external exact schedule')
        g=ec['groups'][0]; gp=e/g['summary']; gs=js(gp,g['sha256']); gc=gs['configuration']; prov=gc['input_provenance']
        need(gs['complete'] and gc['host']==np['host']==worker['host'] and gc['machine']==np['machine']=='x86_64','external/native same actual host and ISA')
        need(gc['build']==external_receipt and source_ok(gc['source'],commit),'external embedded build/source matches shared seal')
        need(prov['complete'] and prov['selected_shots']==prov['independent_measurement_inputs']==10000 and prov['repeats']==3,'external input independent count')
        need(prov['source']['saved_shots']==1000000 and prov['source']['path']==dataset_path and prov['source']['sha256']==dataset[0][1] and prov['dem']['path']==dem_path and prov['dem']['sha256']==dem[0][1],'external source input/DEM identity binding')
        need(gs['oracle']['records']==10000 and gs['oracle']['revision']==OFFICIAL,'external official oracle record')
        need(not prov['physical_observable_truth_available'],'external physical truth not available')
        need(len(gs['results'])==5 and {x['method'] for x in gs['results']}==set(EXTERNAL),'external exact five methods')
        # Load only compact scalar timing rows, not graph, correction chains or giant detector data.
        per_method={}; parity_weights=None; parity_observables=None
        for method in EXTERNAL:
            result=next(x for x in gs['results'] if x['method']==method); contexts.append(f'{name}/external/{method}')
            need(result['shots']==10000 and result['repeats']==3 and result['independent_measurement_inputs']==10000,'external measured sample counts')
            need(result['cpus']==(CPUS[:4] if method=='fusion' else CPUS[:1]),'external CPU assignment')
            need(result['native_status']['completed'] and result['native_status']['warmup_wall_seconds']>=2 and result['native_parity_checks']==30000 and result['independent_chain_checks']==10000,'external completed/warmup/parity/independent-chain receipt')
            need(result['logical_failures'] is None and not result['physical_observable_truth_available'],'external no unsupported logical-error claim')
            raw=read(gp.parent/(method+'.csv'),result['files'][method+'.csv']); counts['external_scalar_csv_bytes']+=len(raw)
            elapsed=[0,0,0]; nrows=[0,0,0]; weights=[[None]*10000 for _ in range(3)]; observables=[[None]*10000 for _ in range(3)]; gap_sums=[0,0,0]; nonoptimal=[0,0,0]; obs_diff=[0,0,0]
            for row in csv.DictReader(io.StringIO(raw.decode())):
                repeat=int(row['repeat']); shot=int(row['shot']); ns=int(row['decode_ns']); iw=int(row['integer_weight']); obs=int(row['observable'])
                need(repeat in [0,1,2] and 0<=shot<10000 and ns>0,'external scalar row domain')
                need(shot==nrows[repeat],'external repeat shot ordering / no omissions'); nrows[repeat]+=1; elapsed[repeat]+=ns; weights[repeat][shot]=iw; observables[repeat][shot]=obs
                if parity_weights is not None:
                    gap=iw-parity_weights[shot]; gap_sums[repeat]+=gap; nonoptimal[repeat]+=int(gap!=0); obs_diff[repeat]+=int(obs!=parity_observables[shot]); need(gap>=0,'external candidate below exact parity oracle proxy')
            need(nrows==[10000]*3,'external complete 3x10000 raw rows')
            if method=='parity':
                parity_weights=weights[0]; parity_observables=observables[0]
                need(all(w==parity_weights for w in weights),'exact parity integer weights repeat consistently')
            if method in ['parity','fusion']:
                need(result['integer_optimality']=='required' and all(x==0 for x in gap_sums) and all(x==0 for x in nonoptimal),'exact external methods reach same optimum')
            for r in range(3):
                need(near(elapsed[r]/1e9,result['repeat_decode_seconds'][r]) and near(elapsed[r]/10000/1000,result['repeat_mean_us'][r]),'external repeated integer timing matches summary')
                need(near(gap_sums[r]/10000,result['weight_gaps'][r]['mean_integer_gap']) and nonoptimal[r]==result['weight_gaps'][r]['nonoptimal_shots'],'external per-shot integer gaps match disclosed summary',repeat=r)
            mean=sum(elapsed)/30000/1000; need(near(mean,result['mean_us_per_shot']),'external pooled integer mean matches summary')
            outer=next(x for x in es['results'] if x['method']==method); need(near(mean,outer['mean_us_per_shot']) and outer['distance']==d and outer['error_rate']==p and outer['source_shots']==1000000,'external top-level summary consistency')
            rec={'case':name,'distance':d,'error_rate':p,'host':worker['host'],'method':method,'cpus':result['cpus'],'shots_per_repeat':10000,'repeats':3,'elapsed_ns_per_repeat':elapsed,'total_elapsed_ns':sum(elapsed),'mean_us_per_shot':mean,'repeat_mean_us_range':[min(elapsed)/10000/1000,max(elapsed)/10000/1000],'repeated_execution_range_percent_of_mean':(max(elapsed)-min(elapsed))/(sum(elapsed)/3)*100,'over_best_candidate':mean/case_means[best],'integer_optimality':result['integer_optimality'],'nonoptimal_shots_per_repeat':nonoptimal,'mean_integer_gap_per_repeat':[x/10000 for x in gap_sums],'observable_differences_from_parity_per_repeat':obs_diff,'timing_scope':result['timing_scope']}
            external.append(rec); per_method[method]=mean; all_external_keys.add((d,p,method)); counts['external_scalar_rows']+=sum(nrows); contexts.pop()
        case_entry['external_means_us']=per_method; cases.append(case_entry); contexts.pop()
        print(json.dumps({'audited_case':name,'issues_so_far':len(issues)}),flush=True)
need(all_native_keys=={(d,p,m) for d,p in expected for m in METHODS},'native all 126 case/method means')
need(all_external_keys=={(d,p,m) for d,p in expected for m in EXTERNAL},'external all 105 case/method means')
# Historical numbers are rounded prior observations, compared only through semantic roles.
history_path=Path('/home/fujy/dev/beyond_blossom/repo/AE/reference/plot-data.csv'); historical=table(read(history_path)); historical_roles={'Upstream':'reference','GridSIMD':'structured','Predecode':'predecoded','Component':'component-parallel','TailReset':'pipelined','Spatial':'parallel-predecode'}
history_map={(int(x['d']),float(x['p']),historical_roles[x['source_method']]):float(x['batch_us_per_shot']) for x in historical}
history_rows=[]
for x in native:
    old=history_map[(x['distance'],x['error_rate'],x['method'])]
    history_rows.append({'case':x['case'],'method':x['method'],'historical_rounded_us':old,'fresh_us':x['mean_us_per_shot'],'fresh_over_historical':x['mean_us_per_shot']/old})
by_method={m:metrics([x['native_means_us']['reference']/x['native_means_us'][m] for x in cases]) for m in METHODS[1:]}
spread_native=sorted(native,key=lambda x:x['repeated_execution_range_percent_of_mean'],reverse=True)
spread_external=sorted(external,key=lambda x:x['repeated_execution_range_percent_of_mean'],reverse=True)
obs_calls=[x for x in correctness if x['observable_differences']]
best_ratios=[x['official_over_best_candidate'] for x in cases]
report={
 'schema':1,'observed_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'audit_passed':not issues,
 'scope':'Completed native and external main matrices only. This audit does not certify completion of policy, statistics, PMU or whole-paper export.',
 'run':str(RUN),'script':str(Path(__file__).resolve()),'script_sha256':sha(Path(__file__).read_bytes()),
 'source_commit':commit,'official_revision':OFFICIAL,'plan_sha256':plan_sha,'shared_builds_manifest_sha256':shared_sha,
 'native_binary':{'path':np['binary']['path'],'sha256_recorded':np['binary']['sha256']},
 'reference_binary':{'path':np['reference']['path'],'sha256_recorded':np['reference']['sha256']},
 'hardware':{x['node']:js(RUN/'workers'/str(x['index'])/'placement.json')['hardware'] for x in plan['shards']},
 'protocol':{'cases':21,'native_methods':6,'native_calls':378,'native_distinct_inputs_per_case':1000000,'native_repeats_same_inputs':3,'external_methods':5,'external_method_means':105,'external_distinct_inputs_per_case':10000,'external_repeats_same_prefix':3,'native_cpu_list':CPUS,'external_fusion_cpus':CPUS[:4],'other_external_cpus':CPUS[:1],'mean_formula':'sum(integer elapsed_ns) / sum(shots) / 1000 = microseconds per shot','best_of_five':'Minimum pooled three-repeat candidate mean per case; ratio is official pooled mean divided by selected candidate pooled mean. This is descriptive selection, not an independent validation set.','geometric_mean':'exp(mean(log(ratio))), equal weighting of the 21 cases','external_oracle':'Separate official e27 optimum after external graph integer quantization; weights are not compared across native/external integer scales.'},
 'counts':dict(counts),'issues':issues,
 'best_of_five_speedup':{**metrics(best_ratios),'minimum_case':min(cases,key=lambda x:x['official_over_best_candidate'])['case'],'maximum_case':max(cases,key=lambda x:x['official_over_best_candidate'])['case'],'selected_method_counts':dict(collections.Counter(x['best_candidate_method'] for x in cases))},
 'official_over_each_candidate':by_method,
 'external_over_best_candidate':{m:metrics([x['over_best_candidate'] for x in external if x['method']==m]) for m in EXTERNAL},
 'correctness':{'native_weight_differences_repeated_executions':sum(x['weight_differences'] for x in correctness),'native_observable_differences_repeated_executions':sum(x['observable_differences'] for x in correctness),'native_calls_with_observable_differences':obs_calls,'native_witness_policy':'Equal-optimum observable differences are accepted only with actual candidate parity/weight/observable witness, as recorded in hashed correctness receipts.','external_exact_methods':['parity','fusion'],'external_exact_nonoptimal_repeated_executions':sum(sum(x['nonoptimal_shots_per_repeat']) for x in external if x['method'] in ['parity','fusion']),'external_approximate_methods':{m:{'cases_with_nonoptimal_shots':sum(any(x['nonoptimal_shots_per_repeat']) for x in external if x['method']==m),'nonoptimal_repeated_executions':sum(sum(x['nonoptimal_shots_per_repeat']) for x in external if x['method']==m),'total_repeated_executions':630000,'mean_integer_gap_range':[min(min(x['mean_integer_gap_per_repeat']) for x in external if x['method']==m),max(max(x['mean_integer_gap_per_repeat']) for x in external if x['method']==m)]} for m in EXTERNAL[2:]},'physical_observable_truth_available':False,'logical_error_rate_comparison_supported':False},
 'repeat_variation':{'interpretation':'Three repeated executions of the same fixed inputs; ranges are descriptive, not confidence intervals or independent input replications.','native_top10':[{'case':x['case'],'method':x['method'],'mean_us_per_shot':x['mean_us_per_shot'],'range_us':x['repeat_mean_us_range'],'range_percent':x['repeated_execution_range_percent_of_mean']} for x in spread_native[:10]],'external_top10':[{'case':x['case'],'method':x['method'],'mean_us_per_shot':x['mean_us_per_shot'],'range_us':x['repeat_mean_us_range'],'range_percent':x['repeated_execution_range_percent_of_mean']} for x in spread_external[:10]]},
 'cases':sorted(cases,key=lambda x:(x['error_rate'],x['distance'])),'native_means':sorted(native,key=lambda x:(x['error_rate'],x['distance'],METHODS.index(x['method']))),'external_means':sorted(external,key=lambda x:(x['error_rate'],x['distance'],EXTERNAL.index(x['method']))),
 'historical_role_comparison':{'path':str(history_path),'sha256':hashes[str(history_path)],'role_mapping':historical_roles,'interpretation':'Historical rounded plot observations and this fresh fixed-source multi-host campaign are different acquisition windows/builds/inputs. Ratios describe observed latency by role and do not establish a code regression, statistical equivalence or a byte-identical replication.','fresh_latency_over_historical_by_role':{m:metrics([x['fresh_over_historical'] for x in history_rows if x['method']==m]) for m in METHODS},'rows':history_rows},
 'verification_limits':['Input datasets, DEMs and binaries are bound through existing sealed receipts; their large contents were not reread or rehashed in this audit.','All 378 compact native measurement, integer block, correctness, placement and lifecycle artifacts were rehashed and checked against completion receipts; native_shots.csv and input_features.csv were deliberately excluded from the final audit.','All 105 external compact scalar timing CSVs (3.15 million rows) were rehashed, integer sums recomputed, and per-shot integer weight gaps compared with the exact parity result, which existing official-oracle receipts certify. Large oracle edge lists and correction chains were not reread; independent parity/chain validation is verified from the sealed completion summaries.','External uses the first 10000 of each million-shot saved input stream. Timing ratios between native and external therefore use different input counts and their stated native timing scopes; external quantization also differs.','No measurements, compilation, job submission, runtime/source changes or full-workflow completion assertion were performed.'],
 'small_and_scalar_file_sha256':hashes
}
OUT.write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
print(json.dumps({'output':str(OUT),'audit_passed':report['audit_passed'],'issues':issues[:15],'issue_count':len(issues),'counts':report['counts'],'best':report['best_of_five_speedup'],'correctness':{k:v for k,v in report['correctness'].items() if k!='native_calls_with_observable_differences'},'native_obs_call_count':len(obs_calls),'external_over_best':report['external_over_best_candidate'],'repeat_variation':report['repeat_variation'],'historical_by_role':report['historical_role_comparison']['fresh_latency_over_historical_by_role']},indent=2))
