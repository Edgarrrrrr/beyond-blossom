#!/usr/bin/env python3
"""Independent, read-only audit of a sealed 21-case structured regression run.

Writes derived results only to a new output directory. It refuses partial runs.
"""
import argparse
import collections
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def write_csv(path, rows):
    with path.open('x', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.run.resolve()
    plan = json.loads((root / 'plan.json').read_text())
    complete = json.loads((root / 'complete.json').read_text())
    require(complete.get('complete') is True, 'Run is not sealed complete')
    require(not (root / 'failure.json').exists(), 'Run contains a failure record')
    require(complete['plan_sha256'] == digest(root / 'plan.json'), 'Plan hash mismatch')
    require(complete['raw_sha256'] == digest(root / 'raw.csv'), 'Raw measurement hash mismatch')
    require(plan['schema'] == 2, 'Expected measured official reference schema')
    require(plan['source_revision'] == '241160314087e5ae698c6e76c3c055a11c3ad422', 'Wrong migration source')
    require(plan['reference']['upstream_revision'] == 'e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd', 'Wrong official reference')
    require(len(plan['cases']) == 21 and len(plan['methods']) == 6, 'Wrong case or method count')
    require(plan['repeats'] == 3 and plan['shots'] == 10000, 'Wrong repeat or shot count')
    require(len(complete['inputs']) == 21 and len(complete['calls']) == 819, 'Wrong sealed artifact counts')
    for variant, binary in plan['binaries'].items():
        require(digest(root / 'bin' / variant) == binary['sha256'], f'Binary hash mismatch: {variant}')
    with (root / 'raw.csv').open(newline='') as stream:
        raw = list(csv.DictReader(stream))
    expected = {(case['case'], method, repeat, variant)
                for case in plan['cases'] for method in plan['methods']
                for repeat in range(1, 4) for variant in ('source', 'candidate')}
    expected |= {(case['case'], 'reference', repeat, 'reference')
                 for case in plan['cases'] for repeat in range(1, 4)}
    actual = [(row['case'], row['method'], int(row['repeat']), row['variant']) for row in raw]
    require(len(raw) == 819 and len(actual) == len(set(actual)) and set(actual) == expected,
            'Missing, duplicate, or unexpected measurement rows')
    cases = {case['case']: case for case in plan['cases']}
    groups = collections.defaultdict(lambda: collections.defaultdict(dict))
    dataset_hashes = collections.defaultdict(set)
    correctness_observable_differences = collections.Counter()
    correctness_witnesses = collections.Counter()
    for row in raw:
        case = cases[row['case']]
        require(int(row['distance']) == case['distance'] and float(row['error_rate']) == case['error_rate'],
                'Raw case metadata mismatch')
        require(int(row['shots']) == 10000 and int(row['elapsed_ns']) > 0, 'Invalid shot count or elapsed time')
        ns = int(row['elapsed_ns']) / int(row['shots'])
        require(math.isclose(float(row['ns_per_shot']), ns, rel_tol=1e-12), 'Raw ns/shot arithmetic mismatch')
        options = plan['official_method'] if row['method'] == 'reference' else plan['methods'][row['method']]
        require(int(row['threads']) == options['threads'], 'Thread-count mismatch')
        require(row['backend'] == options.get('backend', plan['backend']), 'Backend mismatch')
        directory = root / row['directory']
        correctness = json.loads((directory / 'correctness.json').read_text())
        require(correctness.get('status') == 'passed' and correctness.get('weight_differences') == 0
                and correctness.get('shots') == 10000, 'Native exact-weight validation failed')
        differences = int(row['observable_differences'])
        witnesses = int(row['actual_candidate_witnesses'])
        require(differences == correctness.get('observable_differences', 0)
                and witnesses == correctness.get('actual_candidate_witnesses', 0)
                and differences >= 0 and witnesses >= differences, 'Observable witness coverage mismatch')
        warmup = json.loads((directory / 'warmup.json').read_text())
        require(warmup.get('requested_seconds') == 2 and warmup.get('actual_elapsed_ns', 0) >= 2000000000,
                'Incomplete warmup')
        scope = json.loads((directory / 'measurement_scope.json').read_text())
        require(scope.get('internal_instrumentation') is False, 'Instrumented timing record')
        dataset_hashes[row['case']].add(row['dataset_sha256'])
        groups[row['case'], row['method']][row['variant']][int(row['repeat'])] = ns
        correctness_observable_differences[row['variant']] += differences
        correctness_witnesses[row['variant']] += witnesses
    require(all(len(values) == 1 for values in dataset_hashes.values()), 'Unequal saved datasets within a case')
    results = []
    for case in plan['cases']:
        official = groups[case['case'], 'reference']['reference']
        for method in plan['methods']:
            source = groups[case['case'], method]['source']
            candidate = groups[case['case'], method]['candidate']
            sm, cm, om = (statistics.median(values.values()) for values in (source, candidate, official))
            paired_changes = [100 * (candidate[k] / source[k] - 1) for k in range(1, 4)]
            migration_paired = [source[k] / candidate[k] for k in range(1, 4)]
            official_paired = [official[k] / candidate[k] for k in range(1, 4)]
            results.append(dict(case=case['case'], distance=case['distance'], error_rate=case['error_rate'], method=method,
                source_median_ns=sm, source_min_ns=min(source.values()), source_max_ns=max(source.values()),
                candidate_median_ns=cm, candidate_min_ns=min(candidate.values()), candidate_max_ns=max(candidate.values()),
                official_median_ns=om, official_min_ns=min(official.values()), official_max_ns=max(official.values()),
                speedup_official_over_candidate=om / cm, normalized_latency_candidate_over_official=cm / om,
                speedup_source_over_candidate=sm / cm, candidate_change_percent=100 * (cm / sm - 1),
                paired_migration_speedup_median=statistics.median(migration_paired),
                paired_migration_speedup_min=min(migration_paired), paired_migration_speedup_max=max(migration_paired),
                paired_official_speedup_median=statistics.median(official_paired),
                paired_official_speedup_min=min(official_paired), paired_official_speedup_max=max(official_paired),
                paired_candidate_change_percent=json.dumps(paired_changes),
                paired_slowdown_repeats=sum(change > 0 for change in paired_changes),
                paired_slowdown_over_5_percent_repeats=sum(change > 5 for change in paired_changes),
                source_candidate_ranges_overlap=max(min(source.values()), min(candidate.values())) <=
                                                min(max(source.values()), max(candidate.values()))))
    require(len(results) == 126, 'Wrong case/method pair count')
    existing = json.loads((root / 'summary.json').read_text())
    existing_rows = {(row['case'], row['method']): row for row in existing['rows']}
    require(len(existing_rows) == 126, 'Collector summary pair count differs')
    compare = ['source_median_ns', 'candidate_median_ns', 'official_median_ns', 'speedup_official_over_candidate',
               'speedup_source_over_candidate', 'candidate_change_percent']
    for row in results:
        original = existing_rows[row['case'], row['method']]
        require(all(math.isclose(row[key], original[key], rel_tol=1e-12, abs_tol=1e-10) for key in compare),
                'Independent calculations disagree with collector summary')
    regressions = sorted((row for row in results if row['candidate_change_percent'] > 0),
                         key=lambda row: row['candidate_change_percent'], reverse=True)
    flags = [row for row in results if row['candidate_change_percent'] > 5
             or row['paired_slowdown_over_5_percent_repeats'] >= 2]
    methods = []
    for method in plan['methods']:
        subset = [row for row in results if row['method'] == method]
        migration = [row['speedup_source_over_candidate'] for row in subset]
        official = [row['speedup_official_over_candidate'] for row in subset]
        methods.append(dict(method=method, settings=len(subset),
            geometric_mean_migration_speedup=math.exp(statistics.mean(map(math.log, migration))),
            minimum_migration_speedup=min(migration), maximum_migration_speedup=max(migration),
            geometric_mean_official_speedup=math.exp(statistics.mean(map(math.log, official))),
            minimum_official_speedup=min(official), maximum_official_speedup=max(official),
            median_latency_regression_cases=sum(row['candidate_change_percent'] > 0 for row in subset),
            median_latency_regression_over_5_percent_cases=sum(row['candidate_change_percent'] > 5 for row in subset),
            paired_systematic_over_5_percent_cases=sum(row['paired_slowdown_over_5_percent_repeats'] >= 2 for row in subset)))
    result = dict(status='verified', run=str(root), raw_sha256=digest(root / 'raw.csv'),
        plan_sha256=digest(root / 'plan.json'), measurements=len(raw), cases=21, method_pairs=len(results),
        repeats=3, independent_shots_per_case=10000, fixed_official_revision=plan['reference']['upstream_revision'],
        migration_source_revision=plan['source_revision'], controller_identity=plan['controller_identity'],
        archived_binary_hashes={variant: item['sha256'] for variant, item in plan['binaries'].items()},
        exact_weight_checks_passed=819, warmup_checks_passed=819,
        observable_differences_across_repeated_calls=dict(correctness_observable_differences),
        actual_witnesses_across_repeated_calls=dict(correctness_witnesses),
        units='ns/shot; each repetition is a batch mean, not an individual-shot percentile',
        interpretation='Ranges are observed repeats, not confidence intervals. Official time / candidate time is the primary speedup; source241 time / candidate time is the separate migration comparison.',
        positive_median_latency_changes=regressions, over_5_percent_flags=flags,
        largest_median_latency_increase=max(results, key=lambda row: row['candidate_change_percent']),
        by_method=methods, rows=results)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'audit.json').write_text(json.dumps(result, indent=2) + '\n')
    write_csv(args.output / 'case_method_comparisons.csv', results)
    write_csv(args.output / 'method_summary.csv', methods)
    if regressions:
        write_csv(args.output / 'all_positive_median_latency_changes.csv', regressions)
    print(json.dumps({key: result[key] for key in ['status', 'measurements', 'cases', 'method_pairs',
          'exact_weight_checks_passed', 'largest_median_latency_increase', 'over_5_percent_flags', 'by_method']}, indent=2))


if __name__ == '__main__':
    main()
