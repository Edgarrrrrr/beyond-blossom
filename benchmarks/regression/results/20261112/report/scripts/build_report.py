#!/usr/bin/env python3
"""Render compact report text/HTML from summary.json; no native work or figures."""
from pathlib import Path
import hashlib,html,json
R=Path(__file__).resolve().parents[1]
S=json.loads((R/'summary.json').read_text());V=json.loads((R/'validation.json').read_text())
LABEL={'unstructured':'通用图默认路径','structured':'结构化单线程','predecoded':'结构化预解码','component-parallel':'组件并行','pipelined':'流水线','parallel-predecode':'并行预解码'}
fmt=lambda v,n=3:f'{v:,.{n}f}'
esc=html.escape
refs=lambda group:' · '.join(f'<a href="{r["path"]}">{Path(r["path"]).name}</a>' for r in group)
def table(headers,rows):
 return '<div class="table-scroll"><table><thead><tr>'+''.join('<th>'+h+'</th>' for h in headers)+'</tr></thead><tbody>'+''.join('<tr>'+''.join('<td>'+str(c)+'</td>' for c in row)+'</tr>' for row in rows)+'</tbody></table></div>'
def ratio(value):return f'<strong class="{"gain" if value>=1 else "loss"}">{value:.3f}×</strong>'
generic=S.get('generic');structured=S.get('structured');title='Beyond Blossom：代码迁移与性能复核'
lead='通用图迁移对照已完成，结构化三方对照正在采集。'
if structured:
 negatives=[r for r in structured['rows'] if r['source_over_candidate']<1]
 worst=max(structured['rows'],key=lambda r:r['candidate_change_percent'])
 lead=f'本机结构化矩阵的{len(structured["rows"])}个官方比较点均更快；与同名同线程的来源版相比，{len(negatives)}个点耗时增加，最大为{worst["candidate_change_percent"]:.3f}%。全部比较共享保存输入，并保留逐配置结果。'
parts=[f'<p class="lead">{lead}</p><p>实现承接原始来源版 <b>Source 241</b>；运行时固定为 <b>Beyond Blossom edd29b6</b>。结构化主比较使用独立构建的 <b>Official e27</b>，迁移比较另列 <b>Source 241 / Beyond Blossom</b>。三个名称和完整提交映射见下方来源附件。</p>']
if (R/'figures/overview.png').exists():parts.append('<figure><a href="figures/overview.svg"><img src="figures/overview.png" alt="优化方法、主结果与适用范围汇总"></a><figcaption>方法与实际效果；数值取自本报告唯一 summary.json。</figcaption></figure>')
parts.append('<nav><a href="#results">原矩阵结果</a><a href="#boundary-followup">后续五臂验证</a><a href="#mechanisms">三个采用机制</a><a href="#validation">验证与复现</a><a href="code-diff/index.html">独立代码对照</a><a href="summary.json">唯一数值来源</a></nav>')
parts.append('<h2 id="results">测量结果与适用范围</h2>')
if generic:
 gr=generic['rows'];overview=generic['overview'];slow=overview['slower_than_source']
 lines=[]
 for r in gr:lines.append([esc(r['workload']),str(r['unique_input_records']),fmt(r['source_ns']/1000),fmt(r['predecode_off_ns']/1000),fmt(r['candidate_ns']/1000),ratio(r['source_over_candidate']),f'{r["candidate_change_percent"]:+.2f}%'])
 parts.append(f'<h3>通用图迁移：{len(gr)} 个固定工作负载</h3><p>下表对照版本为 Source 241；加速比 = Source耗时 / 候选on耗时，耗时变化 = (候选on耗时 / Source耗时 − 1) × 100%。预解码 off/on 通过显式设置 <code>Mwpm::predecode_enabled</code> 形成同一候选二进制的控制；这些任意图用例没有同轮官方上游数据。每个进程先对同一输入集合的 native 批次取中位数，再对七次交替启动的进程取中位数。表中时间单位为 µs/shot；保存记录数不会随重复执行增加。</p>')
 parts.append(table(['工作负载','保存记录','Source 241','候选 off','候选 on','Source/on ↑','耗时变化'],lines))
 parts.append(f'<p>全工作负载等权几何平均迁移加速比为 <b>{overview["source_geomean_speedup"]:.3f}×</b>，范围 {overview["source_speedup_range"][0]:.3f}–{overview["source_speedup_range"][1]:.3f}×；该等权矩阵摘要不代表生产流量分布。</p>')
 for r in slow:
  a,b=r['process_range_ns']['baseline'],r['process_range_ns']['on']
  parts.append(f'<p class="callout">{esc(r["workload"])} 仍有 {r["candidate_change_percent"]:.3f}% 耗时增加：{fmt(r["source_ns"]/1000)} → {fmt(r["candidate_ns"]/1000)} µs/shot。七个进程中位数的观察范围为 Source {fmt(a[0]/1000)}–{fmt(a[1]/1000)}，候选 {fmt(b[0]/1000)}–{fmt(b[1]/1000)} µs/shot。范围重叠属于描述性观察，没有据此宣称统计等价。</p>')
 parts.append('<details><summary>通用图原始数据、计时范围与全部进程中位数</summary>')
 for run in generic['runs']:
  parts.append(f'<p><b>{esc(run["name"])}</b>：{run["raw_rows"]} 条原始记录；{esc(run["timer"])}。{refs(run["references"])}</p>')
 parts.append('<pre class="json">'+esc(json.dumps([{'workload':r['workload'],'records':r['unique_input_records'],'process_repeats':r['process_repeats'],'batches_per_process':r['batches_per_process'],'process_medians_ns':r['process_medians_ns'],'complete_shots':r['complete_shots'],'skipped_shots':r['skipped_shots']} for r in gr],ensure_ascii=False,indent=2))+'</pre></details>')
