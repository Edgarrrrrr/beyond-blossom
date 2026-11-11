#!/usr/bin/env python3
"""Build a complete Batch3-only source from sealed acquisition receipts, without plots."""
import argparse
import csv
from datetime import datetime, timezone
import itertools
import json
from pathlib import Path
import sys

import migrate_paper_numbers as M


def batch3(rounds):
    M.need(len(rounds) == 3 and {r['repeat'] for r in rounds} == {1, 2, 3}, '需要完整三轮 batch')
    M.need(all(r['shots'] == 1_000_000 and r['elapsed_ns'] > 0 for r in rounds), 'batch 轮次数据无效')
    # Identical operation order to the frozen full analyzer and its heatmaps.
    return sum(r['elapsed_ns'] for r in rounds) / sum(r['shots'] for r in rounds) / 1000


def collect(run, output, spec):
    run, output = run.resolve(), output.resolve()
    M.need(not output.exists(), f'汇总目录必须为新目录：{output}')
    plan_path = run / 'plan.json'
    plan = M.read_json(plan_path)
    for name in ['experiment_id', 'implementation_commit']:
        M.need(plan[name] == spec[name], f'计划身份不符：{name}')
    protocol = plan['protocol']
    M.need(protocol['lto'] == spec['lto'] and protocol['warmup_seconds'] >= 2, '构建或预热配置不符')
    M.need(plan['shots'] == 1_000_000 and protocol['repeats'] == 3, '需要每轮百万、三轮重复')
    M.need(set(protocol['modes']) == {'batch', 'latency'}, '预期完整 batch/latency 矩阵')
    M.need(len(plan['cases']) == 21 and len(plan['methods']) == 7 and len(plan['lanes']) == 6, '矩阵范围不符')
    expected = set(itertools.product([c['id'] for c in plan['cases']], protocol['modes'], range(1, 4), plan['methods']))
    records, lanes, validations, all_warmups = {}, [], [], []
    for lane in plan['lanes']:
        node = lane['node']
        M.need(node in {f'ares{i}' for i in range(1, 7)}, '节点必须为 ares1–6')
        done_path = run / 'lanes' / node / 'complete.json'
        done = M.read_json(done_path)
        M.need(done['complete'] is True and done['status'] == 'complete' and done['node'] == node,
               f'节点采集未完成：{node}')
        M.checked_reference(done['plan'], run, plan_path)
        exit_path = run / f"exit-{done['job_id']}_0.txt"
        M.need(exit_path.is_file() and exit_path.read_text().strip() == '0', f'节点尚未成功退出：{node}')
        lanes.append(dict(node=node, complete=M.reference(done_path), exit=M.reference(exit_path)))
        lane_expected = {(t['case'], t['mode'], t['repeat'], method) for t in lane['tasks'] for method in t['methods']}
        actual = set()
        for item in done['passes']:
            key = item['case'], item['mode'], item['repeat'], item['method']
            M.need(key not in records and key in lane_expected, f'重复或计划外调用：{key}')
            actual.add(key)
            validation_path = M.checked_reference(item['validation'], run)
            validation = M.read_json(validation_path)
            execution = plan['method_settings'][item['method']]['execution']
            physical_mode = ('pipe-' if execution == 'pipeline' else '') + item['mode']
            M.need(validation['complete'] is True and validation['shots'] == plan['shots'] and
                   validation['phase'] == physical_mode and validation['exact_integer_and_observable_differences'] == 0,
                   f'调用未通过精确校验：{key}')
            directory = validation_path.parent
            def file(name):
                return M.checked_reference(validation['files'][name], directory, directory / name)
            warm = M.read_json(file('warmup.json'))
            M.need(warm['requested_seconds'] == 2 and warm['actual_elapsed_ns'] >= 2_000_000_000, f'预热不足：{key}')
            all_warmups.append(int(warm['actual_elapsed_ns']))
            correct = M.read_json(file('correctness.json'))
            M.need(correct['status'] == 'passed' and correct['shots'] == plan['shots'] and
                   correct['weight_differences'] == correct['observable_differences'] == 0, f'正确性差异：{key}')
            with file('measurement.csv').open(newline='') as stream:
                measures = list(csv.DictReader(stream))
            M.need(len(measures) == 1, f'原生记录数不符：{key}')
            measurement = measures[0]
            total, shots = int(validation['elapsed_ns']), int(validation['shots'])
            M.need(total > 0 and total == int(measurement['elapsed_ns']) and shots == int(measurement['shots']) and
                   measurement['mode'] == physical_mode and measurement['execution'] == execution and
                   int(measurement['weight_diff']) == int(measurement['observable_diff']) == 0,
                   f'原生计时或校验结果不符：{key}')
            M.need(int(measurement['total_threads']) == plan['method_settings'][item['method']]['threads'], f'线程设置不符：{key}')
            completion = M.read_json(file('completion.json'))
            drain = completion['final_drain_ns'] or 0
            M.need(completion['complete_elapsed_ns'] == total + drain, f'完整耗时或 drain 不符：{key}')
            if item['mode'] == 'batch':
                M.need(drain == 0, f'batch drain 未包含在最后块：{key}')
                offset = elapsed = 0
                with file('native_blocks.csv').open(newline='') as stream:
                    for block in csv.DictReader(stream):
                        count, ns = int(block['count']), int(block['elapsed_ns'])
                        M.need(int(block['first_shot_index']) == int(block['first_shot_execution_index']) == offset,
                               f'batch 块顺序不符：{key}')
                        M.need(count == min(protocol['block'], shots - offset) and ns > 0, f'batch 块长度或耗时不符：{key}')
                        offset += count
                        elapsed += ns
                M.need(offset == shots and elapsed == total, f'batch 原始块汇总不符：{key}')
            records[key] = dict(repeat=item['repeat'], node=node, shots=shots, elapsed_ns=total,
                                validation=item['validation'], warmup_seconds=int(warm['actual_elapsed_ns']) / 1e9)
            validations.append(dict(case=item['case'], mode=item['mode'], method=item['method'], **records[key]))
        M.need(actual == lane_expected, f'节点覆盖不完整：{node}')
        print(f'{node}: {len(actual)} calls verified', flush=True)
    M.need(set(records) == expected and len(records) == 882, '完整882次采集不齐')
    cases = []
    for case in plan['cases']:
        methods = {}
        for method in plan['methods']:
            rounds = [records[(case['id'], 'batch', repeat, method)] for repeat in range(1, 4)]
            methods[method] = dict(batch3_us=batch3(rounds), rounds=dict(batch=rounds))
        cases.append(dict(id=case['id'], d=case['d'], p=case['p'], rounds=case['rounds'], methods=methods))
    summary = dict(complete=True, experiment_id=plan['experiment_id'], implementation_commit=plan['implementation_commit'],
                   official_commit=plan['official_commit'], lto=protocol['lto'], plan=M.reference(plan_path),
                   lanes=lanes, cases=cases, formal_calls=882, input_records=21_000_000,
                   minimum_warmup_seconds=min(all_warmups) / 1e9, available_metrics=['batch3_us'],
                   aggregation='sum(three batch elapsed_ns) / sum(three shots) / 1000; identical to frozen heatmap analyzer',
                   verification_scope='All 882 validation receipts, warmup/correctness/native measurement/completion files rehashed; all 441 batch block CSVs rehashed and summed. Million-row shot correctness relies on the hash-linked completed acquisition validation; those large CSVs were not re-read.')
    output.mkdir(parents=True)
    for name, data in [('summary.json', summary), ('validation-index.json', validations)]:
        (output / name).write_text(M.json_text(data))
    (output / 'complete.json').write_text(M.json_text(dict(complete=True, created_utc=datetime.now(timezone.utc).isoformat(),
        plan=M.reference(plan_path), summary=M.reference(output / 'summary.json'),
        validations=M.reference(output / 'validation-index.json'), collector=M.reference(__file__),
        helper=M.reference(M.__file__), validated_calls=882, batch_calls=441, exact_differences=0,
        minimum_warmup_seconds=summary['minimum_warmup_seconds'], available_metrics=['batch3_us'],
        scope='Complete Batch3 numeric analysis; latency distribution plotting is a separate analysis.')))
    print(output / 'complete.json', flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mapping', type=Path, default=M.HERE / 'paper_num_mapping.json')
    parser.add_argument('--run', required=True, type=Path, help='完整原始采集目录；离线数值包不包含原始 shot/block 文件')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        config = M.read_json(args.mapping)
        spec = config['source']
        collect(args.run, args.output, spec)
    except (M.MigrationError, OSError, ValueError, KeyError, TypeError) as error:
        print(f'Batch3汇总失败：{error}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
