#!/usr/bin/env python3
"""Render the completed reproduction report from sealed canonical measurements."""
import argparse
import hashlib
import html
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = '75aedd433d1b129e5a973635320f199b5f26cbae'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=HERE)
    args = parser.parse_args()
    root = args.data.resolve()
    manifest = json.loads((root / 'manifest.json').read_text())
    def verified(name):
        payload = (root / name).read_bytes()
        if manifest['files'].get(str(name)) != hashlib.sha256(payload).hexdigest():
            raise ValueError('Archive hash mismatch: ' + str(name))
        return json.loads(payload)
    exporter = HERE.parents[1] / 'export_results.py'
    if manifest.get('source_commit') != SOURCE or manifest.get('exporter_sha256') != hashlib.sha256(exporter.read_bytes()).hexdigest():
        raise ValueError('This report requires its recorded source and renderer')
    data = verified('canonical.json')
    digest = manifest['files']['canonical.json']
    if data['profile'] != 'paper' or not data['complete']:
        raise ValueError('A hash-verified completed paper export is required')
    native = {(r['distance'], str(r['error_rate']), r['method']): r for r in data['native']}
    rates = ('0.0005', '0.001', '0.003')
    distances = (9, 13, 17, 21, 25, 29, 33)
    if len(native) != 126 or len(data['external']) != 105 or len(data['predecoder']) != 21:
        raise ValueError('The full 21-setting matrix is required')
    expected = {(d, p) for d in distances for p in rates}
    for group, methods in [('native', set(data['ablation_stages'])), ('external', {'parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf'})]:
        keys = {(r['distance'], str(r['error_rate']), r['method']) for r in data[group]}
        if len(keys) != len(data[group]) or keys != {(d, p, m) for d, p in expected for m in methods}:
            raise ValueError('Duplicated or incomplete method matrix')
    if {(r['distance'], str(r['error_rate']), r['policy']) for r in data['predecoder']} != {(d, '0.003', p) for d in distances for p in (0, 1, 2)}:
        raise ValueError('Duplicated or incomplete predecoder matrix')
    if any(r['execution'] != 'serial' or r['physical_policy'] != {0: 0, 1: 1, 2: 3}[r['policy']] for r in data['predecoder']):
        raise ValueError('Unexpected predecoder execution or policy')
    for group, shots in [('native', 1000000), ('external', 10000), ('predecoder', 1000000)]:
        for row in data[group]:
            if row['shots'] != shots or row['repeats'] != 3 or row['machine'] != 'x86_64' or not row['platform'].startswith('Linux'):
                raise ValueError('Unexpected measurement scope')
    if any(row['structured_backend'] != 'avx512vl' for row in data['native']):
        raise ValueError('Unexpected structured backend')
    configs = []
    for distance in distances:
        for rate in rates:
            config = verified(f'evidence/cases/d{distance}-p{rate}/config.json')
            if config['source_identity'] != {'commit': SOURCE, 'status': ''} or 'Gold 6230R' not in config['hardware']['model']:
                raise ValueError('Unexpected source or CPU model')
            configs.append(config)
    chosen = {}
    for d in distances:
        for p in rates:
            best = min((native[d, p, m] for m in data['best_configuration_pool']), key=lambda r: r['mean_us'])
            ref = native[d, p, 'reference']
            chosen[d, p] = (best, ref, ref['mean_us'] / best['mean_us'])
    speedups = [x[2] for x in chosen.values()]
    matrix_audit = verified('evidence/validation/main-matrix-audit.json')
    geomean = math.exp(sum(map(math.log, speedups)) / len(speedups))
    if (not matrix_audit['audit_passed'] or matrix_audit['source_commit'] != SOURCE or
            not math.isclose(matrix_audit['best_of_five_speedup']['geometric_mean'], geomean, rel_tol=1e-12)):
        raise ValueError('Independent matrix audit does not match this export')
    correctness = matrix_audit['correctness']
    if correctness['native_weight_differences_repeated_executions'] != 0 or correctness['external_exact_nonoptimal_repeated_executions'] != 0:
        raise ValueError('Unexpected exact-solver weight mismatch')
    distinct_observable = len(correctness['native_unique_case_shots_with_observable_differences'])
    repeated_observable = correctness['native_observable_differences_repeated_executions']
    historical = matrix_audit['historical_role_comparison']['fresh_latency_over_historical_by_role']
    history_ratios = [historical[m]['geometric_mean'] for m in data['best_configuration_pool']]
    hosts = sorted({r['host'] for r in native.values()})
    if hosts != [f'ares{i}' for i in range(1, 7)] or {c['placement']['host'] for c in configs} != set(hosts):
        raise ValueError('Unexpected collection hosts')
    spread = max(max(r['repeat_mean_us']) / min(r['repeat_mean_us']) - 1 for r in native.values()) * 100
    policies = {(r['distance'], r['policy']): r for r in data['predecoder']}
    degree_gains = [policies[d, 1]['mean_us'] / policies[d, 0]['mean_us'] for d in distances]
    macro_audit = verified('macro-audit.json')
    pmu_rows = [value for value in macro_audit['performance_operands'].values()
                if value.get('rule', {}).get('op') == 'pmu_miss_percent']
    pmu_note = ('d = 33、p = 0.3% 的独立 PMU 采集覆盖官方参考与结构化方法的 L2/L3，共四组独立计数，每组百万输入采集一遍。L2 分母为需求数据读请求，L3 分母为 LLC 读请求。'
                if len(pmu_rows) == 4 else
                f'本次提供 {len(pmu_rows)} 项 PMU 计数；缺失项在 macro-audit.json 中明确列出。')
    pmu_values = {(item['operands']['level'], item['operands']['method']): float(item['value']) for item in pmu_rows}
    pmu_table = [[level, f"{pmu_values[level, 'reference']:.4f}", f"{pmu_values[level, 'structured']:.4f}"]
                 for level in ('L2', 'L3') if all((level, method) in pmu_values for method in ('reference', 'structured'))]
    title = 'Beyond Blossom：本地论文规模复现'
    lead = (f'21 组设置在 {", ".join(hosts)} 完成。每组原生方法使用 1,000,000 条保存输入，'
            '外部方法使用相同输入的 10,000 条前缀，均重复计时三次。'
            f'逐设置选择最快 Beyond 配置后，相对固定官方参考的加速比为 {min(speedups):.2f}–{max(speedups):.2f}×，'
            f'几何平均为 {geomean:.2f}×。')
    rows = []
    for d in distances:
        cells = [str(d)]
        for p in rates:
            best, ref, ratio = chosen[d, p]
            cells.append(f'{best["mean_us"]:.3f} / {ref["mean_us"]:.3f} ({ratio:.2f}×)')
        rows.append(cells)
    headers = ['码距', 'p = 0.05%', 'p = 0.1%', 'p = 0.3%']
    notes = [
        '表内为 Beyond / 官方参考的平均 μs/shot，括号内为参考耗时除以 Beyond 耗时。各设置在同节点、同一预定 CPU 池中依次运行，各方法按配置使用 1/2/4/8 核。',
        '最快配置取自五种已测实现；它是逐设置的描述性选择，未测量运行时自适应切换。完整六阶段结果、每轮时间和实际配置见 CSV 与 canonical.json。',
        f'原生方法三轮批均值的最大相对跨度为 {spread:.2f}%（最大值/最小值−1）。三轮用于展示重复波动，未据此给出统计显著性结论。',
        f'主矩阵的原生最优权重差为零。{distinct_observable} 条独立输入出现等最优权重下的 observable 差异，涉及 {repeated_observable} 次重复执行；这些差异均通过实际候选边集的奇偶、权重和 observable 验证。外部精确方法的最优权重差为零，UF 近似方法的差额保留在独立审计中。未保存物理 observable 真值，因此不据此比较逻辑错误率。',
        f'在 p = 0.3% 的串行对照中，度数顺序预解码相对关闭预解码的加速比为 {min(degree_gains):.2f}–{max(degree_gains):.2f}×。简单策略采用明确的半边等分模式 3；历史 coordinate/full-cap 模式 2 保留为独立实现。',
        '原生计时覆盖正常解码调用、返回和结果存储；外部方法按各自原生接口计时，清理等边界存在差异。完整范围记录在每行 timing_scope，跨库曲线保留这些差异。',
        '构建和输入生成在计时前完成。策略计数单独采集，排除统计开销；所有正式作业通过 Slurm 在 ares1–6 运行。' + pmu_note,
        '本次采集来自 Linux x86-64 / Intel Xeon Gold 6230R，结构化后端为 AVX512VL。Apple M4 仅完成已有数据重画；此归档不含新的 M4 测量。',
        '新数据与论文历史归档分别保存。历史预解码表使用四线程 pipeline 和旧坐标顺序基线，本次采用串行模式，不能把两种口径的数值直接替换。',
        f'与历史主矩阵按设置对应后，五种 Beyond 配置的新/旧耗时比的几何平均为 {min(history_ratios):.4f}–{max(history_ratios):.4f}，参考角色为 {historical["reference"]["geometric_mean"]:.4f}。历史 CSV 的 Upstream 仅作角色对应，其构建身份未确认为本次独立官方参考。输入、构建和测量窗口不同，这些差异不能归因于单一代码改动。',
    ]
    figures = [
        ('incremental_ablation_1107_v1', '原生六阶段：绝对耗时与相对官方参考加速比'),
        ('decoder_comparison_shows_speedup_v3', '外部对照：实心点为百万输入，空心点为一万条前缀'),
        ('decoding_time', '三种串行预解码策略的完整解码耗时'),
        ('remaining_syndromes', '预解码后平均剩余事件数，单独统计'),
    ]
    links = [('canonical.json', '规范化测量数据'), ('native.csv', '原生方法'), ('external.csv', '外部方法'),
             ('predecoder.csv', '串行策略'), ('selected-configurations.csv', '逐设置配置选择'),
             ('macro-audit.json', '论文宏与缺失项审计'), ('manifest.json', '归档哈希'),
             ('archive-provenance.json', '小型证据与持久原始数据位置'),
             ('evidence/validation/main-matrix-audit.json', '独立重算、正确性与历史角色对照'),
             ('evidence/validation/policy-matrix-audit.json', '串行策略独立审计'),
             ('evidence/validation/pmu-audit.json', 'PMU 独立审计'),
             ('REPRODUCE.md', '仅从仓库重画图表'), ('../../README.md', '重新采集'),
             ('method/index.html', '简单等分策略完整交互'), ('method/code-diff/index.html', '固定版本代码对照')]
    md = [f'# {title}', '', lead, '', '![方法与完整矩阵总结](summary-card.png)', '', '| ' + ' | '.join(headers) + ' |', '| --- | --- | --- | --- |']
    md += ['| ' + ' | '.join(row) + ' |' for row in rows]
    for note in notes:
        md += ['', note]
    if pmu_table:
        md += ['', '独立 PMU 未命中率：misses / requests（%），d = 33、p = 0.3%。', '',
               '| 缓存层级 | 官方参考 | 结构化方法 |', '| --- | --- | --- |']
        md += ['| ' + ' | '.join(row) + ' |' for row in pmu_table]
    for stem, caption in figures:
        md += ['', f'![{caption}](presentation/{stem}.png)', '', caption]
    md += ['', '## 复现与证据', ''] + [f'- [{label}]({path})' for path, label in links]
    md += ['', '实现与采集源码：`75aedd433d1b129e5a973635320f199b5f26cbae`；官方参考：`e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd`。',
           '', '原始收据按字节归档，保留当时的路径。重画只读取仓库内的 canonical.json；新采集使用随库提供的依赖源码和配置。', '']
    (root / 'README.md').write_text('\n'.join(md))
    esc = html.escape
    table = '<thead><tr>' + ''.join('<th>' + esc(x) + '</th>' for x in headers) + '</tr></thead><tbody>'
    table += ''.join('<tr>' + ''.join('<td>' + esc(x) + '</td>' for x in row) + '</tr>' for row in rows) + '</tbody>'
    body = '<h1>' + esc(title) + '</h1><p class="lead">' + esc(lead) + '</p><figure><a href="summary-card.svg"><img src="summary-card.png" alt="方法与完整矩阵总结"></a></figure><div class="scroll"><table>' + table + '</table></div>'
    body += ''.join('<p>' + esc(note) + '</p>' for note in notes)
    if pmu_table:
        body += '<p>独立 PMU 未命中率：misses / requests（%），d = 33、p = 0.3%。</p><table><thead><tr><th>缓存层级</th><th>官方参考</th><th>结构化方法</th></tr></thead><tbody>'
        body += ''.join('<tr>' + ''.join('<td>' + esc(cell) + '</td>' for cell in row) + '</tr>' for row in pmu_table) + '</tbody></table>'
    body += ''.join(f'<figure><a href="presentation/{stem}.pdf"><img src="presentation/{stem}.png" alt="{esc(caption)}"></a><figcaption>{esc(caption)}</figcaption></figure>' for stem, caption in figures)
    body += '<h2>复现与证据</h2><ul>' + ''.join(f'<li><a href="{path}">{esc(label)}</a></li>' for path, label in links) + '</ul>'
    body += '<details><summary>技术身份</summary><p>实现与采集源码：75aedd433d1b129e5a973635320f199b5f26cbae<br>官方参考：e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd<br>canonical SHA256：' + digest + '</p></details>'
    css = 'body{font:17px/1.7 system-ui,sans-serif;color:#203348;max-width:1200px;margin:36px auto;padding:0 24px}h1{line-height:1.2;font-size:34px}a{color:#175e94}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}th,td{text-align:left;border-bottom:1px solid #dce4ed;padding:12px}th{background:#edf4fa}figure{margin:48px 0}img{display:block;max-width:100%;height:auto}figcaption{color:#52667a}details{background:#edf4fa;padding:16px;overflow-wrap:anywhere}.scroll{overflow:auto}.lead{font-size:20px}'
    (root / 'index.html').write_text('<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>' + title + '</title><style>' + css + '</style><main>' + body + '</main></html>')
    print(root / 'README.md')


if __name__ == '__main__':
    main()
