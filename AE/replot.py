#!/usr/bin/env python3
"""Regenerate paper figures from bundled data or a completed, sealed AE export."""
from __future__ import annotations

import argparse
from decimal import Decimal
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PAPER_FIGURES = {'ablation', 'comparison', 'm4'}
TABLES = ('native.csv', 'external.csv', 'predecoder.csv', 'm4.csv',
          'selected-configurations.csv', 'performance-macros.tex',
          'predecoder_numbers.tex', 'macro-audit.json')


def need(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path):
    def invalid(value):
        raise ValueError('Non-finite JSON value: ' + value)
    return json.loads(Path(path).read_text(), parse_constant=invalid)


def reference(path):
    return {'path': str(Path(path).resolve()), 'sha256': digest(path)}


def completed_export(work):
    """Locate the saved export without invoking acquisition or changing its state."""
    work = Path(work).resolve()
    completion_path = work / 'complete.json'
    completion = read(completion_path)
    need(completion.get('complete') is True, 'The workflow is not complete')
    need(completion.get('export'), 'The workflow has no completed export; export its runs first')
    output = Path(completion['export']).resolve()
    need(output.is_relative_to(work) and output != work, 'Recorded export is outside its workflow')
    receipt = output / 'complete.json'
    bindings = [reference(completion_path)]
    expected = completion.get('export_receipt_sha256')
    if expected is None:
        state_path = work / 'state.json'
        state = read(state_path)
        attempts = state.get('stages', {}).get('figures', [])
        need(attempts and attempts[-1].get('status') == 'complete', 'No completed local export stage')
        last = attempts[-1]
        need(Path(last['output']).resolve() == output, 'Local export stage differs from workflow completion')
        expected = last['receipt_sha256']
        bindings.append(reference(state_path))
    need(digest(receipt) == expected, 'Workflow export receipt changed')
    return output / 'canonical.json', receipt, bindings


def sealed_input(input_path, seal_path=None):
    input_path = Path(input_path).resolve()
    if seal_path is None:
        seal_path = next((input_path.parent / name for name in
                          ('manifest.json', 'complete.json', 'export-complete.json')
                          if (input_path.parent / name).is_file()), None)
        need(seal_path is not None, 'A manifest.json or completed export receipt is required beside canonical.json')
    seal_path = Path(seal_path).resolve()
    seal = read(seal_path)
    need(isinstance(seal, dict) and isinstance(seal.get('files'), dict), 'Invalid canonical seal')
    if 'complete' in seal:
        need(seal['complete'] is True, 'The export receipt is incomplete')
    relative = input_path.relative_to(seal_path.parent).as_posix()
    need(seal['files'].get(relative) == digest(input_path), 'Canonical JSON does not match its seal')
    return input_path, seal_path, seal


def timing_rows(rows, methods, profile, exporter, *, external=False):
    need(isinstance(rows, list), 'Invalid canonical timing table')
    result = {}
    for row in rows:
        d, p = row['distance'], Decimal(str(row['error_rate']))
        need(type(d) is int and d >= 3 and d % 2 == 1 and p.is_finite() and 0 < p < Decimal('.5'),
             'Invalid workload coordinates')
        identity = (d, p, row['method'])
        need(identity not in result, 'Duplicate canonical timing row')
        need(row['method'] in set(methods) | (set() if external else {'unstructured'}), 'Unknown measured method')
        shots, repeats = row['shots'], row['repeats']
        need(type(shots) is int and shots > 0 and type(repeats) is int and repeats > 0, 'Invalid sample count')
        if profile == 'paper':
            need(shots in ((10000, 1000000) if external else (1000000,)) and repeats == 3,
                 'Paper timings require recorded million/ten-thousand inputs and three passes')
        elapsed = row['repeat_elapsed_ns']
        need(isinstance(elapsed, list) and len(elapsed) == repeats and
             all(type(value) is int and value > 0 for value in elapsed), 'Invalid integer timing operands')
        means = [value / shots / 1000 for value in elapsed]
        need(math.isfinite(row['mean_us']) and math.isclose(row['mean_us'], sum(elapsed) / (shots * repeats * 1000), rel_tol=1e-12),
             'Canonical mean differs from integer timings')
        need(len(row['repeat_mean_us']) == repeats and
             all(math.isclose(a, b, rel_tol=1e-12) for a, b in zip(row['repeat_mean_us'], means)), 'Canonical repeat means differ')
        if not external:
            need(math.isclose(row['median_batch_us'], statistics.median(means), rel_tol=1e-12), 'Canonical batch median differs')
        result[identity] = row
    exporter.complete_matrix(result, methods, profile)
    settings = {(d, p) for d, p, _ in result}
    need(settings == {(d, p) for d, _ in settings for _, p in settings}, 'Nonrectangular timing matrix')
    return result


