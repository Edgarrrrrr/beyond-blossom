"""Package a completed matrix for offline paper numbers and AE heatmaps.

Original metadata stays byte-exact under originals/. Portable receipts are
explicit derived copies with relative references; no raw measurement is changed.
"""
import argparse
import csv
import json
from pathlib import Path
import shutil

import migrate_paper_numbers as M

NAMES = ['PyMatching', '+ SIMD-Enabled Structured Grid', '+ Degree-ordered predecoder',
         '+ Component', '+ TailReset', '+ Spatial']


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n')


def ref(path, base):
    return dict(path=path.relative_to(base).as_posix(), sha256=M.digest(path))


def bundle(run, analysis, output, mapping):
    config = M.read_json(mapping)
    matrix = M.Matrix(config, mapping.parent, str(run), str(analysis))
    analysis, run = matrix.analysis, matrix.run
    M.need(not output.exists(), '数据包必须写入新目录')
    # Validate the hash-linked completed analysis and each round before packaging.
    completion = json.loads((analysis / 'complete.json').read_text())
    summary = json.loads((analysis / 'summary.json').read_text())
    for key in ['measurements', 'rounds', 'report']:
        M.checked_reference(completion[key], analysis)
    for plot in completion['plots']:
        M.checked_reference(plot, analysis)
    for case in summary['cases']:
        for method in case['methods'].values():
            rounds = method['rounds']['batch']
            M.need(len(rounds) == 3 and {r['repeat'] for r in rounds} == {1, 2, 3}, '缺少batch轮次')
            M.need(all(r['shots'] == 1_000_000 for r in rounds), '不是百万shots')
            mean = sum(r['elapsed_ns'] for r in rounds) / sum(r['shots'] for r in rounds) / 1000
            M.need(mean == method['batch3_us'], '汇总与原始三轮整数耗时不符')
    output.mkdir(parents=True)
    original = output / 'originals'
    original.mkdir()
    sources = {}

    def preserve(path, name):
        target = original / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
        sources[name] = dict(original=M.reference(path), bundled=ref(target, output))

    for name in ['summary.json', 'complete.json', 'measurements.csv', 'rounds.csv', 'REPORT.md']:
        preserve(analysis / name, name)
    portable = output / 'run'
    portable_analysis = portable / 'analysis-batch'
    portable_analysis.mkdir(parents=True)
    shutil.copy2(run / 'plan.json', portable / 'plan.json')
    sources['plan.json'] = dict(original=M.reference(run / 'plan.json'), bundled=ref(portable / 'plan.json', output))
    plan_ref = dict(path='../plan.json', sha256=M.digest(portable / 'plan.json'))
    lanes = []
    for lane in summary['lanes']:
        node = lane['node']
        complete_path = M.checked_reference(lane['complete'], analysis)
        exit_path = M.checked_reference(lane['exit'], analysis)
        preserve(complete_path, f'lanes/{node}/complete.json')
        preserve(exit_path, exit_path.name)
        done = json.loads(complete_path.read_text())
        done['plan'] = plan_ref
        done['portable_copy_of'] = sources[f'lanes/{node}/complete.json']['bundled']
        target = portable / 'lanes' / node / 'complete.json'
        write(target, done)
        exit_target = portable / exit_path.name
        shutil.copy2(exit_path, exit_target)
        lanes.append(dict(node=node,
            complete=dict(path=f'../lanes/{node}/complete.json', sha256=M.digest(target)),
            exit=dict(path='../' + exit_target.name, sha256=M.digest(exit_target))))
    if matrix.provenance['analysis_exit']:
        preserve(Path(matrix.provenance['analysis_exit']['path']), 'analysis-exit.txt')
    compact = {k: summary[k] for k in ['complete', 'experiment_id', 'implementation_commit', 'official_commit',
                                      'lto', 'formal_calls', 'input_records', 'minimum_warmup_seconds']}
    compact.update(plan=plan_ref, lanes=lanes, available_metrics=['batch3_us'], cases=[])
    compact['verification_scope'] = ('Portable Batch3 view of the completed full analysis. Original metadata is bundled '
        'byte-exact. Raw shot/block arrays, input files, binaries and distribution NPZs remain external; '
        'offline macro/figure reproduction does not repeat acquisition or correctness validation.')
    for case in summary['cases']:
        compact['cases'].append(dict(id=case['id'], d=case['d'], p=case['p'], methods={
            name: dict(batch3_us=method['batch3_us'], rounds=dict(batch=method['rounds']['batch']))
            for name, method in case['methods'].items()}))
    write(portable_analysis / 'summary.json', compact)
    write(portable_analysis / 'complete.json', dict(complete=True, plan=plan_ref,
        summary=dict(path='summary.json', sha256=M.digest(portable_analysis / 'summary.json')),
        validated_calls=completion['validated_calls'], exact_differences=completion['exact_differences'],
        minimum_warmup_seconds=completion['minimum_warmup_seconds'], scope=compact['verification_scope'],
        original_completion=dict(path='../../originals/complete.json', sha256=M.digest(original / 'complete.json'))))
    methods = ['GridSIMD', 'Predecode', 'Component', 'TailReset', 'Spatial']
    for kind, baseline in [('latest-six', 'Upstream'), ('official-reference', 'Official')]:
        kit = output / 'heatmaps' / kind
        inputs = dict(source_kind='Completed document-protocol full matrix, all LTO OFF', build_lto='OFF',
            implementation_commit=summary['implementation_commit'], distances=[9, 13, 17, 21, 25, 29, 33],
            methods=[baseline, *methods], baseline=baseline, metric='batch3_us', panels=[])
        rows = []
        for probability in [.0005, .001, .003]:
            cases = sorted((c for c in summary['cases'] if c['p'] == probability), key=lambda c: c['d'])
            times = [[c['methods'][name]['batch3_us'] for name in inputs['methods']] for c in cases]
            speedups = [[t[0] / v for v in t] for t in times]
            inputs['panels'].append(dict(p=probability, time_us=times, speedups=speedups))
            for case, time, speedup in zip(cases, times, speedups, strict=True):
                winner = min(range(6), key=time.__getitem__)
                for j, name in enumerate(inputs['methods']):
                    rows.append(dict(p=probability, d=case['d'], source_method=name, display_name=NAMES[j],
                        speedup=f'{speedup[j]:.2f}', batch_us_per_shot=f'{time[j]:.3f}', fastest=j == winner))
        write(kit / 'data/ablation.json', inputs)
        for name, selected in [('plot-data.csv', rows), ('fastest-cells.csv', [r for r in rows if r['fastest']])]:
            target = kit / 'reference' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(selected)
        write(kit / 'SOURCE.json', dict(experiment_id=summary['experiment_id'], baseline=baseline,
            summary=dict(path='../../run/analysis-batch/summary.json', sha256=M.digest(portable_analysis / 'summary.json')),
            measurement='sum of three complete million-shot batch elapsed_ns / 3000000 / 1000, us/shot'))
    write(output / 'SOURCE.json', dict(complete=True, original_analysis=sources,
        packager=M.reference(__file__), notes=compact['verification_scope']))
    # Validate relative references through the public migration reader.
    M.Matrix(config, mapping.parent, str(portable), 'analysis-batch')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', required=True, type=Path)
    parser.add_argument('--analysis', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--mapping', type=Path, default=M.HERE / 'paper_num_mapping.json')
    args = parser.parse_args()
    bundle(args.run.resolve(), args.analysis, args.output.resolve(), args.mapping.resolve())


if __name__ == '__main__':
    main()
