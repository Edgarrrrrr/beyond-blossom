#!/usr/bin/env python3
"""Verify/copy the completed 120-call follow-up and render its rejected variant.

No native calls or compilation. With no --run, replay the copied small evidence.
"""
from pathlib import Path
import argparse, copy, csv, hashlib, html, json, math, re, shutil, statistics

R=Path(__file__).resolve().parents[1]
E=R/'evidence/boundary-outline'
ARMS=('source-sealed','current-sealed','rebuilt-current','outlined-boundary','official-sealed')
TARGET='src/beyond_blossom/structured/matcher/mwpm.inl'
def read(p):return json.loads(Path(p).read_text())
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def write(p,x):Path(p).write_text(json.dumps(x,indent=2,ensure_ascii=False)+'\n')
def need(c,m):
 if not c:raise ValueError(m)
def close(a,b):return math.isclose(a,b,rel_tol=1e-12,abs_tol=1e-8)
def freeze(run,work,portable):
 E.mkdir(parents=True,exist_ok=True)
 for name in ('raw.csv','summary.json','plan.json','complete.json','prepared.json'):
  shutil.copyfile(run/name,E/name)
 for name in ('run_confirmatory.py','PROTOCOL.md','PROTOCOL.r1-four-arm.md'):
  shutil.copyfile(run/'controller'/name,E/name)
 for name in ('static-assembly.json','outline-boundary.patch','build-complete.json','source-snapshot.json'):
  shutil.copyfile(work/name,E/name)
 for arm in ARMS:
  shutil.copyfile(run/'identities'/f'{arm}.json',E/f'{arm}-identity.json')
 a=read(R/'sources/manifest.json')['excerpts']['structured_boundary']['after']
 shutil.copyfile(R/a['full_source'],E/'mwpm-before.inl')
 shutil.copyfile(work/'source'/TARGET,E/'mwpm-outlined.inl')
 shutil.copyfile(portable,E/'portable-baseline-validation-receipt.json')

def summarize():
 p=read(E/'plan.json');done=read(E/'complete.json');declared=read(E/'summary.json')
 need(done['complete'] and done['state']=='complete','Follow-up incomplete')
 need(done['plan_sha256']==sha(E/'plan.json') and done['raw_sha256']==sha(E/'raw.csv'),'Follow-up seal changed')
 need(done['prepared_sha256']==sha(E/'prepared.json'),'Prepared seal changed')
 need(p['repeats']==8 and p['shots']==10000 and p['expected_calls']==120,'Unexpected sampling protocol')
 need(p['official_revision']=='e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd','Official identity changed')
 raw=list(csv.DictReader((E/'raw.csv').open()));need(len(raw)==len(done['calls'])==120,'Call count differs')
 groups={};seen=set()
 for row,item in zip(raw,p['schedule']):
  for key in ('schedule_index','repeat','case','method','arm','arm_position'):need(row[key]==str(item[key]),'Raw schedule mismatch')
  identity=(row['case'],row['arm'],int(row['repeat']));need(identity not in seen,'Repeated raw row');seen.add(identity)
  need(int(row['shots'])==10000 and int(row['elapsed_ns'])>0,'Invalid raw count/time')
  need(row['dataset_sha256']==p['cases'][row['case']]['dataset_sha256'],'Saved corpus changed')
  need(int(row['actual_candidate_witnesses'])>=int(row['observable_differences'])>=0,'Missing observable witness')
  value=int(row['elapsed_ns'])/10000;need(close(value,float(row['ns_per_shot'])),'Derived time mismatch')
  groups.setdefault(row['case'],{}).setdefault(row['arm'],{})[int(row['repeat'])]=value
 main=read(R/'summary.json');original={(r['case'],r['method']):r for r in main['structured']['rows']};results=[]
 for expected in declared['results']:
  case=expected['case'];method=expected['method'];g=groups[case]
  values={arm:[g[arm][k] for k in range(1,9)] for arm in ARMS}
  need(values==expected['arm_ns_per_shot'],'Run summary differs from raw')
  medians={a:statistics.median(v) for a,v in values.items()}
  for a in ARMS:need(close(medians[a],expected['arm_median_ns'][a]),'Run medians differ')
  comparisons=[]
  for c in expected['comparisons']:
   a,b=c['baseline'],c['candidate'];paired=[100*(y/x-1) for x,y in zip(values[a],values[b])]
   ratio=medians[a]/medians[b];need(close(ratio,c['speedup_ratio_of_medians']),'Run ratio differs')
   comparisons.append(dict(c,paired_change_percent=paired,paired_change_median_percent=statistics.median(paired),
                           paired_wins=sum(v<0 for v in paired),paired_losses=sum(v>0 for v in paired)))
  results.append(dict(case=case,method=method,arm_ns_per_shot=values,arm_median_ns=medians,
                      arm_observed_range_ns=expected['arm_observed_range_ns'],comparisons=comparisons,
                      original_window={k:original[case,method][k] for k in ('source_ns','candidate_ns','candidate_change_percent','repeat_ns')}))
 identities={a:read(E/f'{a}-identity.json') for a in ARMS}
 for arm in ARMS:need(identities[arm]['binary_sha256']==p['files']['bin/'+arm],'Arm receipt mismatch')
 before=identities['rebuilt-current'];after=identities['outlined-boundary']
 need(before['source_files'].keys()==after['source_files'].keys(),'Source paths differ')
 need([k for k in before['source_files'] if before['source_files'][k]!=after['source_files'][k]]==[TARGET],'Variant changes another file')
 need(sha(E/'mwpm-before.inl')==before['source_files'][TARGET] and sha(E/'mwpm-outlined.inl')==after['source_files'][TARGET],'Source snapshot mismatch')
 need((E/'mwpm-outlined.inl').read_text().replace('[[gnu::noinline]] ','',1)==(E/'mwpm-before.inl').read_text(),'Variant changes more than one attribute')
 need(sha(E/'outline-boundary.patch')==after['variant_patch_sha256'],'Patch receipt mismatch')
 static=read(E/'static-assembly.json')
 for arm,v in static.items():need(v['binary_sha256']==identities[arm]['binary_sha256'],'Assembly linked to another binary')
 refs=[dict(path=str(f.relative_to(R)),sha256=sha(f)) for f in sorted(E.iterdir()) if f.is_file() and f.name!='line-explanations.json']
 result=dict(revision='rejected-boundary-outline-r2',decision='rejected',native_calls=120,process_repeats=8,
             distinct_saved_records=30000,records_per_case=10000,case_count=3,arms=list(ARMS),
             official_revision=p['official_revision'],base_commit=p['base_commit'],runtime_commit=p['runtime_commit'],
             variant_identity='Frozen c5da snapshot plus exactly one [[gnu::noinline]] attribute; unmerged experiment snapshot',
             binary_sha256={a:x['binary_sha256'] for a,x in identities.items()},
             bit_identical_current_controls=p['bit_identical_current_controls'],results=results,static=static,references=refs,
             data_scope='New measurement window; repeats reuse three original 10,000-record inputs; the original 819-call matrix remains separate',
             aggregation='Median of eight complete-batch means. Ratio of medians and median paired change are separately reported. No outlier deletion.',
             decision_reason='Small median gains, mixed paired directions, and visible variation between byte-identical controls do not establish a stable single-function benefit across the selected cases.')
 main['boundary_outline']=result;write(R/'summary.json',main)
 return result

