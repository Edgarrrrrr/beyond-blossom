#!/usr/bin/env python3
"""Adapt the canonical completed summary to the standard summary-card schema."""
from pathlib import Path
import hashlib,json
R=Path(__file__).resolve().parents[1];p=R/'summary.json';s=json.loads(p.read_text())
if s['status']!='complete':raise SystemExit('Complete structured and generic results are required before summary-figure preparation')
st=s['structured'];generic=s['generic'];cases=[];shown=[]
short={'unstructured':'generic','structured':'structured','predecoded':'predecode','component-parallel':'component','pipelined':'pipeline','parallel-predecode':'parallel-pre'}
for row in st['rows']:
 cid=row['case']+'-'+row['method'];cases.append({'case_id':cid,'label':f'd{row["distance"]} {row["error_rate"]*100:g}% {short[row["method"]]}','baseline':row['official_ns']/1000,'candidate':row['candidate_ns']/1000,'unit':'µs/shot'})
# Show the smallest main speedup per method, plus global fastest/worst migration
# cases. Every measured point remains in cases for aggregate accounting.
for method in st['overview']:
 row=min((r for r in st['rows'] if r['method']==method['method']),key=lambda r:r['official_over_candidate']);shown.append(row['case']+'-'+row['method'])
for row in [max(st['rows'],key=lambda r:r['official_over_candidate']),min(st['rows'],key=lambda r:r['source_over_candidate'])]:
 cid=row['case']+'-'+row['method']
 if cid not in shown:shown.append(cid)
slow=generic['overview']['slower_than_source'];worst=max(slow,key=lambda r:r['candidate_change_percent']) if slow else None
migration_min=min(r['source_over_candidate'] for r in st['rows'])
migration_worst=max(st['rows'],key=lambda r:r['candidate_change_percent'])
data={'schema_version':1,'experiment_id':'BB-MIGRATION-20261112 / r1','title':'迁移后的代码路径与性能复核','status':'final',
 'baseline_label':'Official e27 · 单线程正常解码','candidate_label':'Beyond Blossom edd29b6 · 各配置',
 'scope':{'metric':'latency','direction':'lower_is_better','workload':'rotated memory-X，rounds=d；7个码距×3个噪声率×6配置；另有10个通用图迁移用例。',
 'measurement':f'每设置{st["unique_inputs_per_setting"]:,}条保存输入×{st["repeats"]}次批处理；取批均值中位数。','environment':'Xeon Gold 6230R · GCC12.2 · 结构化AVX-512VL · LTO关闭 · 候选1/2/4/8核',
 'coverage':'全126个官方比较点等权汇总；退化与输入数量详见正文','total_cases':len(cases)},
 'changes':[{'before':'结构化 primal 定义位于 .cc','after':'头文件包含 .inl 实现','mechanism':'调用点可见事件分派与交替树函数体；原矩阵未分离全部内联效果。'},
 {'before':'通用图直接创建 blossom roots','after':'合法半径与 tight 预解码','mechanism':'完整匹配达到 dual 下界才返回；部分结果全部导入，允许完整求解器改配。'},
 {'before':'稀疏高阶邻接扫描存在额外成本','after':'最多8×32局部探测，短shot缩预算','mechanism':'稀疏邻域回退普通 roots；商余数递推和类内访问器减轻门控成本。'}],
 'cases':cases,'display_case_ids':shown,'display_selection_reason':'每方法展示最小官方速度比，再补全局最快官方点及最慢迁移点；全部126点参与汇总。',
 'metrics':[{'label':'Source迁移最小速度比','value':f'{migration_min:.3f}×','scope':f'Source241 / candidate；最大耗时增加{migration_worst["candidate_change_percent"]:.3f}%，d{migration_worst["distance"]}/p{migration_worst["error_rate"]:g}，{migration_worst["threads"]}线程'},
 {'label':'通用图迁移速度比范围','value':'–'.join(f'{x:.3f}' for x in generic['overview']['source_speedup_range'])+'×',
  'scope':f'Source241 / candidate；hubs耗时+{worst["candidate_change_percent"]:.3f}%，观察范围重叠不等于统计等价' if worst else 'Source241 / candidate；全部10种工作负载'}],
 'decision':'采用独立后端与证书预解码实现，保留全部迁移退化点；主速度比为官方耗时 / 候选耗时。',
 'limitations':['这是10,000-shot结构化回归与固定通用图语料验证，不是重新采集论文百万-shot或M4矩阵。',
 '统计单位是重复批次/进程；相同保存输入重复运行不会增加独立样本。没有从批均值推导逐shot尾延迟。',
 '主官方串行参考与并行候选的核数不同；Source迁移比较按同名同线程配置配对。',
 '通用图只有Source241/on/off同轮对照。机制组合的总效果没有分摊给单项内联或语句。'],
 'source_refs':[{'label':'Canonical verified summary.json','path':'summary.json','sha256':hashlib.sha256(p.read_bytes()).hexdigest()}]}
if s.get('boundary_outline'):
 data['experiment_id']='BB-MIGRATION-20261112 / r2'
 data['decision']='保留发布实现及全部迁移退化点；后续120-call隔离验证未采纳单函数noinline变体。'
 data['limitations'][-1]+=' 后续三设置五臂结果单列，未与原819-call矩阵合并。'
(R/'overview.json').write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n');print(R/'overview.json')
