#!/usr/bin/env python3
"""Verify fresh AE runs and export the current article's figures, tables and macros.

Historical datasets are never used as a fallback. All output roles name the actual
current method; the fixed official reference is the sole speedup denominator.
"""
from __future__ import annotations

import argparse
import csv
from decimal import Decimal, ROUND_HALF_UP, localcontext
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import sys

import reproduce as R
import pmu_data as P

HERE = Path(__file__).resolve().parent
DISTANCES = [9, 13, 17, 21, 25, 29, 33]
RATES = [.0005, .001, .003]
CANDIDATES = ['structured', 'predecoded', 'component-parallel', 'pipelined', 'parallel-predecode']
STAGES = ['reference', *CANDIDATES]
EXTERNAL = ['parity', 'fusion', 'parity-uf', 'unionfind', 'ldpc-uf']
LABELS = {'reference': 'Official upstream reference', 'unstructured': 'Current unstructured',
          'structured': 'Structured', 'predecoded': 'Predecoded',
          'component-parallel': 'Component parallel', 'pipelined': 'Pipelined',
          'parallel-predecode': 'Parallel predecode', 'best': 'Beyond Blossom (best configuration)',
          'parity': 'Parity Blossom', 'fusion': 'Fusion Blossom', 'parity-uf': 'Parity UF',
          'unionfind': 'UnionFind C++', 'ldpc-uf': 'LDPC UF'}
ALIASES = {'pymatching': 'reference', 'structured-simd': 'structured', 'predecoding': 'predecoded',
           'component-parallel': 'component-parallel', 'tail-reset': 'pipelined',
           'parallel-predecoding': 'parallel-predecode', 'tail-reset-reference': 'component-parallel'}
POLICIES = {0: ('degree-ordered', 'Degree ordered'), 1: ('none', 'No predecoding'),
            2: ('coordinate', 'Coordinate ordered (full-cap)')}
GROUPS = ['ablation', 'comparison', 'predecoder', 'm4', 'macros', 'xeon-macros']