def pages(f):
 annotation=read(R/'sources/line-explanations.json');mapping=read(R/'sources/manifest.json')
 scopes=['structured_dispatch','structured_boundary','structured_shatter','structured_root']
 data={'model':'outline','sources':{k:copy.deepcopy(annotation['excerpts'][k]) for k in scopes},
       'origin':{'excerpts':{k:copy.deepcopy(mapping['excerpts'][k]) for k in scopes}}}
 for k in scopes:
  data['sources'][k]['before']=copy.deepcopy(annotation['excerpts'][k]['after'])
  data['origin']['excerpts'][k]['before']=copy.deepcopy(mapping['excerpts'][k]['after'])
 row=data['sources']['structured_boundary']['after'][0]
 row['source']='[[gnu::noinline]] '+row['source'];row['explanation']='保留inline定义的链接语义；GNU noinline属性要求此函数不展开到调用方，函数体与状态更新顺序不变。'
 source=(E/'mwpm-outlined.inl').read_text().splitlines()
 for k in scopes:
  # Other helper files are unchanged; the boundary-containing file is checked against the variant snapshot.
  for r in data['sources'][k]['after']:
   if data['origin']['excerpts'][k]['after']['path']==TARGET:need(source[r['line']-1]==r['source'],'Variant line mapping differs')
  origin=data['origin']['excerpts'][k]['after'];origin['commit']=None
  origin['snapshot']=f['base_commit']+' + patch '+sha(E/'outline-boundary.patch')
  origin['text']=''.join(r['source']+'\n' for r in data['sources'][k]['after'])
  origin['excerpt_sha256']=hashlib.sha256(origin['text'].encode()).hexdigest()
  origin.pop('export_path',None)
  if origin['path']==TARGET:
   origin['source_sha256']=sha(E/'mwpm-outlined.inl');origin['full_source']=str((E/'mwpm-outlined.inl').relative_to(R))
 write(E/'line-explanations.json',data)
 original=(R/'mechanisms/structured-primal-inlining.html').read_text()
 doc=original.replace('结构化 primal：让调用点看到完整事件处理体','未采纳变体：把边界处理保留为独立调用')
 doc=doc.replace('源码 edd29b678071','c5da快照 + 单行 noinline 属性（未合入）')
 doc=re.sub(r'<p class="intro">.*?</p>','<p class="intro">只在 handle_tree_hitting_boundary 添加 noinline。事件分派仍在头文件可见，边界状态更新顺序相同；演示显式调用、后代处理与返回。调用层次按源码展示，非真实机器调用栈；辅助函数仍可能内联。静态代码缩小与端到端计时分别核验。</p>',doc,count=1)
 trial=next(c for c in f['results'][0]['comparisons'] if c['baseline']=='rebuilt-current' and c['candidate']=='outlined-boundary')
 ta=f['static']['rebuilt-current']['functions'];tb=f['static']['outlined-boundary']['functions'];t='process_prepared_timeline_until_completion_impl<true>';n='process_prepared_timeline_until_completion_impl<false>'
 info=f'<section class="supplement"><h2>为何没有采纳</h2><p>三个设置中位耗时均小幅下降，但组件并行仅{trial["paired_wins"]}/{f["process_repeats"]}次配对更快，其配对耗时变化中位数仍为{trial["paired_change_median_percent"]:+.4f}%。两份完全相同的当前二进制也出现可见波动。本轮保留该变体为隔离证据，发布实现保持不变。</p><p>{f["case_count"]}个设置各{f["records_per_case"]:,}条保存记录×{f["process_repeats"]}次进程重复×{len(f["arms"])}臂，共{f["native_calls"]}次调用；固定Official e27正常单线程参考与四控制臂共享输入。<a href="../index.html#boundary-followup">完整数值与两个测量窗口</a> · <a href="../boundary-diff/index.html">完整单行变体差异</a> · <a href="../evidence/boundary-outline/PROTOCOL.md">冻结协议</a></p><p>执行边界：process_event → 边界处理 → 后代回收 → 返回。become_root演示输入已是根；冻结区域是dual辅助调用边界，其队列内部未模拟。NO_EVENT路径不调用边界函数。可视步骤不表示CPU周期。</p><details><summary>静态机器码与测量范围</summary><p>timeline&lt;true&gt;从{ta[t]["bytes"]}降至{tb[t]["bytes"]}字节；timeline&lt;false&gt;从{ta[n]["bytes"]}降至{tb[n]["bytes"]}字节。独立边界函数为{tb["Mwpm::handle_tree_hitting_boundary"]["bytes"]}字节；这是代码位置与调用边界改变，不是整段算法消失，也不能将字节数下降换算成周期收益。</p><p><a href="../evidence/boundary-outline/static-assembly.json">实际ELF符号范围和汇编哈希</a> · <a href="../evidence/boundary-outline/line-explanations.json">逐行解释与完整函数</a> · <a href="../summary.json">唯一报告数值来源</a></p></details></section>'
 doc=re.sub(r'<section class="supplement">.*?</section></main>',lambda _:info+'</main>',doc,count=1,flags=re.S)
 serialized=json.dumps(data,ensure_ascii=False).replace('</','<\\/')
 doc=re.sub(r'window.MECHANISM_DATA=.*?;</script>',lambda _:'window.MECHANISM_DATA='+serialized+';</script>',doc,count=1,flags=re.S)
 model=(R/'scripts/boundary_outline.js').read_text()
 doc=doc.replace('window.models={primal,generic,gate};',model+'\nwindow.models={primal,generic,gate,outline};')
 doc=doc.replace('活动树 ${s.num_active_trees} · arena活节点', '活动树 ${s.num_active_trees} · arena活节点')
 (R/'mechanisms/boundary-outline.html').write_text(doc)
 # Complete changed-file diff: one changed signature; show its entire function on both sides.
 D=R/'boundary-diff';D.mkdir(exist_ok=True);shutil.copyfile(E/'outline-boundary.patch',D/'diff.patch')
 before=annotation['excerpts']['structured_boundary']['after'];after=data['sources']['structured_boundary']['after']
 def lines(rows):
  return ''.join(f'<div class="line"><span>{r["line"]}</span><div><code>{html.escape(r["source"])}</code><p>{html.escape(r["explanation"])}</p></div></div>' for r in rows)
 title='完整变体差异：仅边界函数 noinline'
 style='*{box-sizing:border-box}body{margin:0;background:#f3f6f8;color:#203b50;font:16px/1.5 system-ui}header,main{padding:16px 22px}h1{font-size:25px;margin:5px 0}a{color:#176b96}.controls{display:flex;gap:10px;flex-wrap:wrap}button,select{font:inherit;padding:5px 10px}.columns{display:grid;grid-template-columns:1fr 1fr;gap:14px}.pane{background:white;padding:12px;min-width:0}.code{height:65vh;overflow:auto;resize:vertical}.line{display:grid;grid-template-columns:34px minmax(0,1fr);border-bottom:1px solid #e2e9ef;padding:4px}.line code{font:var(--size,15px)/1.6 monospace;white-space:pre-wrap;overflow-wrap:anywhere}.line p{font:13px/1.5 system-ui;margin:4px 0;color:#486174}.line:first-child{background:#fff0cd}details{margin:18px 0}pre{white-space:pre-wrap;overflow-wrap:anywhere}.unified .columns{grid-template-columns:1fr}.unified .code{height:45vh}@media(max-width:900px){.columns{grid-template-columns:1fr}header,main{padding:12px}.code{height:55vh}}'
 js="let size=15;document.querySelector('#view').onchange=e=>document.body.classList.toggle('unified',e.target.value==='unified');for(const [id,d]of [['up',1],['down',-1]])document.querySelector('#'+id).onclick=()=>{size=Math.max(12,Math.min(22,size+d));document.documentElement.style.setProperty('--size',size+'px');};"
 diffdoc=f'<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>{title}</title><style>{style}</style><header><a href="../index.html#boundary-followup">← 后续实验</a> · <a href="../mechanisms/boundary-outline.html">完整交互机制</a><h1>{title}</h1><p>rebuilt-current → outlined-boundary；冻结快照仅一个文件的一行发生变化，未创建新的运行时提交。</p><div class="controls"><select id="view"><option value="side">并排对照</option><option value="unified">上下对照</option></select><button id="up">A+</button><button id="down">A−</button><a href="diff.patch">原始完整 patch</a></div></header><main><div class="columns"><section class="pane"><h2>当前重编译控制</h2><div class="code">{lines(before)}</div></section><section class="pane"><h2>未采纳变体</h2><div class="code">{lines(after)}</div></section></div><details><summary>完整文件、来源与比较范围</summary><p>c5da46e12b3865e20d5a8c38157959ffefe8f417快照，运行时内容对应edd29b6780713dc78f6a99d67c4c4d192e9c52b5；变体身份由补丁和完整文件SHA标识。比较范围为整个源码清单，只有 src/beyond_blossom/structured/matcher/mwpm.inl 的第44行不同。这里完整展示变化涉及的函数，未变化的其余函数保留在原文件下载中。</p><a href="../evidence/boundary-outline/mwpm-before.inl">完整原文件</a> · <a href="../evidence/boundary-outline/mwpm-outlined.inl">完整变体文件</a> · <a href="manifest.json">文件/补丁/解释哈希</a></details></main><script>{js}</script></html>'
 (D/'index.html').write_text(diffdoc)
 write(D/'manifest.json',dict(scope='Entire frozen source file map; exactly one changed file and one signature line',base_commit=f['base_commit'],runtime_commit=f['runtime_commit'],variant='unmerged snapshot',
                             path=TARGET,patch_sha256=sha(D/'diff.patch'),before_sha256=sha(E/'mwpm-before.inl'),after_sha256=sha(E/'mwpm-outlined.inl'),
                             line_explanations_sha256=sha(E/'line-explanations.json'),shown_lines={'before':[44,57],'after':[44,57]},omitted_changes=[]))
 coverage=read(R/'optimization-coverage.json');coverage['revision']=2
 coverage['optimizations']=[x for x in coverage['optimizations'] if x['optimization_id']!='boundary-outline']
 coverage['optimizations'].append(dict(optimization_id='boundary-outline',title='未采纳的单函数noinline变体',decision='rejected',
  html={'path':'mechanisms/boundary-outline.html','sha256':sha(R/'mechanisms/boundary-outline.html')},
  line_explanations={'path':'evidence/boundary-outline/line-explanations.json','sha256':sha(E/'line-explanations.json')},
  summary={'path':'summary.json','sha256':sha(R/'summary.json')},source_scopes=scopes,
  comparison={'baseline':'rebuilt-current','candidate':'outlined-boundary','official':'official-sealed'},
  interaction={'entry':'Mwpm::process_event(const MwpmEvent&) → void','model':'outline','scenarios':['leaf','child','none']},
  publication={'kind':'offline'},verification={'source_and_line_mapping':'passed','browser':'pending current revision'}))
 for x in coverage['optimizations']:x['summary']['sha256']=sha(R/'summary.json')
 write(R/'optimization-coverage.json',coverage)

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',type=Path);p.add_argument('--work',type=Path,default=Path('/tmp/beyond-boundary-outline-20261112'))
 p.add_argument('--portable-receipt',type=Path,default=Path('/tmp/beyond-portable-baseline-validation/validation-receipt.json'));args=p.parse_args()
 if args.run:freeze(args.run,args.work,args.portable_receipt)
 f=summarize();pages(f);print('Verified 120 rows; canonical follow-up and two offline pages generated')