def canonical_tables(data, exporter):
    need(isinstance(data, dict) and data.get('schema') == 1 and data.get('complete') is True and
         data.get('profile') in ('paper', 'smoke'), 'Expected completed schema-1 canonical measurements')
    groups = data['groups']
    need(isinstance(groups, list) and len(groups) == len(set(groups)) and set(groups) <= set(exporter.GROUPS) and
         bool(set(groups) & PAPER_FIGURES), 'No valid paper figure groups in this export')
    need(data['ablation_stages'] == exporter.STAGES and data['best_configuration_pool'] == exporter.CANDIDATES,
         'Canonical method roles differ from the renderer')
    # Display labels may change with the presentation; measured roles and values may not.
    need(all(isinstance(data[name], dict) for name in ('sources', 'placement', 'hardware', 'aggregation')) and data['sources'],
         'Canonical provenance or hardware metadata is missing')
    need(data['hardware']['structured_backend'] in ('scalar', 'avx2', 'avx512vl', 'avx512'), 'Invalid structured backend')
    native = timing_rows(data['native'], exporter.STAGES, data['profile'], exporter)
    external = timing_rows(data['external'], exporter.EXTERNAL, data['profile'], exporter, external=True) if 'comparison' in groups else {}
    if 'comparison_shot_policy' in data:
        need(data['comparison_shot_policy'] == exporter.classify_shot_counts(external.values()),
             'Canonical sample-count policy differs from measured rows')
    if external:
        need({(d, p) for d, p, _ in external} == {(d, p) for d, p, _ in native}, 'Native/external settings differ')
    m4 = timing_rows(data['m4'], exporter.STAGES, data['profile'], exporter) if 'm4' in groups else {}
    policies = data['predecoder']
    need(isinstance(policies, list), 'Invalid policy table')
    if 'predecoder' in groups:
        distances = set(exporter.DISTANCES) if data['profile'] == 'paper' else {r['distance'] for r in policies}
        need(policies and len(policies) == len(distances) * 3 and
             {(r['distance'], r['policy']) for r in policies} == {(d, p) for d in distances for p in (0, 1, 2)},
             'Incomplete or duplicate policy table')
        for row in policies:
            need(Decimal(str(row['error_rate'])) == Decimal('.003') and row['execution'] in ('serial', 'pipeline') and
                 isinstance(row['strategy_label'], str) and math.isfinite(row['mean_us']) and row['mean_us'] > 0 and
                 math.isfinite(row['mean_residual']) and row['mean_residual'] >= 0, 'Invalid policy measurement')
            if data['profile'] == 'paper':
                need(row['shots'] == 1000000 and row['repeats'] == 3, 'Paper policy timings require one million inputs and three passes')
    return set(groups) & PAPER_FIGURES, native, external, policies, m4