if structured:
 st=structured;parts.append(f'<h3>结构化与默认路径：{st["input_sets"]} 个设置、每设置 {st["unique_inputs_per_setting"]:,} 条保存输入</h3><p>同轮比较 Official e27、Source 241 和候选。每配置重复 {st["repeats"]} 次，使用相同保存输入；方法顺序轮转，source/candidate 紧邻交替，官方调用在组间轮转。主比值为官方批均值中位数 / 候选批均值中位数；迁移比值单列。结构化内核使用 {esc(st["backend"])}；官方与 unstructured 路径采用 generic 后端。LTO关闭。批次均值的轮间范围不是逐 shot 尾延迟分布。</p>')
 rows=[]
 for r in st['overview']:rows.append([LABEL.get(r['method'],r['method']),ratio(r['official_geomean_speedup']),ratio(r['source_geomean_speedup']),f'{r["source_speedup_range"][0]:.3f}–{r["source_speedup_range"][1]:.3f}×',str(r['slower_than_source'])])
 parts.append(table(['配置','Official/候选 几何均值 ↑','Source/候选 几何均值 ↑','迁移加速比范围','慢于来源设置数'],rows))
 worst=sorted(st['rows'],key=lambda r:r['source_over_candidate'])[:6]
 parts.append('<h3>迁移比较中最慢的六个配置</h3><p>完整矩阵保留全部设置；这里按 Source/候选 从小到大选择，用于检查退化。</p>')
 parts.append(table(['设置 / 配置','Official µs/shot','Source µs/shot','候选 µs/shot','Official/候选 ↑','Source/候选 ↑'],[[f'd{r["distance"]}, p={r["error_rate"]:g} · {LABEL.get(r["method"],r["method"])}',fmt(r['official_ns']/1000),fmt(r['source_ns']/1000),fmt(r['candidate_ns']/1000),ratio(r['official_over_candidate']),ratio(r['source_over_candidate'])] for r in worst]))
 w=worst[0];parts.append(f'<p class="callout">最大迁移耗时增加出现在 {esc(w["case"])} 的 {LABEL[w["method"]]}：{w["candidate_change_percent"]:.3f}%，即 {fmt(w["source_ns"]/1000)} → {fmt(w["candidate_ns"]/1000)} µs/shot。三次批均值的范围为 Source {fmt(min(w["repeat_ns"]["source"])/1000)}–{fmt(max(w["repeat_ns"]["source"])/1000)}，候选 {fmt(min(w["repeat_ns"]["candidate"])/1000)}–{fmt(max(w["repeat_ns"]["candidate"])/1000)} µs/shot。这里只报告本轮观察，不把小幅差异解释为统计等价或单项机制成本。</p>')
 nonoverlap=[r for r in st['rows'] if min(r['repeat_ns']['candidate'])>max(r['repeat_ns']['source'])]
 parts.append('<p>以下退化配置的三次批均值范围不重叠：'+ '；'.join(f'{esc(r["case"])} {LABEL[r["method"]]} +{r["candidate_change_percent"]:.3f}%' for r in nonoverlap)+'。这类观察不能直接归为纯噪声。<a href="evidence/structured-audit/all_positive_median_latency_changes.csv">全部正延迟变化</a>与<a href="evidence/structured-audit/audit.json">独立审计</a>保留每次配对结果；5%筛查带不构成性能等价证明。</p>')
 parts.append('<details><summary>完整结构化矩阵与原始凭据</summary>'+refs(st['references'])+table(['设置','配置','线程','保存记录','Official µs','Source µs','候选 µs','Official/候选','Source/候选'],[[r['case'],r['method'],r['threads'],r['unique_input_records'],fmt(r['official_ns']/1000),fmt(r['source_ns']/1000),fmt(r['candidate_ns']/1000),ratio(r['official_over_candidate']),ratio(r['source_over_candidate'])] for r in st['rows']])+'</details>')
