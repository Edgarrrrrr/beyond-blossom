#!/usr/bin/env python3
"""Redraw the included figures from the hash-verified, repository-local export."""
from __future__ import annotations

import argparse
from decimal import Decimal
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
AE = HERE.parents[1]
FIGURE_GROUPS = {'ablation', 'comparison', 'predecoder', 'm4'}


def need(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def read(path):
    def invalid(value):
        raise ValueError('Non-finite JSON value: ' + value)
    return json.loads(Path(path).read_text(), parse_constant=invalid)


def positive(value):
    return type(value) in (int, float) and math.isfinite(value) and value > 0


def timing_rows(rows, methods, profile, exporter, *, external=False):
    result = {}
    need(isinstance(rows, list), 'Timing table must be a list')
    for row in rows:
        need(isinstance(row, dict), 'Invalid timing row')
        distance, probability = row['distance'], Decimal(str(row['error_rate']))
        need(type(distance) is int and distance >= 3 and distance % 2 == 1 and
             probability.is_finite() and 0 < probability < Decimal('.5'), 'Invalid workload coordinates')
        identity = (distance, probability, row['method'])
        need(identity not in result, 'Duplicate timing row: ' + str(identity))
        need(positive(row['mean_us']) and (external or positive(row['median_batch_us'])), 'Invalid timing value')
        need(type(row['shots']) is int and row['shots'] > 0 and
             type(row['repeats']) is int and row['repeats'] > 0, 'Invalid sample count')
        if profile == 'paper':
            need(row['shots'] in ((10000, 1000000) if external else (1000000,)) and row['repeats'] == 3,
                 'Paper timings require the recorded million/ten-thousand shots and three passes')
        result[identity] = row
    exporter.complete_matrix(result, methods, profile)
    settings = {(d, p) for d, p, _ in result}
    need(settings == {(d, p) for d, _ in settings for _, p in settings}, 'Timing settings are not a rectangular matrix')
    return result


def load(input_path, manifest_path):
    manifest = read(manifest_path)
    need(isinstance(manifest, dict) and manifest.get('schema') == 1 and isinstance(manifest.get('files'), dict), 'Expected schema-1 file manifest')
    relative = input_path.relative_to(manifest_path.parent).as_posix()
    need(manifest['files'].get(relative) == sha256(input_path), 'Canonical JSON hash does not match the manifest')
    exporter_path = AE / 'export_results.py'
    need(manifest.get('exporter_sha256') == sha256(exporter_path), 'Repository exporter hash differs from the archived renderer')
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(AE))
    spec = importlib.util.spec_from_file_location('bb_archived_exporter', exporter_path)
    exporter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(exporter)
    data = read(input_path)
    need(isinstance(data, dict) and data.get('schema') == 1 and data.get('complete') is True and
         data.get('profile') in ('paper', 'smoke'), 'Expected complete schema-1 canonical export with paper/smoke profile')
    groups = data['groups']
    need(isinstance(groups, list) and len(groups) == len(set(groups)) and
         set(groups) <= set(exporter.GROUPS) and bool(set(groups) & FIGURE_GROUPS), 'Invalid or empty included figure groups')
    need(data['ablation_stages'] == exporter.STAGES and data['best_configuration_pool'] == exporter.CANDIDATES and
         data['method_labels'] == exporter.LABELS, 'Archived method roles differ from the renderer')
    need(all(isinstance(data[name], dict) for name in ('sources', 'placement', 'hardware', 'aggregation')) and
         bool(data['sources']), 'Canonical export lacks provenance or hardware metadata')
    need(data['hardware']['structured_backend'] in ('scalar', 'avx2', 'avx512vl', 'avx512'), 'Invalid recorded structured backend')
    native = timing_rows(data['native'], exporter.STAGES, data['profile'], exporter)
    external = timing_rows(data['external'], exporter.EXTERNAL, data['profile'], exporter, external=True) if 'comparison' in groups else {}
    if external:
        need({(d, p) for d, p, _ in external} == {(d, p) for d, p, _ in native}, 'External/native matrices differ')
    m4 = timing_rows(data['m4'], exporter.STAGES, data['profile'], exporter) if 'm4' in groups else {}
    policies = data['predecoder']
    need(isinstance(policies, list), 'Invalid predecoder table')
    if 'predecoder' in groups:
        indices = [(row['distance'], row['policy']) for row in policies]
        distances = set(exporter.DISTANCES) if data['profile'] == 'paper' else {d for d, _ in indices}
        need(bool(indices) and len(indices) == len(set(indices)) and set(indices) == {(d, m) for d in distances for m in (0, 1, 2)},
             'Predecoder table is incomplete or duplicated')
        for row in policies:
            need(Decimal(str(row['error_rate'])) == Decimal('.003') and positive(row['mean_us']) and
                 type(row['mean_residual']) in (int, float) and math.isfinite(row['mean_residual']) and row['mean_residual'] >= 0,
                 'Invalid predecoder table values')
            need(row['execution'] in ('serial', 'pipeline') and isinstance(row['strategy_label'], str), 'Predecoder execution/label missing')
            if data['profile'] == 'paper':
                need(row['shots'] == 1000000 and row['repeats'] == 3, 'Paper policy timings require one million shots and three passes')
    return data, exporter, native, external, policies, m4


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=HERE / 'canonical.json')
    parser.add_argument('--manifest', type=Path, help='Default: manifest.json beside --input; exporter_sha256 is required')
    parser.add_argument('--output', type=Path, required=True, help='New directory for PDF/SVG/PNG figures and replot.json')
    args = parser.parse_args(argv)
    try:
        input_path = args.input.resolve()
        manifest_path = (args.manifest or input_path.parent / 'manifest.json').resolve()
        output = args.output.resolve()
        need(not output.exists(), 'Output already exists; choose a new directory')
        need(output != HERE and HERE not in output.parents, 'Keep redraw output outside the archived result directory')
        before = {'canonical': sha256(input_path), 'manifest': sha256(manifest_path), 'exporter': sha256(AE / 'export_results.py')}
        data, exporter, native, external, policies, m4 = load(input_path, manifest_path)
        output.mkdir(parents=True)
        figures = exporter.render_figures(output, set(data['groups']) & FIGURE_GROUPS, native, external, policies, m4, data['profile'])
        need(before == {'canonical': sha256(input_path), 'manifest': sha256(manifest_path), 'exporter': sha256(AE / 'export_results.py')},
             'Input, manifest or renderer changed during drawing')
        receipt = {'schema': 1, 'complete': True, 'profile': data['profile'], 'groups': data['groups'], 'sha256': before,
                   'files': {str(Path(item['path']).relative_to(output)): item['sha256'] for item in figures},
                   'scope': 'Offline figure redraw from sealed canonical values; no decoding or raw-run revalidation'}
        (output / 'replot.json').write_text(json.dumps(receipt, indent=2) + '\n')
        print(output / 'replot.json')
        return 0
    except (OSError, ValueError, KeyError, TypeError, ArithmeticError, ImportError) as error:
        print('Replot failed: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