def replay(input_path, seal_path, output, bindings=()):
    import export_results as exporter
    input_path, seal_path, seal = sealed_input(input_path, seal_path)
    need(not output.exists(), 'Use a new output directory')
    need(output != input_path.parent and input_path.parent not in output.parents, 'Keep redraw output outside the source export')
    need(output != HERE and HERE not in output.parents, 'Keep redraw output outside AE')
    inputs = [reference(input_path), reference(seal_path), *bindings]
    renderer = reference(Path(exporter.__file__))
    renderers = exporter.renderer_sources()
    data = read(input_path)
    groups, native, external, policies, m4 = canonical_tables(data, exporter)
    copies = [(input_path, 'canonical.json')]
    for name in TABLES:
        if name in seal['files']:
            path = seal_path.parent / name
            need(digest(path) == seal['files'][name], 'Sealed table changed: ' + name)
            copies.append((path, name)); inputs.append(reference(path))
    output.mkdir(parents=True)
    for source, name in copies:
        shutil.copyfile(source, output / name)
    figures = exporter.render_figures(output, groups, native, external, policies, m4, data['profile'])
    for item in [*inputs, *renderers]:
        need(digest(item['path']) == item['sha256'], 'Input or renderer changed during replay')
    files = {name: digest(output / name) for _, name in copies}
    for figure in figures:
        path = Path(figure['path']).resolve()
        need(path.is_relative_to(output), 'Renderer artifact is outside its output directory')
        need(digest(path) == figure['sha256'], 'Renderer output changed')
        files[str(path.relative_to(output))] = figure['sha256']
    receipt = dict(schema=1, complete=True, mode='canonical-replay', profile=data['profile'],
                   groups=sorted(groups), input=reference(input_path), seal=reference(seal_path),
                   comparison_shot_policy=exporter.classify_shot_counts(external.values()),
                   workflow_bindings=list(bindings), exporter=renderer, renderers=renderers,
                   recorded_source_exporter=seal.get('exporter') or {
                       name: seal[name] for name in ('original_exporter_sha256', 'exporter_sha256') if name in seal},
                   files=files,
                   scope='Offline rendering of sealed canonical measurements with the current renderer; no decoding, raw-run revalidation, or new measurements. Tables and macros are copied unchanged.')
    (output / 'complete.json').write_text(json.dumps(receipt, indent=2, allow_nan=False) + '\n')
    print(output / 'complete.json')


def historical_inputs(figures):
    """Snapshot the small bundled figure and macro inputs, with direct-data seals."""
    paths = {HERE / name for name in ('paper_num_mapping.json', 'paper_num_template.tex',
             'predecoder_macro_source.json', 'predecoder_numbers.tex', 'data/predecode.csv')}
    # These are compact metadata bundles, not detector inputs or per-shot arrays.
    for name in ('data/latest-matrix/run', 'data/pmu', 'data/tailreset'):
        paths.update(path for path in (HERE / name).rglob('*') if path.is_file())
    expected = {}
    if 'ablation' in figures:
        root = HERE / 'data/latest-matrix'
        manifest = root / 'SHA256SUMS'
        paths.add(manifest)
        sums = dict((name, sha) for sha, name in (line.split(maxsplit=1)
                    for line in manifest.read_text().splitlines() if line.strip()))
        for name in ('SOURCE.json', 'data/ablation.json', 'reference/plot-data.csv', 'reference/fastest-cells.csv'):
            relative = 'heatmaps/latest-six/' + name
            path = root / relative
            expected[path] = (sums[relative], manifest)
            paths.add(path)
    if 'm4' in figures:
        provenance = HERE / 'm4_pro/source/provenance.json'
        path = HERE / 'm4_pro/data/feishu_m4_six_roles.csv'
        expected[path] = (read(provenance)['csv_sha256'], provenance)
        paths.update((path, provenance, HERE / 'm4_pro/palette.json'))
    if 'comparison' in figures:
        manifest = HERE / 'data/paper-comparison.json'
        path = HERE / 'data/paper-comparison.csv'
        expected[path] = (read(manifest)['csv_sha256'], manifest)
        paths.update((path, manifest))
    inputs = []
    for path in sorted(paths):
        item = reference(path)
        if path in expected:
            sha, manifest = expected[path]
            need(item['sha256'] == sha, 'Archived figure input checksum mismatch: ' + str(path))
            item.update(expected_sha256=sha, seal=reference(manifest))
        inputs.append(item)
    return inputs