else:parts.append('<h3>结构化三方对照正在采集</h3><p>冻结协议为21个设置、每设置10,000条保存输入、三次重复、六个来源/候选配置加独立官方调用，共819次 native 调用。采集完成并验证原始凭据后填入此处；当前页没有使用试验轮次补数。</p>')
parts.append('<p class="note">本轮是代码迁移与回归验证。结构化10,000-shot矩阵、通用图固定语料和64-shot外部集成 smoke 各有独立口径；它们没有替代归档论文的一百万-shot采集。AE 的 <code>unstructured</code> 行运行当前库默认预解码路径；该 harness 的 <code>--predecode 0</code> 只控制结构化适配器。严格 on/off 对照来自上面的通用图专用实验。</p>')
if S.get('boundary_outline'):
 f=S['boundary_outline']
 def comparison(row,a,b):return next(c for c in row['comparisons'] if c['baseline']==a and c['candidate']==b)
 parts.append(f'<h2 id="boundary-followup">后续隔离验证：单个边界函数禁止内联，未采纳</h2><p>为检查原矩阵三个观察范围不重叠的退化设置，只给 <code>Mwpm::handle_tree_hitting_boundary</code> 添加 <code>[[gnu::noinline]]</code>，保留事件分派与其余内联函数。候选快照没有合入发布运行时。本次复用三个设置的 {f["distinct_saved_records"]:,} 条保存记录，每设置10,000条；每臂固定 {f["process_repeats"]} 次进程重复，共 {f["native_calls"]} 次调用。1024-shot blocks、两秒预热、实际CPU放置、正常清理及pipeline最后drain均沿用原契约，全部正确性检查通过。</p>')
 parts.append('<p>五臂为固定官方 <b>Official e27</b>、原始 <b>Source 241</b>、封存当前版 <b>current</b>、当前版重编译 <b>rebuilt</b> 和单行变体 <b>outlined</b>。Official仍走正常generic单线程；四个结构化控制臂保持同名同线程。current与rebuilt的benchmark、core库和Stim库均逐字节相同，此控制观察重复执行与顺序波动。五臂连续运行，采用预先冻结的轮转/部分镜像顺序；八轮中每臂在每个位置出现1或2次，不是完全平衡。</p>')
 parts.append(table(['新窗口 / µs·shot⁻¹','Official e27','Source 241','current','rebuilt','outlined','Official/outlined ↑'],[[r['case']+' · '+LABEL[r['method']],*[fmt(r['arm_median_ns'][a]/1000) for a in ('official-sealed','source-sealed','current-sealed','rebuilt-current','outlined-boundary')],ratio(comparison(r,'official-sealed','outlined-boundary')['speedup_ratio_of_medians'])] for r in f['results']]))
 parts.append('<p>上表为八次完整批均值的中位数；下面单函数比较同时保留“中位数之比”和“逐轮配对变化的中位数”，二者不是同一统计量。耗时变化=(outlined/rebuilt−1)×100%，负数表示更快。</p>')
 rows=[]
 for r in f['results']:
  c=comparison(r,'rebuilt-current','outlined-boundary');control=comparison(r,'current-sealed','rebuilt-current')
  rows.append([r['case']+' · '+LABEL[r['method']],f'{c["candidate_change_percent"]:+.4f}%',f'{c["paired_change_median_percent"]:+.4f}%',f'{c["paired_wins"]}/{f["process_repeats"]}',f'{min(c["paired_change_percent"]):+.3f}% … {max(c["paired_change_percent"]):+.3f}%',f'{control["candidate_change_percent"]:+.4f}%'])
 parts.append(table(['设置','变体耗时变化：中位数之比','配对变化中位数','变体更快次数','配对变化观察范围','相同binary rebuilt/current变化'],rows))
 c=comparison(f['results'][0],'rebuilt-current','outlined-boundary')
 parts.append(f'<p class="callout">决定：保留当前发布实现。三个设置的中位耗时降幅均不足1%，组件并行仅{c["paired_wins"]}/{f["process_repeats"]}次配对更快，配对变化中位数为{c["paired_change_median_percent"]:+.4f}%；相同binary控制也有可见波动。本轮提供了单函数的有限对照，没有建立足够稳定的增量收益来支持采用。所有八轮与所有block保留，不剔除异常值。</p>')
 parts.append('<h3>原窗口与后续窗口分别报告</h3>')
 parts.append(table(['设置','原819-call窗口 current/source耗时变化','后续120-call窗口 current/source耗时变化'],[[r['case']+' · '+LABEL[r['method']],f'{r["original_window"]["candidate_change_percent"]:+.4f}%',f'{comparison(r,"source-sealed","current-sealed")["candidate_change_percent"]:+.4f}%'] for r in f['results']]))
 parts.append('<p>组件并行在新窗口的方向反转，说明该效果对运行窗口敏感；这不会推翻原窗口的测量，也不能证明原退化“只是噪声”。两个窗口的重复数、顺序和比较集合不同，不能直接合并成一个总体。此处只做描述性比较，没有据观察范围或固定阈值宣称统计等价。</p>')
 t='process_prepared_timeline_until_completion_impl<true>';n='process_prepared_timeline_until_completion_impl<false>'
 a=f['static']['rebuilt-current']['functions'];b=f['static']['outlined-boundary']['functions']
 parts.append(f'<p>实际ELF中，timeline&lt;true&gt;从{a[t]["bytes"]}缩至{b[t]["bytes"]}字节，timeline&lt;false&gt;从{a[n]["bytes"]}缩至{b[n]["bytes"]}字节。移出的边界函数仍有{b["Mwpm::handle_tree_hitting_boundary"]["bytes"]}字节；这是代码位置和调用边界变化，静态容量下降不等于动态周期收益。<a href="mechanisms/boundary-outline.html">打开未采纳变体的完整交互页</a> · <a href="boundary-diff/index.html">完整单行变体diff与逐行解释</a></p>')
 parts.append('<details><summary>五臂全部数据、冻结协议和来源</summary>'+refs(f['references'])+'<pre class="json">'+esc(json.dumps([{'case':r['case'],'arm_ns_per_shot':r['arm_ns_per_shot'],'comparisons':r['comparisons']} for r in f['results']],ensure_ascii=False,indent=2))+'</pre></details>')