def need(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return json.loads(Path(path).read_text())


def reference(path):
    return dict(path=str(Path(path).resolve()), sha256=R.digest(path))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    def encode(value):
        if isinstance(value, Decimal):return str(value)
        raise TypeError(f'Unsupported exported type: {type(value)}')
    with path.open('x') as stream:
        json.dump(value, stream, indent=2, allow_nan=False, default=encode)
        stream.write('\n')


def write_csv(path, rows):
    need(bool(rows), f'Empty export table: {path}')
    with Path(path).open('x', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def key(d, p):
    return int(d), Decimal(str(p))


def integer(value, name, positive=False):
    need(type(value) is int and value >= int(positive), f'Invalid integer {name}')
    return value


def decimal_mean(row):
    with localcontext() as context:
        context.prec = 60
        return Decimal(sum(row['repeat_elapsed_ns']))/(row['shots']*row['repeats']*1000)


def hardware_context(native):
    """Report the measured platform without inferring the manuscript's hardware."""
    plan=native['plan']
    need(plan['backend'] in ('scalar','avx2','avx512vl','avx512'), 'Unsupported recorded structured backend')
    cases=native.get('case_placement') or {
        case:{field:plan.get(field) for field in ('host','platform','machine')}
        for case in native['inputs']}
    return dict(structured_backend=plan['backend'],cases=cases,
        manuscript_hardware_equivalence='not asserted',
        macro_namespace='Historical xeon keys are compatibility identifiers, not measured hardware labels')


def hardware_note(rows):
    platforms=sorted({str(row.get('platform') or 'unrecorded').split('-')[0] for row in rows.values()})
    machines=sorted({str(row.get('machine') or 'unrecorded') for row in rows.values()})
    backends=sorted({str(row.get('structured_backend') or 'unrecorded') for row in rows.values()})
    return ('Recorded platform: '+', '.join(platforms)+' / '+', '.join(machines)+
            '; structured backend: '+', '.join(backends)+'. Hardware equivalence to the manuscript is not asserted.')


def native_run(directory, measurement, profile='paper'):
    root = Path(directory).resolve()
    plan, done = read(root/'plan.json'), read(root/'complete.json')
    need(done.get('complete') and done.get('plan_sha256') == R.digest(root/'plan.json'), 'Incomplete/changed native plan')
    need(plan['measurement'] == measurement, f'Expected {measurement} run: {root}')
    if profile == 'paper':
        need(plan.get('profile') == 'paper' and plan['source']['commit'] and not plan['source']['status'], 'Paper export requires clean formal source identities')
        need(bool(plan.get('host')), 'Paper export requires recorded native host identity')
        need(plan['reference']['independent'] and (plan['reference'].get('build_record') or {}).get('upstream_revision') == 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd', 'Fixed independent reference receipt required')
        build = plan['binary'].get('build_record') or {}
        need(build.get('complete') and build.get('binary', {}).get('sha256') == plan['binary']['sha256'] and build.get('source') == plan['source'], 'Candidate build receipt missing or mismatched')
        need(plan['shots'] == 1000000, 'Current paper native runs require one million distinct inputs')
        if measurement == 'batch':
            need(plan['repeats'] == 3, 'Current paper batch aggregation requires three passes')
    cases = {(f'd{d:02d}-p{p:g}', int(d), float(p)) for p in plan['error_rates'] for d in plan['distances']}
    need(len(cases) == len(plan['distances'])*len(plan['error_rates']), 'Duplicate native cases')
    inputs = {}
    for item in done['inputs']:
        need(item['case'] not in inputs, 'Duplicate native input receipt')
        R.verify_relative_files(root, item['files'])
        dataset = f"inputs/{item['case']}/dataset.bin"
        need(dataset in item['files'], 'Sparse input identity missing')
        inputs[item['case']] = item['files'][dataset]
    need(set(inputs) == {c for c, _, _ in cases}, 'Native input matrix incomplete')
    expected = {(c,d,p,m,r) for c,d,p in cases for m in plan['methods'] for r in range(1,plan['repeats']+1)}
    actual, records = set(), []
    for call in done['calls']:
        identity = (call['case'], call['distance'], call['error_rate'], call['method'], call['repeat'])
        need(identity in expected and identity not in actual, 'Incomplete or duplicate native call matrix')
        actual.add(identity)
        R.verify_relative_files(root, call['files'])
        folder = (root/call['directory']).resolve()
        need(root in folder.parents, 'Native artifact path escapes run')
        metric_name = 'syndromes.json' if measurement == 'statistics' else 'pmu.json' if measurement == 'pmu' else 'measurement.csv'
        need(all(str(Path(call['directory'])/name) in call['files'] for name in ('correctness.json',metric_name)), 'Required native evidence is not sealed')
        correctness = read(folder/'correctness.json')
        need(correctness['status'] == 'passed' and correctness['weight_differences'] == 0, 'Native weight validation failed')
        need(correctness.get('shots') == plan['shots'], 'Native validation does not cover all saved shots')
        differences=integer(correctness.get('observable_differences'),'observable differences')
        need(integer(correctness.get('actual_candidate_witnesses'),'candidate witnesses') >= differences,
             'Observable ties lack actual candidate correction witnesses')
        row = dict(case=call['case'], distance=call['distance'], error_rate=call['error_rate'], method=call['method'], repeat=call['repeat'],
                   host=plan.get('host'), platform=plan.get('platform'), machine=plan.get('machine'),structured_backend=plan['backend'],
                   shots=plan['shots'], input_sha256=inputs[call['case']], validation=reference(folder/'correctness.json'),
                   source=reference(folder/('syndromes.json' if measurement == 'statistics' else 'pmu.json' if measurement == 'pmu' else 'measurement.csv')))
        if measurement == 'batch':
            with (folder/'measurement.csv').open(newline='') as stream:
                values = list(csv.DictReader(stream))
            need(len(values) == 1, 'Native measurement must contain one complete batch')
            value = values[0]
            need(int(value['shots']) == plan['shots'] and int(value['weight_diff']) == 0, 'Native batch count/weight differs')
            scope_path=folder/'measurement_scope.json'
            need(str(scope_path.relative_to(root)) in call['files'], 'Native timing scope is not sealed')
            scope=read(scope_path)
            execution=plan['methods'][call['method']]['execution']
            mode='pipe-batch' if execution=='pipeline' else 'batch'
            need(value['mode']==scope['mode']==mode and value['execution']==execution and
                 scope.get('internal_instrumentation') is False, 'Native batch timing scope/execution differs or is instrumented')
            row.update(elapsed_ns=int(value['elapsed_ns']), threads=int(value['total_threads']), execution=value['execution'],
                       scope=scope['timed_scope'],measurement_scope=scope)
            need(row['elapsed_ns'] > 0 and row['threads'] == plan['methods'][call['method']]['threads'], 'Invalid native timing/threads')
        elif measurement == 'statistics':
            value = read(folder/'syndromes.json')
            need(value['complete'] and value['timed'] is False and value['shots'] == plan['shots'], 'Incomplete or timed residual statistics')
            totals = {name: sum(integer(c[name], name) for c in value['components']) for name in ('normalized','residual','pairs','boundaries')}
            need(totals['normalized'] == totals['residual'] + 2*totals['pairs'] + totals['boundaries'], 'Residual conservation failed')
            need(integer(value['completed_shots'],'completed shots') <= plan['shots'], 'Invalid complete-predecode count')
            execution=plan['methods'][call['method']]['execution']
            need(value.get('execution', 'pipeline') == execution, 'Statistics execution differs from planned decoder')
            row.update(policy=value['mode'], execution=execution, threads=plan['methods'][call['method']]['threads'],
                       components=value['components'], completed_shots=value['completed_shots'], scope=value['scope'], **totals)
        else:
            value = read(folder/'pmu.json')
            levels = [level for level, spec in P.LEVELS.items() if value['events'] == spec['events']]
            need(len(levels) == 1, 'Unrecognized PMU event group')
            level = levels[0]
            loads, misses = P.counters(value, level, plan['shots'])
            row.update(level=level, requests=loads, misses=misses, events=value['events'], scope=value['scope'])
        records.append(row)
    need(actual == expected, 'Native call matrix is incomplete')
    return dict(kind=measurement, root=str(root), plan=plan, records=records, inputs=inputs,
                provenance=dict(plan=reference(root/'plan.json'), completion=reference(root/'complete.json')))


def aggregate(run):
    groups = {}
    for row in run['records']:
        groups.setdefault((*key(row['distance'], row['error_rate']), row['method']), []).append(row)
    result = {}
    for identity, rows in groups.items():
        rows.sort(key=lambda r:r['repeat'])
        n = sum(r['shots'] for r in rows)
        result[identity] = dict(distance=identity[0], error_rate=str(identity[1]), method=identity[2],
            shots=rows[0]['shots'], repeats=len(rows), repeat_elapsed_ns=[r['elapsed_ns'] for r in rows],
            repeat_mean_us=[r['elapsed_ns']/r['shots']/1000 for r in rows],
            mean_us=sum(r['elapsed_ns'] for r in rows)/n/1000,
            median_batch_us=statistics.median(r['elapsed_ns']/r['shots']/1000 for r in rows),
            input_sha256=rows[0]['input_sha256'], threads=rows[0]['threads'], execution=rows[0]['execution'], timing_scope=rows[0]['scope'],
            host=rows[0].get('host'),platform=rows[0].get('platform'),machine=rows[0].get('machine'),structured_backend=rows[0].get('structured_backend'),
            sources=[r['source'] for r in rows])
    return result


def complete_matrix(rows, methods, profile):
    settings = {(d,p) for d,p,m in rows}
    if profile == 'paper':
        need(settings == {key(d,p) for d in DISTANCES for p in RATES}, 'Paper figures require the full 21-setting matrix')
    need(bool(settings), 'Empty timing matrix')
    for setting in settings:
        need(all((*setting,m) in rows for m in methods), f'Missing measured configuration at {setting}')
    return sorted(settings)


def same_inputs(left, right, methods=None):
    need(left['plan']['source']['commit'] == right['plan']['source']['commit'], 'Results use different candidate source commits')
    for case, digest in right['inputs'].items():
        a=left.get('case_placement',{}).get(case,left['plan'])
        b=right.get('case_placement',{}).get(case,right['plan'])
        if a.get('host') or b.get('host'):
            need(a.get('host') == b.get('host'), f'Paired native results use different hosts for {case}')
        for field in ('platform','machine'):
            if a.get(field) and b.get(field):need(a[field]==b[field], f'Paired native {field} differs for {case}')
        need(left['inputs'].get(case) == digest, f'Different saved inputs for {case}')


def external_run(directory, native, profile):
    root = Path(directory).resolve()
    plan, done, summary = read(root/'plan.json'), read(root/'complete.json'), read(root/'summary.json')
    need(done.get('complete') and summary.get('complete'), 'External collection incomplete')
    need(done['plan_sha256'] == R.digest(root/'plan.json') and done['summary_sha256'] == R.digest(root/'summary.json') and done['summary_csv_sha256'] == R.digest(root/'summary.csv'), 'External matrix seal changed')
    need(plan['ae_plan_sha256'] == native['provenance']['plan']['sha256'], 'External methods did not consume this native run input plan')
    if profile == 'paper':
        need(plan['profile'] == 'paper' and not plan['source']['dirty'] and plan['source']['commit'] == native['plan']['source']['commit'], 'External paper source identity differs')
    result = {}
    for group in done['groups']:
        summary_path = (root/group['summary']).resolve()
        need(root in summary_path.parents and R.digest(summary_path) == group['sha256'], 'External group seal changed')
        value = read(summary_path)
        need(value.get('complete'), 'External method group is incomplete')
        folder = summary_path.parent
        configuration=value['configuration']
        if configuration.get('host'):
            need(configuration['host']==native['plan'].get('host'), 'External and native timings must identify the same host')
        if profile == 'paper':
            configuration = value['configuration']
            build_source=configuration['build']['source']
            need(configuration['profile']=='paper' and not build_source['dirty'] and
                 build_source['commit']==native['plan']['source']['commit'] and
                 value['oracle']['revision']=='e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd',
                 'External group build/profile/oracle identity differs')
            need(native['plan'].get('host') and configuration.get('host') == native['plan']['host'] and
                 configuration.get('platform') == native['plan']['platform'] and configuration.get('machine') == native['plan']['machine'],
                 'External and native timings must identify the same host and platform')
        provenance = value['configuration']['input_provenance']
        need(provenance['source']['sha256'] == native['inputs'][group['case']], 'External graph source differs from native saved inputs')
        prepared = folder.parent/'input'
        R.verify_relative_files(prepared, provenance['files'])
        need(R.digest(folder/'oracle.jsonl') == value['oracle']['sha256'], 'Independent external oracle changed')
        for item in value['results']:
            method = item['method']
            need(f'{method}.csv' in item['files'] and f'{method}.corrections.jsonl' in item['files'], 'Required external evidence is not sealed')
            R.verify_relative_files(folder, item['files'])
            need(method in EXTERNAL and method in group['methods'], 'Unexpected external method')
            need(item['shots']==group['shots']==provenance['selected_shots'] and item['repeats']==provenance['repeats'],
                 'External measured counts disagree with prepared input identity')
            d, p = provenance['source']['distance'], provenance['source']['error_rate']
            identity = (*key(d,p),method)
            need(identity not in result, 'Duplicate external setting/method')
            totals = [0]*item['repeats']
            with (folder/f'{method}.csv').open(newline='') as stream:
                rows = csv.DictReader(stream)
                measured = 0
                for index, row in enumerate(rows):
                    r, shot = divmod(index,item['shots'])
                    need(r < len(totals) and (int(row['repeat']),int(row['shot'])) == (r,shot), 'External row sequence differs')
                    elapsed = int(row['decode_ns'])
                    need(elapsed >= 0, 'Negative external time')
                    totals[r] += elapsed
                    measured += 1
            need(measured == item['shots']*item['repeats'] and measured > 0, 'External measurement count differs')
            means = [ns/item['shots']/1000 for ns in totals]
            need(math.isclose(sum(means)/len(means), item['mean_us_per_shot'], rel_tol=1e-12), 'External mean disagrees with integer timing')
            if profile == 'paper':
                need(item['shots'] in (10000,1000000) and item['repeats'] == 3, 'External paper counts differ')
            result[identity] = dict(distance=d,error_rate=str(p),method=method,shots=item['shots'],repeats=item['repeats'],
                host=configuration.get('host'),platform=configuration.get('platform'),machine=configuration.get('machine'),
                source_commit=(configuration.get('build') or {}).get('source',{}).get('commit'),
                repeat_elapsed_ns=totals,repeat_mean_us=means,mean_us=sum(means)/len(means),
                input_sha256=provenance['source']['sha256'], timing_scope=item['timing_scope'],
                physical_observable_truth_available=False, source=reference(summary_path),
                quantization='Original external even-integer graph, maximum q=20000')
    return dict(rows=result,provenance=dict(plan=reference(root/'plan.json'),completion=reference(root/'complete.json')))


def policy_results(timing, counts, profile):
    need(set(timing) == set(counts) == set(POLICIES), 'All three policy timing and statistics runs are required')
    rows = []
    timings = {mode:aggregate(run) for mode,run in timing.items()}
    stat = {mode:{key(r['distance'],r['error_rate']):r for r in run['records']} for mode,run in counts.items()}
    settings = set(stat[1])
    methods = {r['method'] for run in counts.values() for r in run['records']}
    need(len(methods) == 1 and methods <= {'predecoded','pipelined'}, 'Policies require one consistent serial or pipeline execution')
    method = next(iter(methods))
    execution, threads = ('serial',1) if method == 'predecoded' else ('pipeline',4)
    if profile == 'paper':
        need(settings == {key(d,.003) for d in DISTANCES}, 'Predecoder table requires all seven p=0.003 distances')
    for mode in POLICIES:
        same_inputs(timing[1],timing[mode]); same_inputs(timing[1],counts[mode])
        need(set(stat[mode]) == settings and len(stat[mode]) == len(counts[mode]['records']), 'Duplicate/missing residual statistics')
        configuration = (timing[mode]['plan']['binary'].get('build_record') or {}).get('configuration', {})
        physical_modes={row['policy'] for row in counts[mode]['records']}
        need(len(physical_modes)==1, 'Mixed physical predecoder policies in one table role')
        physical_mode=next(iter(physical_modes))
        need(physical_mode in ((2,3) if mode==2 else (mode,)), 'Physical predecoder policy differs from table role')
        strategy,label=('simple-equal-rate','Simple equal-rate') if physical_mode==3 else POLICIES[mode]
        if profile == 'paper':
            need(str(configuration.get('BB_TAILRESET_PREDECODE_MODE')) == str(physical_mode), 'Timing binary policy differs from requested table role')
        elif configuration.get('BB_TAILRESET_PREDECODE_MODE') is not None:
            need(str(configuration['BB_TAILRESET_PREDECODE_MODE']) == str(physical_mode), 'Timing binary policy differs from counts')
        for setting in sorted(settings):
            need((*setting,method) in timings[mode], 'Table timing requires the same serial or pipeline execution as counts')
            t, s, initial = timings[mode][(*setting,method)], stat[mode][setting], stat[1][setting]
            need(t['threads'] == threads and t['execution'] == execution and s['execution'] == execution
                 and s['method'] == method and s['policy'] == physical_mode, 'Predecoder execution or mode differs')
            need(s['normalized'] == initial['normalized'] and initial['residual'] == initial['normalized'], 'Predecoder policies do not share normalized inputs')
            need(t['shots'] == s['shots'] and s['normalized'] > 0, 'Table timing/count inputs differ or no initial defects')
            rows.append(dict(distance=setting[0],error_rate=str(setting[1]),policy=mode,physical_policy=physical_mode,
                strategy=strategy,strategy_label=label,macro_strategy=POLICIES[mode][0],
                host=t.get('host'),platform=t.get('platform'),machine=t.get('machine'),structured_backend=t.get('structured_backend'),
                shots=s['shots'],repeats=t['repeats'],threads=threads,execution=execution,method=method,mean_us=t['mean_us'],
                repeat_elapsed_ns=t['repeat_elapsed_ns'], normalized=s['normalized'],residual=s['residual'],
                pairs=s['pairs'],boundaries=s['boundaries'],completed_shots=s['completed_shots'],
                mean_residual=s['residual']/s['shots'],matched_percent=100*(s['normalized']-s['residual'])/s['normalized'],
                timing_scope=t['timing_scope'],count_scope=s['scope'],input_sha256=s['input_sha256'],
                timing_sources=t['sources'],statistics_source=s['source']))
    return rows


def predecoder_macros(policies):
    need(len(policies) == 21 and {(r['distance'],r['policy']) for r in policies} ==
         {(d,m) for d in DISTANCES for m in POLICIES}, 'All predecoder rows required for macros')
    executions={(row['execution'],row['threads']) for row in policies}
    need(len(executions)==1, 'Mixed predecoder execution cannot populate one table')
    execution,threads=next(iter(executions))
    third={row['strategy'] for row in policies if row['policy']==2}
    need(len(third)==1, 'Mixed simple/coordinate policies cannot populate one table')
    pre = [f'% Fresh {execution} table, {threads} thread(s); third policy={next(iter(third))}; legacy coordinate macro namespace retained; independent untimed residual replay.']
    rates = []
    with localcontext() as ctx:
        ctx.prec = 60
        for row in policies:
            prefix = f"xeon.predecoder-comparison.d{row['distance']}.p0p3.{row.get('macro_strategy',row['strategy'])}."
            entries = {'mean-time-us':decimal_mean(row),
                       'mean-remaining-syndromes':Decimal(row['residual'])/row['shots']}
            if row['policy'] != 1:
                entries['matched-percent'] = Decimal(100)*(row['normalized']-row['residual'])/row['normalized']
            if row['policy'] == 0:rates.append(entries['matched-percent'])
            for suffix, value in entries.items():
                pre.append('\\DefinePerfValue{'+prefix+suffix+'}{'+format(value.quantize(Decimal('.01'),rounding=ROUND_HALF_UP),'.2f')+'}')
        for label, value in [('min',min(rates)),('max',max(rates))]:
            pre.append('\\DefinePerfValue{xeon.predecoder-comparison.p0p3.degree-ordered.matched-percent.'+label+'}{'+format(value.quantize(Decimal('.01'),rounding=ROUND_HALF_UP),'.2f')+'}')
    need(len(pre) == 59, 'Predecoder macro coverage incomplete')
    return '\n'.join(pre)+'\n'


def paper_macros(native, pmu, policies, m4, *, xeon_only=False):
    """Reuse published numeric rules with explicit current roles and new sources."""
    import migrate_paper_numbers as M
    rows, m4rows = aggregate(native), aggregate(m4) if m4 else {}
    full_native={(d,p) for d,p,m in rows} == {key(d,p) for d in DISTANCES for p in RATES}
    if not xeon_only or full_native:complete_matrix(rows,STAGES,'paper')
    if not xeon_only:complete_matrix(m4rows,STAGES,'paper')
    config = read(HERE/'paper_num_mapping.json')
    config['aliases'] = ALIASES
    matrix = object.__new__(M.Matrix)
    matrix.rows = [dict(id=f'd{d:02d}-p{p}',d=d,p=str(p),rounds=d,
        methods={m:dict(batch3_us=str(decimal_mean(rows[d,p,m]))) for m in STAGES})
        for d,p in sorted({(d,p) for d,p,m in rows})]
    matrix.metric, matrix.aliases, matrix.config = 'batch3_us', ALIASES, config
    counts = {}
    for level, run in pmu.items():
        same_inputs(native,run)
        for row in run['records']:
            need(row['level'] == level, 'Wrong PMU level supplied')
            identity = (*key(row['distance'],row['error_rate']),row['method'],level)
            old = counts.setdefault(identity,[0,0])
            old[0] += row['requests']; old[1] += row['misses']
    values, audit = {}, {}
    with localcontext() as ctx:
        ctx.prec = 60
        for name, rule in config['rules'].items():
            # A smoke prefix cannot stand in for a paper-wide geomean, maximum,
            # crossover count, or fixed workload. Keep all missing values undefined.
            if xeon_only and not full_native:continue
            if rule['op'] == 'pmu_miss_percent':
                identity = (*key(rule['d'],rule['p']),ALIASES[rule['method']],rule['level'])
                if xeon_only and identity not in counts:continue
                need(identity in counts,'Missing PMU input for '+name)
                requests, misses = counts[identity]
                value, operands = Decimal(100)*misses/requests,dict(requests=requests,misses=misses,method=identity[2],level=identity[3])
            else:
                value, operands = matrix.calculate(rule)
            values[name] = value
            audit[name] = dict(value=str(value),rule=rule,operands=operands)
        for p, token in ([] if xeon_only else [(.0005,'p0p05'),(.001,'p0p1'),(.003,'p0p3')]):
            q = Decimal(str(p))
            def median_us(row):return Decimal(statistics.median(row['repeat_elapsed_ns']))/(row['shots']*1000)
            speedups = [median_us(m4rows[d,q,'reference'])/min(median_us(m4rows[d,q,m]) for m in CANDIDATES) for d in DISTANCES]
            macro=f'm4pro.decoder-comparison.{token}.best.vs-pymatching.max-speedup'
            values[macro] = max(speedups)
            audit[macro] = dict(value=str(values[macro]),aggregation='median of three complete-batch means',
                               distances=DISTANCES,speedups=speedups,candidates=CANDIDATES,reference='reference')
        for name, methods in ([] if xeon_only else [('beyond-blossom',CANDIDATES),('pymatching',['reference'])]):
            macro=f'm4pro.realtime.{name}.num-supported-settings'
            values[macro] = Decimal(sum(
                min(median_us(m4rows[d,Decimal(str(p)),m]) for m in methods)/d <= 1
                for d in DISTANCES for p in RATES))
            audit[macro] = dict(value=str(values[macro]),formula='count(median block time / d <= 1 us)',methods=methods,
                               settings=[dict(distance=d,error_rate=p,us_per_round=min(median_us(m4rows[d,Decimal(str(p)),m]) for m in methods)/d) for d in DISTANCES for p in RATES])
        template = (HERE/'paper_num_template.tex').read_text()
        items = M.declarations(template)
        omitted=sorted({item.key for item in items}-set(values))
        if not xeon_only:need(not omitted, 'Performance macro coverage incomplete')
        for item in reversed(items):
            if item.key not in values:continue
            places = len(item.value.partition('.')[2])
            formatted = format(values[item.key].quantize(Decimal(1).scaleb(-places),rounding=ROUND_HALF_UP),'f')
            template = template[:item.start]+formatted+template[item.end:]
        if xeon_only:
            template='\n'.join(line for line in template.splitlines() if not any(line.startswith('\\DefinePerfValue{'+key+'}{') for key in omitted))+'\n'
            template='% Linux-result subset (legacy xeon keys): unavailable PMU/M4 macros are intentionally undefined; no archived values.\n'+template
        template='% '+hardware_note(rows)+'\n% Legacy xeon keys do not identify the measurement hardware.\n'+template
    return template, predecoder_macros(policies) if policies and (not xeon_only or len(policies)==21) else '', dict(performance_values=values,performance_operands=audit,predecoder_rows=policies,
        scope='xeon-only subset' if xeon_only else 'full paper',omitted_performance_keys=omitted,
        native_matrix_complete=full_native,placement=native.get('case_placement'),hardware=hardware_context(native),
        denominator='Fixed official reference; historical pymatching macro key retained for manuscript compatibility',rounding='Decimal precision 60, final ROUND_HALF_UP')


def render_figures(output, groups, native, external, policies, m4, profile):
    """Render current article data with adaptive axes and explicit method labels."""
    os.environ.setdefault('MPLCONFIGDIR', str(output/'.matplotlib'))
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
    plt.rcParams.update({'font.family':'DejaVu Sans','font.size':10,'pdf.fonttype':42,'svg.fonttype':'none'})
    figures = output/'figures'
    figures.mkdir()
    exported = []
    def save(fig, name):
        if profile == 'smoke':
            fig.text(.5,1.03,'Development smoke: not paper performance evidence',ha='center',va='bottom',fontsize=9,color='#9d2600')
        pool=m4 if name=='m4_pro_per_round' else native
        fig.text(.5,-.025,hardware_note(pool),ha='center',va='top',fontsize=7)
        hosts=sorted({row['host'] for row in pool.values() if row.get('host')})
        if len(hosts)>1:
            fig.text(.5,-.065,'Each setting is paired on one host; collection spans '+', '.join(hosts)+'. See canonical.json.',
                     ha='center',va='top',fontsize=7)
        for extension in ('pdf','svg','png'):
            path = figures/f'{name}.{extension}'
            fig.savefig(path, dpi=300, bbox_inches='tight')
            exported.append(reference(path))
        plt.close(fig)
    settings = sorted({(d,p) for d,p,m in native})
    distances, rates = sorted({d for d,p in settings}), sorted({p for d,p in settings})
    colors = ['#325A7E','#636A73','#BF7A49','#7E64A0','#8B7535','#D1A17B','#A18BBB']
    if 'ablation' in groups:
        fig, axes = plt.subplots(1,len(rates),figsize=(max(6,6*len(rates)),max(3,0.55*len(distances)+2)),squeeze=False,constrained_layout=True)
        top = max(native[d,p,'reference']['mean_us']/native[d,p,m]['mean_us'] for d,p in settings for m in STAGES)
        for ax,p in zip(axes[0],rates):
            values = np.array([[native[d,p,'reference']['mean_us']/native[d,p,m]['mean_us'] for m in STAGES] for d in distances])
            im=ax.imshow(values,cmap='Blues',vmin=0,vmax=max(1,top),aspect='auto')
            ax.set_xticks(range(len(STAGES)),[LABELS[m] for m in STAGES],rotation=40,ha='right')
            ax.set_yticks(range(len(distances)),distances); ax.set_ylabel('Code distance')
            ax.set_title(f'p = {float(p)*100:g}%')
            for i,d in enumerate(distances):
                winner=min(STAGES,key=lambda m:native[d,p,m]['mean_us'])
                for j,m in enumerate(STAGES):
                    ax.text(j,i,f"{values[i,j]:.2f}×\n{native[d,p,m]['mean_us']:.3f} μs",ha='center',va='center',
                            fontsize=8,color='white' if values[i,j]>top*.6 else 'black',fontweight='bold' if m==winner else 'normal')
        fig.colorbar(im,ax=list(axes[0]),label='Speedup over fixed official reference',orientation='horizontal',shrink=.6)
        save(fig,'incremental_ablation_1107_v1')
    if 'comparison' in groups:
        fig,axes=plt.subplots(1,len(rates),figsize=(max(5,4*len(rates)),4.6),squeeze=False)
        fig.subplots_adjust(left=.08,right=.985,bottom=.19,top=.72,wspace=.20)
        series=['best','reference',*EXTERNAL]
        for ax,p in zip(axes[0],rates):
            for index,method in enumerate(series):
                selected=[native[d,p,min(CANDIDATES,key=lambda m:native[d,p,m]['mean_us'])] if method=='best'
                          else native[d,p,method] if method=='reference' else external[d,p,method] for d in distances]
                times=[row['mean_us'] for row in selected]
                label={'best':'Beyond Blossom','reference':'Official reference'}.get(method,LABELS[method])
                ax.plot(distances,times,label=label,color=colors[index],linewidth=1.3)
                for d,time,row in zip(distances,times,selected):
                    ax.plot(d,time,'o',color=colors[index],markerfacecolor=colors[index] if row['shots']==1000000 else 'white',markersize=4)
            ax.set_yscale('log'); ax.set_xticks(distances); ax.set_xlabel('Code distance')
            ax.set_title(f'p = {float(p)*100:g}%'); ax.grid(axis='y',alpha=.2)
        axes[0,0].set_ylabel('Mean native time (μs/shot)')
        handles,labels=axes[0,0].get_legend_handles_labels()
        fig.legend(handles,labels,loc='upper center',bbox_to_anchor=(.5,.99),ncol=4 if len(rates)>1 else 2,frameon=False,fontsize=9)
        fig.supxlabel('Filled: 1,000,000 distinct inputs; open: recorded smaller prefix. Native timing scopes differ.',fontsize=8)
        save(fig,'decoder_comparison_shows_speedup_v3')
    if 'predecoder' in groups:
        policy_distances=sorted({r['distance'] for r in policies})
        by={(r['distance'],r['policy']):r for r in policies}
        for metric,name,ylabel in [('mean_us','decoding_time','Mean complete decode time (μs/shot)'),('mean_residual','remaining_syndromes','Mean unmatched defects per shot')]:
            fig,ax=plt.subplots(figsize=(8.5,4.5))
            fig.subplots_adjust(left=.10,right=.985,bottom=.14,top=.76)
            x=np.arange(len(policy_distances))
            for i,mode in enumerate((1,2,0)):
                values=[by[d,mode][metric] for d in policy_distances]
                bars=ax.bar(x+(i-1)*.27,values,.25,label=by[policy_distances[0],mode]['strategy_label'],color=colors[i])
                ax.bar_label(bars,fmt='%.2f',padding=3,fontsize=8)
            ax.set_xticks(x,policy_distances); ax.set_xlabel('Code distance'); ax.set_ylabel(ylabel)
            ax.margins(y=.2); ax.set_title(('Serial' if policies[0]['execution']=='serial' else 'Four-thread pipeline')+', p = 0.3%')
            fig.legend(*ax.get_legend_handles_labels(),loc='upper center',bbox_to_anchor=(.54,.99),ncol=3,fontsize=9,frameon=False)
            save(fig,name)
    if 'm4' in groups:
        m4settings=sorted({(d,p) for d,p,m in m4}); ds=sorted({d for d,p in m4settings}); ps=sorted({p for d,p in m4settings})
        fig,ax=plt.subplots(figsize=(7,4.5))
        fig.subplots_adjust(left=.12,right=.985,bottom=.14,top=.73)
        for i,p in enumerate(ps):
            for best in (False,True):
                times=[min(m4[d,p,m]['median_batch_us'] for m in CANDIDATES)/d if best else m4[d,p,'reference']['median_batch_us']/d for d in ds]
                ax.plot(ds,times,marker='o',markersize=4,linestyle='-' if best else '--',color=colors[i],
                        markerfacecolor=colors[i] if best else 'white',label=f"{'Beyond Blossom' if best else 'Official reference'}, p={float(p)*100:g}%")
        ax.axhline(1,color='#777777',linestyle=':');ax.set_yscale('log');ax.set_xticks(ds)
        ax.set_xlabel('Code distance');ax.set_ylabel('Median block time / d (μs/round)')
        fig.legend(*ax.get_legend_handles_labels(),loc='upper center',bbox_to_anchor=(.54,.99),ncol=2,fontsize=8,frameon=False)
        save(fig,'m4_pro_per_round')
    return exported


def merge_native_runs(runs):
    """Combine disjoint case matrices without inventing a shared measurement host."""
    need(bool(runs),'Missing native shard collection')
    first=runs[0]
    variant_keys=('BB_TAILRESET_PREDECODE_MODE','BB_TAILRESET_SYNDROME_STATS','BB_AE_CACHE_LEVEL',
                  'BB_STRUCTURED_STORAGE_DIRECTIONS','BB_PREDECODE_SIMD','BB_PREDECODE_ENABLED',
                  'BB_STRUCTURED_PROFILE_COUNTERS','BB_STRUCTURED_PROFILE_TIMERS','BB_STRUCTURED_PROFILE_TIMELINE',
                  'CMAKE_INTERPROCEDURAL_OPTIMIZATION','CMAKE_CXX_FLAGS','CMAKE_CXX_FLAGS_RELEASE')
    def variant(run):
        config=(run['plan']['binary'].get('build_record') or {}).get('configuration',{})
        return {name:config.get(name) for name in variant_keys}
    expected_variant=variant(first)
    inputs,records,placements,provenance={},{},{},{}
    for index,run in enumerate(runs):
        need(run['kind']==first['kind'],'Mixed native measurement kinds')
        need(run['plan']['source']['commit']==first['plan']['source']['commit'],'Shards use different candidate source commits')
        need(run['plan']['backend']==first['plan']['backend'],'Shards use different structured SIMD backends')
        need(variant(run)==expected_variant,'Shards use different native build variants/configuration')
        for case,digest in run['inputs'].items():
            need(case not in inputs,f'Duplicate native shard case: {case}')
            need(bool(run['plan'].get('host')),f'Shard host identity missing for {case}')
            inputs[case]=digest
            placements[case]={field:run['plan'].get(field) for field in ('host','platform','machine')}
        for row in run['records']:
            identity=(row['case'],row['method'],row['repeat'])
            need(identity not in records,'Duplicate native shard measurement')
            records[identity]=row
        for name,item in run['provenance'].items():provenance[f'shard_{index:03d}_{name}']=item
    hosts=sorted({p['host'] for p in placements.values()})
    plan=dict(first['plan'],host=hosts[0] if len(hosts)==1 else None,hosts=hosts,
              collection_scope='per-setting paired multi-host' if len(hosts)>1 else 'single host')
    return dict(kind=first['kind'],plan=plan,records=list(records.values()),inputs=inputs,
                case_placement=placements,provenance=provenance)


def shard_runs(path, groups, profile):
    """Each bundle owns all comparisons for its cases; only case-disjoint bundles merge."""
    path=Path(path).resolve()
    manifest=read(path)
    need(manifest.get('schema')==1 and isinstance(manifest.get('shards'),list) and manifest['shards'],
         'Expected schema 1 and a nonempty shards list')
    resolve=lambda value:(path.parent/Path(value)).resolve()
    natives,external,policy_timing,policy_counts,cache=[],{}, {},{},{}
    sources={'shards_manifest':{'manifest':reference(path)}}
    allowed={'native_run','external_run','policy_timing','policy_counts','l2_run','l3_run'}
    for index,bundle in enumerate(manifest['shards']):
        need(isinstance(bundle,dict) and set(bundle)<=allowed and bundle.get('native_run'), 'Unknown or missing shard bundle field')
        native=native_run(resolve(bundle['native_run']),'batch',profile)
        complete_matrix(aggregate(native),STAGES,'smoke')
        natives.append(native)
        if 'comparison' in groups:
            need(bundle.get('external_run'),'Every comparison shard requires external_run')
            imported=external_run(resolve(bundle['external_run']),native,profile)
            need({key(r['distance'],r['error_rate']) for r in imported['rows'].values()} ==
                 {key(r['distance'],r['error_rate']) for r in native['records']}, 'External/native shard case matrices differ')
            for identity,row in imported['rows'].items():
                need(identity not in external,'Duplicate external shard setting/method')
                need(row.get('host')==native['plan'].get('host'),'External shard host identity missing or mismatched')
                need(row.get('source_commit')==native['plan']['source']['commit'],'External shard candidate source commit differs')
                external[identity]=row
            sources[f'shard_{index:03d}_external']=imported['provenance']
        if bundle.get('policy_timing') or bundle.get('policy_counts'):
            need(set(bundle.get('policy_timing',{}))==set(bundle.get('policy_counts',{}))=={'0','1','2'},
                 'Each policy shard requires timing and counts for all three roles')
            for label,measurement,collection in [('policy_timing','batch',policy_timing),('policy_counts','statistics',policy_counts)]:
                for mode,location in bundle[label].items():
                    run=native_run(resolve(location),measurement,profile)
                    same_inputs(native,run)
                    collection.setdefault(int(mode),[]).append(run)
        for level,label in [('L2','l2_run'),('L3','l3_run')]:
            if bundle.get(label):
                run=native_run(resolve(bundle[label]),'pmu',profile)
                same_inputs(native,run)
                cache.setdefault(level,[]).append(run)
    native=merge_native_runs(natives)
    sources['native']=native['provenance']
    need_policy=bool(groups & {'predecoder','macros'}) or bool(policy_timing or policy_counts)
    policies=[]
    if need_policy:
        timing={mode:merge_native_runs(runs) for mode,runs in policy_timing.items()}
        counts={mode:merge_native_runs(runs) for mode,runs in policy_counts.items()}
        policies=policy_results(timing,counts,profile)
        for mode in timing:
            sources[f'policy_{mode}_timing']=timing[mode]['provenance']
            sources[f'policy_{mode}_counts']=counts[mode]['provenance']
    pmu={level:merge_native_runs(runs) for level,runs in cache.items()}
    for level,run in pmu.items():sources[level]=run['provenance']
    return native,external,policies,pmu,sources


def mode_paths(specs):
    result={}
    for spec in specs or []:
        mode, separator, path = spec.partition('=')
        need(separator and mode in ('0','1','2') and int(mode) not in result,'Use each policy once as MODE=/absolute/run')
        result[int(mode)] = Path(path)
    return result


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    input_group=parser.add_mutually_exclusive_group(required=True)
    input_group.add_argument('--native-run',type=Path)
    input_group.add_argument('--shards',type=Path,help='Schema-1 manifest of case-disjoint, internally host-paired runs')
    parser.add_argument('--external-run',type=Path)
    parser.add_argument('--l2-run',type=Path)
    parser.add_argument('--l3-run',type=Path)
    parser.add_argument('--policy-timing',nargs='+',metavar='MODE=RUN')
    parser.add_argument('--policy-counts',nargs='+',metavar='MODE=RUN')
    parser.add_argument('--m4-run',type=Path)
    parser.add_argument('--groups',nargs='+',choices=GROUPS,default=GROUPS[:-1])
    parser.add_argument('--profile',choices=['smoke','paper'],default='paper')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--no-render',action='store_true',help='Export verified tables/macros without figures')
    args=parser.parse_args(argv)
    output=args.output.resolve()
    if output.exists() or output==HERE or HERE in output.parents:
        parser.error('Use a fresh output directory outside AE')
    groups=set(args.groups)
    try:
        if args.shards:
            need(not any((args.external_run,args.l2_run,args.l3_run,args.policy_timing,args.policy_counts)),
                 'With --shards, paired native runs must be specified only inside the manifest')
            native,ext,policies,pmu,sources=shard_runs(args.shards,groups,args.profile)
        else:
            native=native_run(args.native_run,'batch',args.profile)
            sources={'native':native['provenance']}
            ext,policies,pmu={},[],{}
        rows=aggregate(native)
        complete_matrix(rows,STAGES,args.profile)
        hardware=hardware_context(native)
        m4rows,m4={},None
        if 'comparison' in groups:
            if not args.shards:
                need(args.external_run,'Comparison requires --external-run; archived CSV is never substituted')
                imported=external_run(args.external_run,native,args.profile)
                ext=imported['rows'];sources['external']=imported['provenance']
            complete_matrix(ext,EXTERNAL,args.profile)
            need({(d,p) for d,p,m in ext} == {(d,p) for d,p,m in rows},'External/native case matrices differ')
        if not args.shards and (groups & {'predecoder','macros'} or ('xeon-macros' in groups and (args.policy_timing or args.policy_counts))):
            timing={mode:native_run(path,'batch',args.profile) for mode,path in mode_paths(args.policy_timing).items()}
            counts={mode:native_run(path,'statistics',args.profile) for mode,path in mode_paths(args.policy_counts).items()}
            policies=policy_results(timing,counts,args.profile)
            for mode,run in timing.items():
                same_inputs(native,run)
                sources[f'policy_{mode}_timing']=run['provenance'];sources[f'policy_{mode}_counts']=counts[mode]['provenance']
        if groups & {'m4','macros'}:
            need(args.m4_run,'M4 figure/macros require --m4-run; archived values are never retained')
            m4=native_run(args.m4_run,'batch',args.profile)
            need('Darwin' in m4['plan']['platform'] and m4['plan']['machine'].lower() in ('arm64','aarch64'),'M4 evidence must originate on Apple Silicon')
            need(m4['plan']['backend']=='scalar','M4 source role must use the scalar structured dual backend')
            need(m4['plan']['source']['commit']==native['plan']['source']['commit'],'M4 and Xeon results use different source commits')
            m4rows=aggregate(m4);complete_matrix(m4rows,STAGES,args.profile);sources['m4']=m4['provenance']
        if groups & {'macros','xeon-macros'}:
            if not args.shards:pmu={level:native_run(path,'pmu',args.profile) for level,path in [('L2',args.l2_run),('L3',args.l3_run)] if path}
            if 'macros' in groups:need(set(pmu)=={'L2','L3'},'Performance macros require both L2 and L3 runs')
            for level,run in pmu.items():sources[level]=run['provenance']
            performance,predecoder,macro_audit=paper_macros(native,pmu,policies,m4,xeon_only='macros' not in groups)
        output.mkdir(parents=True)
        canonical=dict(schema=1,complete=True,profile=args.profile,groups=sorted(groups),sources=sources,
            hardware=hardware,placement=dict(scope=native['plan'].get('collection_scope','single host'),
                           cases=native.get('case_placement',{case:{field:native['plan'].get(field) for field in ('host','platform','machine')} for case in native['inputs']})),
            method_labels=LABELS,ablation_stages=STAGES,best_configuration_pool=CANDIDATES,
            macro_aliases=ALIASES,baseline_policy='reference always means the independently built official e27b8c6a revision',
            native=list(rows.values()),external=list(ext.values()),predecoder=policies,m4=list(m4rows.values()),
            scope='Current article only; historical grid diagnostics and mixed-LTO plots remain archived supplemental material',
            aggregation=dict(xeon='sum integer elapsed_ns / sum measured shots',m4='median of three complete-batch means',
                             selection='minimum across explicitly listed configurations; descriptive envelope, not measured adaptive switching'))
        write_json(output/'canonical.json',canonical)
        write_csv(output/'native.csv',list(rows.values()))
        if ext:write_csv(output/'external.csv',list(ext.values()))
        if policies:write_csv(output/'predecoder.csv',policies)
        if m4rows:write_csv(output/'m4.csv',list(m4rows.values()))
        selections=[]
        for name, pool, metric in [('xeon',rows,'mean_us'),('m4',m4rows,'median_batch_us')]:
            for d,p in sorted({(d,p) for d,p,m in pool}):
                chosen=min(CANDIDATES,key=lambda method:pool[d,p,method][metric])
                item=pool[d,p,chosen]
                selections.append(dict(platform=name,distance=d,error_rate=str(p),selected_configuration=chosen,
                    host=item.get('host'),machine=item.get('machine'),recorded_platform=item.get('platform'),structured_backend=item.get('structured_backend'),
                    threads=item['threads'],shots=item['shots'],repeats=item['repeats'],aggregation=metric,
                    time_us=item[metric],reference_us=pool[d,p,'reference'][metric],
                    speedup=pool[d,p,'reference'][metric]/item[metric],input_sha256=item['input_sha256']))
        write_csv(output/'selected-configurations.csv',selections)
        banner='% Fresh verified AE export; '+args.profile+' profile.\n'
        banner+='% '+hardware_note(rows)+'\n'
        if args.shards:banner+='% Per-setting host pairing; distributed collection. See canonical.json placement.cases.\n'
        if policies and len(policies)==21:
            (output/'predecoder_numbers.tex').write_text(banner+predecoder_macros(policies))
        if groups & {'macros','xeon-macros'}:
            (output/'performance-macros.tex').write_text(banner+performance)
            write_json(output/'macro-audit.json',macro_audit)
        artifacts=render_figures(output,groups,rows,ext,policies,m4rows,args.profile) if not args.no_render else []
        # Recheck all receipts after rendering; individual inputs were verified before export.
        for provenance in sources.values():
            for item in provenance.values():
                need(R.digest(item['path'])==item['sha256'],'Source receipt changed during export')
        write_json(output/'complete.json',dict(complete=True,profile=args.profile,groups=sorted(groups),sources=sources,
            exporter=reference(__file__),figures=artifacts,
            files={str(p.relative_to(output)):R.digest(p) for p in output.rglob('*') if p.is_file() and '.matplotlib' not in p.parts}))
        print(output/'complete.json')
        return 0
    except (OSError,ValueError,KeyError,TypeError) as error:
        if output.exists() and not (output/'complete.json').exists():
            write_json(output/'failure.json',dict(complete=False,error=str(error)))
        print(str(error),file=sys.stderr)
        return 1


if __name__=='__main__':
    raise SystemExit(main())