def historical(output, figures=('ablation', 'comparison', 'm4')):
    need(not output.exists(), 'Use a new output directory')
    need(not (HERE == output or HERE in output.parents and output.parts[len(HERE.parts)] in {'data', 'reference', 'm4_pro'}),
         'Output must not replace bundled evidence')
    if 'comparison' in figures:
        need((HERE / 'data/paper-comparison.csv').is_file(),
             'The original paper comparison dataset is not bundled. The older 1000-shot CSV is a different experiment. Use --input AE/data/reproduction/canonical.json to redraw the completed reproduction, or select --paper-figures ablation m4 for the verified original figures.')
    import export_results as exporter
    inputs = historical_inputs(figures)
    renderers = exporter.renderer_sources() + [reference(HERE / name) for name in (
        'replot.py', 'common.py', 'pmu_data.py', 'pmu_runtime.py', 'tailreset_data.py',
        'migrate_paper_numbers.py', 'generate_predecoder_macros.py')]
    font_manifest = read(HERE / 'fonts/manifest.json')
    for name, sha in font_manifest['files'].items():
        item = reference(HERE / 'fonts' / name)
        need(item['sha256'] == sha, 'Paper font checksum mismatch: ' + name)
        renderers.append(item)
    output.mkdir(parents=True)
    os.environ['MPLCONFIGDIR'] = str(output / '.matplotlib')
    commands = [
        ['pmu_data.py', 'verify'], ['tailreset_data.py', 'verify'],
        ['migrate_paper_numbers.py', '--check'], ['generate_predecoder_macros.py', '--check'],
        ['migrate_paper_numbers.py', '--output', str(output / 'performance-macros.tex')],
        ['generate_predecoder_macros.py', '--output', str(output / 'predecoder_numbers.tex')],
    ]
    for command in commands:
        subprocess.run([sys.executable, str(HERE / command[0]), *command[1:]], check=True)
    target = output / 'figures'
    if 'ablation' in figures:
        import ablation
        ablation.render(target, data_dir=HERE / 'data/latest-matrix/heatmaps/latest-six',
                        stem='incremental_ablation_1107_v1')
    if 'm4' in figures:
        subprocess.run([sys.executable, str(HERE / 'm4_pro/plot_m4_per_round.py'),
                        '--output-dir', str(target)], check=True)
    if 'comparison' in figures:
        import csv
        import decoders
        source = HERE / 'data/paper-comparison.csv'
        seal = read(HERE / 'data/paper-comparison.json')
        need(digest(source) == seal['csv_sha256'], 'Paper comparison input checksum mismatch')
        points = {}
        with source.open(newline='') as stream:
            for row in csv.DictReader(stream):
                key = (int(row['distance']), Decimal(row['error_rate']), row['method'])
                need(key not in points, 'Duplicate paper comparison point')
                points[key] = dict(mean_us=float(row['mean_us']), shots=int(row['shots']))
        decoders.render_paper(target, {}, {}, paper_points=points)
    for item in [*inputs, *renderers]:
        need(digest(item['path']) == item['sha256'], 'Input or renderer changed during historical replay')
    files = {path.relative_to(output).as_posix(): digest(path) for path in sorted(output.rglob('*'))
             if path.is_file() and '.matplotlib' not in path.parts}
    (output / 'complete.json').write_text(json.dumps(dict(schema=1, complete=True, mode='historical-replay',
        scope='Verified original manuscript figures; no decoder execution', figures=list(figures),
        commands=commands, inputs=inputs, renderers=renderers, files=files), indent=2, allow_nan=False) + '\n')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--input', type=Path, help='Replay a canonical.json using its adjacent manifest or export receipt')
    source.add_argument('--run', type=Path, help='Replay the saved export of a completed local or Slurm workflow')
    parser.add_argument('--manifest', type=Path, help='Explicit seal for --input; otherwise discovered beside canonical.json')
    parser.add_argument('--paper-figures', nargs='+', choices=('ablation', 'comparison', 'm4'),
                        help='Select original manuscript figures (default: all three); incompatible with --run/--input')
    parser.add_argument('--output', type=Path, required=True, help='New output directory; original measurements are never overwritten')
    args = parser.parse_args(argv)
    try:
        need(args.manifest is None or args.input is not None, '--manifest requires --input')
        need(not args.paper_figures or not (args.run or args.input), '--paper-figures cannot be combined with --run/--input')
        output = args.output.resolve()
        if args.run:
            input_path, seal_path, bindings = completed_export(args.run)
            replay(input_path, seal_path, output, bindings)
        elif args.input:
            replay(args.input, args.manifest, output)
        else:
            historical(output, args.paper_figures or ('ablation', 'comparison', 'm4'))
        return 0
    except (OSError, ValueError, KeyError, TypeError, ArithmeticError, ImportError, subprocess.CalledProcessError) as error:
        print('Replot failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