parts.append('<h2 id="mechanisms">实现变化与三个采用机制</h2><p>库名与 namespace 统一为 <code>beyond_blossom</code>，构建前缀为 <code>BB_</code>。<code>core</code> 保留共享基础类型；<code>unstructured</code> 使用动态邻接图；<code>structured</code> 独立保留网格、局部预解码与并行路径；<code>driver</code> 提供公共入口。历史测量身份保持不变。</p><div class="cards">')
mechanisms=[('structured-primal-inlining','1. 结构化 primal 内联','把事件分派与交替树操作从 .cc 移入头文件可见的 .inl；减少无 LTO 时的翻译单元障碍。状态更新顺序保持一致，重型结果提取仍留在 .cc。','边界事件 → 树旋转/回收 → 区域匹配 → 返回'),('certified-generic-predecode','2. 有证书的通用图预解码','按当前边权保持 dual 可行性，只配 tight 边。完整匹配达到下界后直接提取；部分结果导入全部 singleton，完整 blossom 仍可改配。','合法增长 → tight 配对 → 完整检查或全部状态导入'),('bounded-density-gate','3. 有界局部密度门控','先检查活动节点平均度和直接边界，再做最多8×32邻接探测；局部活动比例不足1/8时回退。短 syndrome 按 ceil(seed数/4) 缩小采样预算。','局部采样 → 阈值 → 普通 roots 或预解码')]
for ident,title2,body,flow in mechanisms:parts.append(f'<article><h3>{title2}</h3><p>{body}</p><p class="flow">{flow}</p><a href="mechanisms/{ident}.html">打开完整交互页 →</a></article>')
parts.append('</div><p>商余数递推保持与 floor(k×N/count) 相同的节点索引；每邻域32槽使用右移计算，类内 <code>seed_index</code> 提供内联机会。已有 Release 对象反汇编保留为<a href="evidence/validation/generic-codegen.txt">机器码证据</a>，其范围是库对象而非重新反汇编的最终 ELF。端到端结果检验组合实现，没有把组合收益分配给单一语句或内联动作。</p><p><a href="code-diff/index.html">打开独立代码对照</a> · <a href="code-diff/diff.patch">下载原始 patch</a> · <a href="sources/manifest.json">固定来源和范围清单</a> · <a href="optimization-coverage.json">机制覆盖清单</a></p>')
parts.append(f'<h2 id="validation">验证与复现</h2><p>从 clean edd29b6 的源码包构建 wheel，在仓库外安装与导入，{V["package"]["tests"]} 项 Python 测试通过；CLI、公共与直接 native Structured API、独立 CMake consumer 和许可证随包检查均有回执。AE 的 {V["ae_tests"]["passed"]} 项测试及 {V["ae_tests"]["subtests_passed"]} 项子测试通过。</p><p>正式 AE native 构建封存源码、依赖与二进制。七种方法在同一批 {V["native_smoke"]["shots_per_method"]} 输入上完成 smoke；最终 wheel 的转换器配合五种外部 native 方法，对 {V["external_smoke"]["shots_per_method"]} 条输入完成 {V["external_smoke"]["native_parity_checks"]} 次 parity 和 {V["external_smoke"]["independent_chain_checks"]} 次独立校正链检查。Parity/Fusion要求整数最优，其余三种UF记录最优性差距。集成 smoke 期间有并发构建，性能结果仅使用后续串行冻结回归采集。</p>')
parts.append(f'<p>Release与ASan/UBSan各有{V["ctest"]["release"]["passed"]}项C++测试通过。LeakSanitizer在本机ptrace环境下关闭；最终源码封存与构建身份单独归档。<a href="evidence/validation/release-final-ctest.log">Release日志</a> · <a href="evidence/validation/asan-final-ctest.log">ASan/UBSan日志</a></p>')
if S.get('boundary_outline'):
 portable=json.loads((R/'evidence/boundary-outline/portable-baseline-validation-receipt.json').read_text())
 parts.append(f'<p>后续可移植构建参数修复对应 {esc(portable["commit"][:12])}；本机 {esc(portable["compiler"])} 下，{esc(portable["linked_objects_equal"])} 个链接对象与核心库均和原构建逐字节相同，{portable["ctest_discovered_count"]}项完整CTest通过。该检查没有新计时。<a href="evidence/boundary-outline/portable-baseline-validation-receipt.json">本机源码与构建收据</a>。实际GCC15的CI为独立验证；当前随包收据尚未记录其最终状态。</p>')
