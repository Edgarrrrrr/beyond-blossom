#!/usr/bin/env python3
"""Rebuild the source-backed summary card; no measurements or network access.

Usage: python summary-card.py
Requires matplotlib. Reads canonical.json and evidence/validation/main-matrix-audit.json
beside this script. --canonical/--audit can select the original sealed copies.
Writes only summary-card-input.json, summary-card.{svg,png,json}.
The custom layout shows all 21 cases, including absolute latency, in one image.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(test, message):
    if not test:
        raise ValueError(message)


def close(a, b):
    require(math.isclose(a, b, rel_tol=1e-12, abs_tol=1e-12), f"Values disagree: {a}, {b}")


def extract(canonical_path, audit_path, output):
    hashes = [sha(canonical_path), sha(audit_path)]
    data = json.loads(canonical_path.read_text())
    audit = json.loads(audit_path.read_text())
    require(data['complete'] and data['profile'] == 'paper' and audit['audit_passed'],
            'Completed paper export and passing independent audit required')
    stages = ['reference', 'structured', 'predecoded', 'component-parallel',
              'pipelined', 'parallel-predecode']
    require(data['ablation_stages'] == stages and data['best_configuration_pool'] == stages[1:],
            'Unexpected six-stage comparison')
    native = {(r['distance'], float(r['error_rate']), r['method']): r for r in data['native']}
    distances = sorted({r['distance'] for r in data['native']})
    rates = sorted({float(r['error_rate']) for r in data['native']})
    require(distances == [9, 13, 17, 21, 25, 29, 33] and rates == [.0005, .001, .003],
            'Expected full 21-setting matrix')
    require(len(native) == len(data['native']) == 126, 'Duplicate or missing native records')
    expected_external = {(d, p, m) for d in distances for p in rates
                         for m in ('parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf')}
    require({(r['distance'], float(r['error_rate']), r['method']) for r in data['external']}
            == expected_external and len(data['external']) == len(expected_external),
            'Duplicate or missing external records')
    for group, n in [('native', 1000000), ('external', 10000)]:
        for row in data[group]:
            require(row['shots'] == n and row['repeats'] == 3, 'Unexpected sample scope')
            require(row['platform'].startswith('Linux') and row['machine'] == 'x86_64',
                    'Unexpected platform')
            if group == 'native':
                close(row['mean_us'], sum(row['repeat_elapsed_ns']) / (n * 3 * 1000))
    require(not data['m4'], 'This card specifies no fresh M4 measurements')
    require(sorted(audit['hardware']) == [f'ares{i}' for i in range(1, 7)], 'Unexpected nodes')
    require(all('6230R' in r['model'] and r['backend'] == 'avx512vl'
                for r in audit['hardware'].values()), 'Unexpected CPU/backend')
    audited = {(r['distance'], float(r['error_rate'])): r for r in audit['cases']}
    cases = []
    for d in distances:
        for p in rates:
            rows = [native[d, p, m] for m in stages]
            require(len({r['host'] for r in rows}) == 1 and len({r['input_sha256'] for r in rows}) == 1,
                    'Within-case native host/input mismatch')
            ref = rows[0]
            chosen = min(rows[1:], key=lambda r: r['mean_us'])
            item = dict(case_id=f'd{d}-p{p:g}', distance=d, error_rate=p,
                        label=f'd={d}, p={p:g}', baseline=ref['mean_us'], candidate=chosen['mean_us'],
                        unit='us/shot', selected_method=chosen['method'], host=ref['host'],
                        input_sha256=ref['input_sha256'], speedup=ref['mean_us'] / chosen['mean_us'])
            a = audited[d, p]
            close(item['baseline'], a['native_means_us']['reference'])
            close(item['candidate'], a['best_candidate_mean_us'])
            close(item['speedup'], a['official_over_best_candidate'])
            require(item['selected_method'] == a['best_candidate_method'], 'Selection mismatch')
            cases.append(item)
    speeds = [r['speedup'] for r in cases]
    gm = math.exp(sum(map(math.log, speeds)) / len(speeds))
    for key, value in [('geometric_mean', gm), ('minimum', min(speeds)), ('maximum', max(speeds))]:
        close(value, audit['best_of_five_speedup'][key])
    correctness = audit['correctness']
    require(correctness['native_weight_differences_repeated_executions'] == 0, 'Weight mismatch')
    unique_ties = correctness['native_unique_case_shots_with_observable_differences']
    require(len(unique_ties) == 1, 'Unexpected distinct observable tie count')
    require(all(r['actual_candidate_witnesses'] == r['observable_differences'] and r['status'] == 'passed'
                for r in correctness['native_calls_with_observable_differences']), 'Missing tie witness')
    history = audit['historical_role_comparison']['fresh_latency_over_historical_by_role']
    historical_gms = [history[m]['geometric_mean'] for m in stages[1:]]
    require(all(history[m]['count'] == len(cases) for m in stages), 'Historical coverage mismatch')
    result = dict(
        schema_version=1, experiment_id='20261112-slurm-paper', status='final',
        title='Beyond Blossom: method and measured effect',
        baseline_label='Official upstream ' + audit['official_revision'][:8],
        candidate_label='Best of five measured Beyond configurations',
        source_commit=audit['source_commit'], official_revision=audit['official_revision'],
        scope=dict(metric='latency', direction='lower_is_better', total_cases=len(cases),
                   workload='Surface-code circuit-level noise, d=9..33; p=0.05%, 0.1%, 0.3%',
                   measurement='Pooled mean of three full-batch repeats; native normal decode lifecycle',
                   environment='Linux / Intel Xeon Gold 6230R / ares1-6 / AVX512VL',
                   coverage='All 21 settings; within-case same host and saved input',
                   native_shots=1000000, external_shots=10000, repeats=3, native_methods=stages,
                   fresh_m4=False, native_cpu_counts=sorted({r['threads'] for r in native.values()})),
        changes=[
            dict(before='Official sparse-blossom reference', after='Structured graph and SIMD',
                 mechanism='Exploit fixed grid adjacency and vectorized local graph operations.'),
            dict(before='Full solver starts from every defect', after='One-hop predecode plus exact fallback',
                 mechanism='Accept tight local matches; pass valid partial state to the full solver.'),
            dict(before='Serial execution', after='Component parallelism, pipeline, parallel predecode',
                 mechanism='Use X/Z independence and overlap work across shots and worker teams.')],
        stages=[dict(method=m, label=data['method_labels'][m],
                     threads=sorted({r['threads'] for r in native.values() if r['method'] == m}))
                for m in stages], cases=cases,
        metrics=dict(geometric_mean_speedup=gm, minimum_speedup=min(speeds), maximum_speedup=max(speeds),
                     wins=sum(x > 1 for x in speeds),
                     native_weight_differences=correctness['native_weight_differences_repeated_executions'],
                     distinct_observable_ties=len(unique_ties),
                     repeated_observable_ties=correctness['native_observable_differences_repeated_executions'],
                     historical_role_gm_range=[min(historical_gms), max(historical_gms)],
                     historical_reference_role_gm=history['reference']['geometric_mean']),
        historical_role_comparison={m: history[m] for m in stages},
        decision='Retain all five configurations; report the best observed configuration per setting.',
        limitations=[
            'Best-of-five is descriptive selection on the measured settings, not measured adaptive switching or an independent validation set.',
            'Composite end-to-end gains cannot be attributed to one code change; methods use 1/2/4/8 cores.',
            'Historical new/old latency ratios compare different inputs, builds and acquisition windows; they do not establish a code regression or statistical equivalence.',
            'External solvers use a 10k prefix, their own timing scope and integer quantization. Physical observable truth is absent; no logical-error-rate claim. No fresh Apple M4 run.'],
        source_refs=[dict(label=label, path=os.path.relpath(path, output), sha256=digest)
                     for label, path, digest in zip(['Canonical export', 'Independent main-matrix audit'],
                                                   [canonical_path, audit_path], hashes)])
    require(hashes == [sha(canonical_path), sha(audit_path)], 'Sources changed during extraction')
    return result


def render(data, output):
    os.environ.setdefault('MPLCONFIGDIR', str(Path(tempfile.gettempdir()) / 'beyond-summary-mpl'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'svg.fonttype': 'path',
                         'svg.hashsalt': 'beyond-summary-20261112', 'axes.unicode_minus': False})
    fig = plt.figure(figsize=(18, 12.5), dpi=180, facecolor='#f9fbfe')
    ink, muted, blue, teal = '#12243b', '#4b6079', '#2159a0', '#087f8c'
    texts = []

    def text(x, y, value, size=12, color=ink, weight='normal', ha='left'):
        item = fig.text(x, y, value, fontsize=size, color=color, weight=weight,
                        ha=ha, va='top', linespacing=1.4)
        texts.append(item)
        return item

    def box(x, y, w, h, color, edge='none'):
        fig.add_artist(FancyBboxPatch((x, y), w, h, boxstyle='round,pad=0.008,rounding_size=0.009',
                       linewidth=.8, facecolor=color, edgecolor=edge, transform=fig.transFigure, zorder=0))

    m, s = data['metrics'], data['scope']
    text(.035, .968, 'BEYOND BLOSSOM  /  PAPER-SCALE REPRODUCTION  /  12 NOV 2026', 11, blue, 'bold')
    text(.035, .933, data['title'], 29, ink, 'bold')
    text(.035, .885, 'Six-stage comparison: from the official reference to structured and parallel execution', 13, muted)
    titles = ['Official reference', 'Structured + SIMD', 'One-hop predecode', 'X/Z components', 'Shot pipeline', 'Parallel predecode']
    subtitles = ['Sparse blossom', 'Fixed grid adjacency', 'Tight matches + fallback', 'Independent components', 'Overlap across shots', 'Split local work']
    for i, stage in enumerate(data['stages']):
        x = .035 + i * .159
        box(x, .781, .143, .077, '#e8eef6' if i == 0 else '#e6f2f4')
        text(x + .008, .846, f'{i+1:02d}  {titles[i]}', 11, blue if i == 0 else teal, 'bold')
        text(x + .008, .818, subtitles[i] + '\n' + '/'.join(map(str, stage['threads'])) + ' core' + ('s' if stage['threads'] != [1] else ''), 9.5, muted)
        if i < 5:
            fig.add_artist(FancyArrowPatch((x + .146, .819), (x + .156, .819),
                           arrowstyle='-|>', mutation_scale=12, color=muted, transform=fig.transFigure))
    boxes = [(.035, .277), (.336, .277), (.637, .328)]
    for x, w in boxes:
        box(x, .656, w, .098, '#ffffff', '#dce6f0')
    text(.050, .738, 'OFFICIAL / BEST BEYOND', 10, muted, 'bold')
    text(.050, .713, f"{m['geometric_mean_speedup']:.2f}×", 30, teal, 'bold')
    text(.170, .699, 'geometric mean\nall 21 settings, equal weight', 10, muted)
    text(.351, .738, 'RANGE ACROSS ALL SETTINGS', 10, muted, 'bold')
    text(.351, .713, f"{m['minimum_speedup']:.2f}–{m['maximum_speedup']:.2f}×", 28, blue, 'bold')
    text(.652, .738, f"{s['total_cases']} SETTINGS  ·  {s['repeats']} REPEATS", 10, muted, 'bold')
    text(.652, .709, '1,000,000 native / 10,000 external', 16, ink, 'bold')
    text(.652, .680, 'Native = official + Beyond; external uses a 10k input prefix', 9.5, muted)

    text(.035, .627, 'All settings: official mean latency / selected Beyond mean latency', 13, ink, 'bold')
    text(.035, .600, 'Large: speedup (>1 faster)    Small: official / Beyond, µs per shot', 10, muted)
    x0, ytop, row_h = .087, .539, .0392
    widths = .167
    rates = sorted({r['error_rate'] for r in data['cases']})
    distances = sorted({r['distance'] for r in data['cases']})
    by_case = {(r['distance'], r['error_rate']): r for r in data['cases']}
    text(.047, .569, 'd', 11, muted, 'bold')
    for j, p in enumerate(rates):
        text(x0 + (j+.5)*widths, .569, f'p = {p*100:g}%', 11, muted, 'bold', 'center')
    for i, d in enumerate(distances):
        y = ytop - i * row_h
        text(.047, y-.009, str(d), 12, ink, 'bold')
        for j, p in enumerate(rates):
            row = by_case[d, p]
            intensity = (row['speedup'] - 1) / 10
            color = plt.get_cmap('Blues')(.08 + intensity * .30)
            fig.add_artist(Rectangle((x0+j*widths, y-row_h+.002), widths-.007, row_h-.004,
                                    facecolor=color, linewidth=0, transform=fig.transFigure, zorder=0))
            center = x0+(j+.5)*widths-.0035
            text(center, y-.004, f"{row['speedup']:.2f}×", 12, ink, 'bold', 'center')
            text(center, y-.023, f"{row['baseline']:.3g} / {row['candidate']:.3g}", 9, muted, ha='center')
    text(.035, .244, 'Mean = summed integer batch time / summed shots; best = minimum of five candidate means.', 9.4, muted)
    text(.035, .224, 'Retain all five options. Best-of-five is descriptive, without measured adaptive switching or single-stage attribution.', 9.4, muted)

    box(.635, .270, .330, .307, '#edf5f7')
    text(.651, .560, 'MEASUREMENT SCOPE', 10, teal, 'bold')
    text(.651, .533, 'Linux · Xeon Gold 6230R · AVX512VL\nares1–6; each setting stays on one node', 12, ink, 'bold')
    text(.651, .485, 'Same input and CPU pool within each setting.\nOfficial: 1 core; Beyond configurations: 1/2/4/8 cores.\nNative timer includes normal decode, return and storage.', 10, muted)
    text(.651, .420, 'CORRECTNESS, WITH THE EXCEPTION VISIBLE', 10, teal, 'bold')
    text(.651, .391, f"{m['native_weight_differences']} native optimum-weight differences", 15, ink, 'bold')
    text(.651, .357, f"{m['distinct_observable_ties']} distinct equal-optimum observable tie\n({m['repeated_observable_ties']} executions); actual candidate edge sets checked\nfor parity, weight and observable.", 10.5, muted)
    text(.651, .289, 'No physical observable truth: no logical-error-rate claim.', 9.4, muted)

    box(.035, .092, .930, .106, '#fff2e5')
    text(.050, .182, 'HISTORICAL COMPARISON IS A SEPARATE DENOMINATOR', 10, '#955119', 'bold')
    lo, hi = m['historical_role_gm_range']
    text(.050, .156, f'New / historical latency: {lo:.4f}–{hi:.4f}× across the five method-wise geometric means (21 settings each).', 12, ink, 'bold')
    text(.050, .129, f"Reference role (historical Upstream): {m['historical_reference_role_gm']:.4f}×. Build identity differs or is unverified; no single-code-change attribution.", 10, muted)
    text(.050, .108, 'External comparisons have different sample counts, timing scopes and quantization. No new Apple M4 measurements.', 10, muted)

    text(.035, .068, f"Implementation {data['source_commit'][:12]}  ·  Official baseline {data['official_revision'][:12]}  ·  All 21 settings shown", 9, muted)
    text(.035, .047, 'Sources: canonical.json [' + data['source_refs'][0]['sha256'][:12] + ']  +  main-matrix-audit.json [' + data['source_refs'][1]['sha256'][:12] + ']', 8.5, muted)
    text(.035, .028, 'Full data, exact formulas, source hashes and limitations: summary-card-input.json and summary-card.json. Main report retains every method and repeat.', 8.5, muted)

    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    for item in texts:
        b = item.get_window_extent(renderer)
        require(b.x0 >= 0 and b.x1 <= fig.bbox.width and b.y0 >= 0 and b.y1 <= fig.bbox.height,
                'Text extends outside the canvas: ' + item.get_text())
    # Export an exact physical canvas; no implicit tight-bbox resizing.
    fig.savefig(output/'summary-card.svg', metadata={'Date': None})
    fig.savefig(output/'summary-card.png', dpi=180, metadata={'Software': 'Beyond Blossom summary-card.py'})
    plt.close(fig)
    return dict(canvas_inches=[18, 12.5], png_pixels=[3240, 2250],
                text_items_inside_canvas=len(texts), all_case_cells=len(data['cases']))


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--canonical', type=Path, default=here/'canonical.json')
    parser.add_argument('--audit', type=Path, default=here/'evidence/validation/main-matrix-audit.json')
    parser.add_argument('--output', type=Path, default=here)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    data = extract(args.canonical.resolve(), args.audit.resolve(), output)
    inp = output/'summary-card-input.json'
    inp.write_text(json.dumps(data, indent=2, ensure_ascii=False)+'\n')
    layout = render(data, output)
    receipt = dict(schema_version=1, complete=True, renderer_sha256=sha(Path(__file__)),
                   input_sha256=sha(inp), source_refs=data['source_refs'], layout=layout,
                   formula='speedup = official pooled mean latency / minimum candidate pooled mean latency',
                   aggregation='Equal-weight geometric mean over all 21 settings; descriptive, not significance',
                   metrics=data['metrics'], cases=data['cases'],
                   files={name: sha(output/name) for name in ('summary-card-input.json', 'summary-card.svg', 'summary-card.png')})
    (output/'summary-card.json').write_text(json.dumps(receipt, indent=2, ensure_ascii=False)+'\n')
    print(json.dumps({'complete': True, 'output': str(output), 'metrics': data['metrics'], 'layout': layout}, indent=2))


if __name__ == '__main__':
    main()