parts.append('<p><a href="validation.json">验证摘要与原始回执索引</a> · <a href="evidence/validation/package-audit.json">源码包、wheel、SDK审计</a> · <a href="evidence/validation/ae-tests.log">AE测试日志</a> · <a href="evidence/validation/asan-final-validation.json">最终356项C++测试的源码和artifact收据</a></p><h3>三个复现入口</h3><ol><li><b>最小验证：</b>按仓库 <code>AE/README.md</code> 构建固定官方参考与 candidate，再运行 <code>reproduce.py run --profile smoke</code>；本报告的native与外部闭环均已执行。</li><li><b>完整重新采集：</b>使用 <code>benchmarks/regression/run_generic.py</code> 与 <code>run_structured.py</code>，按封存协议中的 commands 创建新目录；官方、来源和候选读取相同输入。论文级百万-shot矩阵使用AE教程的独立命令。</li><li><b>仅重算：</b>运行本目录的 <code>scripts/summarize_results.py</code> 读取已完成的三个run，再运行 <code>build_pages.py</code> 和 <code>build_report.py</code>。所有数值由 <code>summary.json</code> 派生。</li></ol><p>具体命令见 <a href="REPRODUCE.md">复现说明</a>。大二进制和输入语料由工作区归档另行提供；本紧凑包保留小型 raw、采集凭据与哈希。大归档的<a href="evidence/validation/root-archives-verification.json">完整性校验</a>与<a href="evidence/validation/replay-verification.json">异路径重算回执</a>随包保留。在线发布未执行；所有机制页和源码对照可直接离线打开。</p>')
parts.append('<details><summary>完整技术身份与统计约定</summary>'+table(['角色','身份'],[['原始迁移来源',S['migration_source_revision']],['本地历史恢复tag','source-2411603 → 9651df809d8c2c034717bd3b036c35fda1c90fcb'],['被测实现',S['implementation_commit']],['固定官方上游',S['official_revision']],['结构化采集器后续提交',S['collector_commits'][-1]],['机器','Intel Xeon Gold 6230R · zeus · Linux x86_64'],['比较方向','baseline/candidate，大于1更快；耗时变化=(candidate/baseline−1)×100%']])+f'<p>summary.json SHA256：<code>{hashlib.sha256((R/"summary.json").read_bytes()).hexdigest()}</code></p><p>完整矩阵保留每个工作负载的正负结果。重复使用固定输入只增加执行次数。范围描述观察到的批次或进程波动，不推导逐 shot 分位数、统计显著性或生产分布。</p></details>')
css='''*{box-sizing:border-box}body{margin:0;background:#f4f7fa;color:#1f3749;font:16px/1.65 system-ui,sans-serif}main{max-width:1180px;margin:auto;padding:35px 28px 70px}header{border-bottom:3px solid #248477;margin-bottom:24px}h1{font-size:32px;line-height:1.25;margin:0 0 12px}h2{font-size:24px;margin-top:40px}h3{font-size:18px;margin:22px 0 9px}p{max-width:1080px}a{color:#176c97}nav{display:flex;gap:16px;flex-wrap:wrap;padding:14px 0;border-block:1px solid #d8e3eb}.lead{font-size:20px;font-weight:650}.meta,.note{font-size:13px;color:#5a7182}.table-scroll{overflow:auto}table{border-collapse:collapse;background:#fff;width:100%;font-size:14px}th{background:#e6eff5;text-align:left}th,td{padding:10px;border-bottom:1px solid #dbe5ec}td:nth-child(n+2){font-variant-numeric:tabular-nums;white-space:nowrap}.gain{color:#087762}.loss{color:#9c4330}.callout{padding:14px 18px;border-left:4px solid #b66b42;background:#fff5ed}details{background:#fff;border:1px solid #d7e1e8;border-radius:8px;padding:14px;margin:18px 0}summary{font-weight:650;cursor:pointer}.json{max-height:420px;overflow:auto;font:12px/1.55 ui-monospace,monospace}.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:15px}.cards article{background:#fff;padding:17px;border:1px solid #d6e3eb;border-radius:9px}.cards h3{margin-top:0}.flow{font-size:14px;color:#31637c}code{font-size:.9em;overflow-wrap:anywhere}figure{margin:20px 0}figure img{max-width:100%;height:auto}figcaption{font-size:13px;color:#5b7281}@media(max-width:850px){main{padding:22px 14px}.cards{grid-template-columns:1fr}h1{font-size:27px}table{font-size:12px}td,th{padding:8px}}'''
doc='<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>'+title+'</title><style>'+css+'</style><main><header><p class="meta">离线技术报告 · 2026-11-12 · 修订 r2 · '+('采集完成' if S['status']=='complete' else '结构化采集中')+'</p><h1>'+title+'</h1></header>'+''.join(parts)+'</main></html>'
(R/'index.html').write_text(doc)
# The compact machine-readable summary is canonical; Markdown is a stable index.
(R/'REPORT.md').write_text('# '+title+'\n\n'+lead+'\n\n打开 [完整离线报告](index.html) 阅读从 summary.json 生成的结果表、正负案例与方法说明。\n\n- [唯一数值来源](summary.json)\n- [验证摘要](validation.json)\n- [源码对照](code-diff/index.html)\n- [三个采用机制与一个未采纳变体](optimization-coverage.json)\n- [复现说明](REPRODUCE.md)\n\n实现：'+S['implementation_commit']+'。官方基线：'+S['official_revision']+'。迁移来源：'+S['migration_source_revision']+'。\n')
if S.get('boundary_outline'):
 with (R/'REPORT.md').open('a') as stream:stream.write('\n后续五臂隔离验证保留为独立测量窗口：120次调用、8次进程重复、30,000条保存记录。单函数noinline变体未采纳；原819-call矩阵不变。见[后续结果](index.html#boundary-followup)、[交互机制](mechanisms/boundary-outline.html)与[完整变体差异](boundary-diff/index.html)。\n')
print(R/'index.html')
