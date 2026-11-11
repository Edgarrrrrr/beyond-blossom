"""Bundle and verify completed L2/L3 request-counter runs using only the stdlib."""
from __future__ import annotations

import argparse
import csv
from decimal import Decimal, localcontext
import hashlib
import itertools
import json
from pathlib import Path
import shutil
import sys
import tempfile

HERE = Path(__file__).resolve().parent
LEVELS = {
    'L2': dict(prefix='l2', metric='l2_load_miss_percent', events=[
        dict(name='L2_RQSTS.ALL_DEMAND_DATA_RD', type=4, config=0xe124),
        dict(name='L2_RQSTS.DEMAND_DATA_RD_MISS', type=4, config=0x2124)]),
    'L3': dict(prefix='llc', metric='l3_load_miss_percent', events=[
        dict(name='LLC-loads', type=3, config=2), dict(name='LLC-load-misses', type=3, config=65538)]),
}
NODES = {f'ares{i}' for i in range(1, 7)}
METHODS = {'Upstream', 'GridSIMD'}
MATRIX = set(itertools.product([9, 13, 17, 21, 25, 29, 33], map(Decimal, ['.0005', '.001', '.003'])))


class PmuError(ValueError):
    pass


def need(ok, message):
    if not ok:
        raise PmuError(message)


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def read(path):
    return json.loads(Path(path).read_bytes())


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + '\n')


def original_ref(path):
    path = Path(path).resolve()
    return dict(path=str(path), sha256=digest(path))


def original_checked(value):
    path = Path(value['path'])
    need(path.is_file() and digest(path) == value['sha256'], f'原PMU工件缺失或SHA不符：{path}')
    return path


def counters(value, level, shots):
    spec = LEVELS[level]
    need(value.get('mode') == 'pmu' and value.get('shots') == shots, 'PMU模式或shots不符')
    need(value.get('events') == spec['events'], f'{level}事件类型或编码不符')
    need(value.get('latency_collected') is False and value.get('scaled') is False and
         value.get('multiplexed') is False, 'PMU数据包含延迟、缩放或时间复用')
    need(value.get('pinned_group') is True and value.get('inherit') is False and
         value.get('exclude_kernel') is True and value.get('exclude_hv') is True,
         'PMU线程/计数范围不符')
    need(value.get('time_enabled_ns') == value.get('time_running_ns') and
         value.get('time_enabled_ns', 0) > 0, 'PMU组未完整运行')
    loads, misses = value[spec['prefix'] + '_loads'], value[spec['prefix'] + '_load_misses']
    need(type(loads) is int and type(misses) is int and loads > 0 and 0 <= misses <= loads,
         'PMU原始计数无效')
    raw = value.get('group_raw', [])
    need(len(raw) == 7 and raw[0] == 2 and raw[1] == raw[2] == value['time_enabled_ns'] and
         raw[3] == loads and raw[5] == misses and raw[4] != raw[6], 'PMU组原始读取不符')
    with localcontext() as ctx:
        ctx.prec = 60
        percent = Decimal(100) * misses / loads
        need(abs(Decimal(str(value[spec['metric']])) - percent) < Decimal('1e-12'),
             '缓存的PMU百分比与原始计数不符')
    return loads, misses


class PmuData:
    def __init__(self, manifest=HERE / 'data/pmu/SOURCE.json', expected_sha=None, expected_plan=None):
        self.manifest = Path(manifest).resolve()
        self.root = self.manifest.parent
        self.paths = {self.manifest}
        need(expected_sha is None or digest(self.manifest) == expected_sha, 'PMU数据包SOURCE SHA256不符')
        source = read(self.manifest)
        need(source.get('schema_version') == 1 and source.get('complete') is True, 'PMU数据包未完成')
        need(source.get('levels') and set(source['levels']) <= LEVELS.keys(), '未知或空PMU缓存级别')
        self.rows = {}
        self.plans = {}
        for level, block in source['levels'].items():
            self._level(level, block, expected_plan)
        if len(self.plans) == 2:
            identity = lambda p: {(c['d'], Decimal(str(c['p']))): c['inputs']['dataset']['sha256'] for c in p['cases']}
            a, b = self.plans.values()
            need(a['implementation_commit'] == b['implementation_commit'] and identity(a) == identity(b),
                 'L2/L3使用了不同实现或输入')
        self.provenance = dict(manifest=original_ref(self.manifest), levels=list(source['levels']),
            unit='percent', calculation='100 * integer misses / integer requests; Decimal precision 60',
            verification='Bundled completion/exit/PMU/validation/warmup/correctness/execution metadata and SHA verified; '
                         'million-row exact correctness relies on completed acquisition receipts; no decode re-execution.')

    def checked(self, item):
        relative = Path(item['path'])
        need(not relative.is_absolute(), 'PMU包内凭据必须使用相对路径')
        path = (self.root / relative).resolve()
        need(path.is_relative_to(self.root), 'PMU包内路径越界')
        need(path.is_file() and digest(path) == item['sha256'], f'PMU包工件缺失或SHA不符：{path}')
        self.paths.add(path)
        return path

    def _level(self, level, block, expected_plan):
        plan_path = self.checked(block['plan'])
        summary_path = self.checked(block['summary'])
        plan, summary = read(plan_path), read(summary_path)
        done = read(self.checked(block['completion']))
        need(done.get('complete') is True and done['summary']['sha256'] == digest(summary_path),
             f'{level}原分析未完成或来源不符')
        need(self.checked(block['analysis_exit']).read_text().strip() == '0', f'{level}分析退出非0')
        need(summary.get('complete') is True and summary['plan']['sha256'] == digest(plan_path), 'PMU summary/plan不符')
        protocol = plan['protocol']
        need(protocol.get('lto') == 'OFF' and protocol.get('repeats') == 1 and
             protocol.get('exclusive') is True and protocol.get('latency_collected') is False and
             protocol.get('warmup_seconds', 0) >= 2 and plan.get('shots') == 1000000, 'PMU协议不符')
        need(protocol.get('events') == LEVELS[level]['events'] and protocol.get('metric') == LEVELS[level]['metric'],
             'PMU冻结事件/指标不符')
        need(set(plan['methods']) == METHODS and len(plan['methods']) == 2, 'PMU方法范围不符')
        need(all(plan['method_settings'][m]['threads'] == 1 and plan['method_settings'][m]['execution'] == 'serial' and
                 plan['method_settings'][m]['predecode'] == 0 for m in METHODS), 'PMU入口配置不符')
        need(plan['method_settings']['Upstream']['backend'] == 'generic' and
             plan['method_settings']['GridSIMD']['backend'] == 'avx512vl', 'PMU后端与方法名称不符')
        metadata = {(c['d'], Decimal(str(c['p']))): c for c in plan['cases']}
        need(len(metadata) == len(plan['cases']) == 21 and set(metadata) == MATRIX, 'PMU矩阵不完整')
        need(all(c['rounds'] == c['d'] and c['sigma'] == 0 for c in plan['cases']), 'PMU rounds/sigma不符')
        if expected_plan is not None:
            need(plan['implementation_commit'] == expected_plan['implementation_commit'], 'PMU与计时矩阵实现不一致')
            other = {(c['d'], Decimal(str(c['p']))): c for c in expected_plan['cases']}
            need(set(other) == set(metadata), 'PMU与计时d/p矩阵不一致')
            need(all(metadata[k]['inputs']['dataset']['sha256'] == other[k]['inputs']['dataset']['sha256'] for k in other),
                 'PMU与计时输入不一致')
        planned = {(lane['node'], case, method) for lane in plan['lanes'] for case in lane['cases'] for method in METHODS}
        need(len(planned) == 42 and len({(c, m) for _, c, m in planned}) == 42, 'PMU节点计划有重复或缺项')
        need({lane['node'] for lane in block['lanes']} == {lane['node'] for lane in plan['lanes']} and
             len(block['lanes']) == len(plan['lanes']), 'PMU节点完成凭据不全')
        passes = {}
        for lane in block['lanes']:
            need(lane['node'] in NODES, 'PMU节点不在ares1–6')
            receipt = read(self.checked(lane['complete']))
            need(receipt.get('complete') is True and receipt.get('status') == 'complete' and
                 receipt['node'] == lane['node'] and self.checked(lane['exit']).read_text().strip() == '0', 'PMU节点未成功完成')
            for item in receipt['passes']:
                key = lane['node'], item['case'], item['method']
                need(key not in passes and key in planned, '节点PMU调用重复或不在计划内')
                passes[key] = item
        need(set(passes) == planned, 'PMU节点实际调用不完整')
        need(summary.get('metric') == LEVELS[level]['metric'], 'PMU summary缓存级别不符')
        need(summary.get('formal_calls') == 42 and summary.get('total_decodes') == 42000000 and
             summary.get('latency_collected') is False, 'PMU汇总调用数不完整')
        original = {(r['node'], r['case'], r['method']): r for r in summary['measurements']}
        need(len(original) == len(summary['measurements']) == 42 and set(original) == planned, '原PMU summary缺项或重复')
        with self.checked(block['csv']).open() as stream:
            csv_rows = list(csv.DictReader(stream))
        need(len(csv_rows) == 42 and {(r['node'],r['case'],r['method']) for r in csv_rows} == planned, 'PMU CSV覆盖不完整')
        for row in csv_rows:
            raw = original[row['node'],row['case'],row['method']]
            for field in [LEVELS[level]['prefix']+'_loads', LEVELS[level]['prefix']+'_load_misses']:
                need(int(row[field]) == raw[field], 'PMU CSV计数与summary不一致')
            need(abs(Decimal(row[LEVELS[level]['metric']]) - Decimal(str(raw[LEVELS[level]['metric']]))) < Decimal('1e-12'),
                 'PMU CSV百分比与summary不一致')
        need(len(block['measurements']) == 42, '包内PMU调用数不完整')
        seen = set()
        for row in block['measurements']:
            key = row['node'], row['case'], row['method']
            need(key in planned and key not in seen, '包内PMU调用重复或不在计划内')
            seen.add(key)
            pmu = read(self.checked(row['pmu']))
            validation = read(self.checked(row['validation']))
            need(row['pmu']['sha256'] == original[key]['pmu']['sha256'] == passes[key]['pmu']['sha256'] and
                 row['validation']['sha256'] == original[key]['validation']['sha256'] == passes[key]['validation']['sha256'],
                 'PMU调用的包内/原summary/节点SHA链不符')
            need(validation.get('complete') is True and validation.get('shots') == 1000000 and
                 validation.get('exact_differences') == 0 and validation.get('latency_collected') is False, 'PMU正确性凭据不符')
            for name in ['pmu.json', 'warmup.json', 'correctness.json', 'execution.csv']:
                field = {'pmu.json':'pmu', 'warmup.json':'warmup', 'correctness.json':'correctness', 'execution.csv':'execution'}[name]
                need(validation['files'][name]['sha256'] == row[field]['sha256'], f'PMU校验文件SHA链不符：{name}')
                self.checked(row[field])
            warm, correct = read(self.checked(row['warmup'])), read(self.checked(row['correctness']))
            need(warm['requested_seconds'] >= 2 and warm['actual_elapsed_ns'] >= 2000000000, 'PMU预热不足')
            need(correct.get('status') == 'passed' and correct['shots'] == 1000000 and
                 correct['weight_differences'] == correct['observable_differences'] == 0, 'PMU结果有差异')
            with self.checked(row['execution']).open() as stream:
                executions = list(csv.DictReader(stream))
            need(len(executions) == 1 and executions[0]['execution'] == 'serial' and executions[0]['total_threads'] == '1',
                 'PMU线程配置不符')
            loads, misses = counters(pmu, level, 1000000)
            spec = LEVELS[level]
            need(original[key][spec['prefix']+'_loads'] == loads and original[key][spec['prefix']+'_load_misses'] == misses,
                 'PMU汇总计数与原始组计数不符')
            need(abs(Decimal(str(original[key][spec['metric']])) - Decimal(str(pmu[spec['metric']]))) < Decimal('1e-12'),
                 'PMU summary百分比与原始组记录不一致')
            case = metadata[(original[key]['d'], Decimal(str(original[key]['p'])))]
            need(case['id'] == row['case'], 'PMU case身份不符')
            self.rows[level, row['method'], case['d'], Decimal(str(case['p']))] = dict(
                level=level, method=row['method'], d=case['d'], p=str(case['p']), case=case['id'],
                node=row['node'], shots=1000000, loads=loads, misses=misses,
                pmu=original_ref(self.checked(row['pmu'])), validation=original_ref(self.checked(row['validation'])))
        need(seen == planned, '包内PMU实际覆盖不完整')
        self.plans[level] = plan

    def calculate(self, rule, aliases):
        level, method = rule['level'], aliases[rule['method']]
        key = level, method, int(rule['d']), Decimal(str(rule['p']))
        need(key in self.rows, f'没有匹配的已完成PMU设置：{key}')
        row = self.rows[key]
        with localcontext() as ctx:
            ctx.prec = 60
            value = Decimal(100) * row['misses'] / row['loads']
        return value, [dict(row, unit='percent', formula='100 * integer misses / integer requests')]


def bundle(runs, output):
    output = Path(output).resolve()
    need(not output.exists(), 'PMU输出目录已存在，拒绝覆盖')
    need(runs and set(runs) <= LEVELS.keys(), '请选择L2和/或L3原始run')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.pmu-bundle-', dir=output.parent) as temporary:
        stage = Path(temporary) / 'data'
        stage.mkdir()
        manifest = dict(schema_version=1, complete=True, unit='percent', levels={},
            scope='Byte-exact completed metadata and raw PMU groups; large inputs/binaries/shot CSVs stay external. '
                  'Only bundled relative references are followed during offline verification and macro calculation.')
        for level, run in runs.items():
            run = Path(run).resolve()
            def preserve(path, name):
                path = Path(path).resolve()
                target = stage / level / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
                return dict(path=target.relative_to(stage).as_posix(), sha256=digest(target), original_path=str(path))
            complete = read(run / 'complete.json')
            need(complete['complete'] is True, f'{level} run未完成')
            original_checked(complete['summary'])
            plan, summary = read(run / 'plan.json'), read(run / 'analysis-r01/summary.json')
            need(summary['plan']['sha256'] == digest(run/'plan.json'), '原PMU plan SHA不符')
            block = dict(experiment_id=plan['experiment_id'], implementation_commit=plan['implementation_commit'],
                acquisition_commit=plan['acquisition_commit'], original_run=str(run),
                plan=preserve(run/'plan.json', 'plan.json'), summary=preserve(run/'analysis-r01/summary.json','summary.json'),
                csv=preserve(run/'analysis-r01/summary.csv','summary.csv'), completion=preserve(run/'complete.json','complete.json'),
                analysis_exit=preserve(run/f'exit-{complete["job_id"]}.txt','analysis-exit.txt'), lanes=[], measurements=[])
            for lane in plan['lanes']:
                node = lane['node'];need(node in NODES, '不允许的PMU节点')
                done_path = run/'lanes'/node/'complete.json';done = read(done_path)
                block['lanes'].append(dict(node=node, complete=preserve(done_path,f'lanes/{node}/complete.json'),
                    exit=preserve(run/f'exit-{done["job_id"]}.txt',f'lanes/{node}/exit.txt')))
            for index, row in enumerate(summary['measurements']):
                pmu_path = original_checked(row['pmu']);validation_path = original_checked(row['validation'])
                validation = read(validation_path)
                item = {k:row[k] for k in ['case','method','node']}
                item['pmu'] = preserve(pmu_path, f'calls/{index:02}/pmu.json')
                item['validation'] = preserve(validation_path, f'calls/{index:02}/validation.json')
                for name in ['warmup','correctness','execution']:
                    extension = '.csv' if name == 'execution' else '.json'
                    path = original_checked(validation['files'][name+extension])
                    item[name] = preserve(path, f'calls/{index:02}/{name}{extension}')
                block['measurements'].append(item)
            manifest['levels'][level] = block
        write(stage/'SOURCE.json', manifest)
        PmuData(stage/'SOURCE.json')
        need(not output.exists(), 'PMU输出目录已出现，拒绝覆盖')
        stage.rename(output)
    return output/'SOURCE.json'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest='action', required=True)
    export = actions.add_parser('bundle', help='核验已完成run并导出独立的AE数据包')
    export.add_argument('--l2-run', type=Path)
    export.add_argument('--l3-run', type=Path)
    export.add_argument('--output', type=Path, required=True)
    verify = actions.add_parser('verify', help='离线核验包内全部PMU数据、计数和完成凭据')
    verify.add_argument('--data', type=Path, default=HERE/'data/pmu/SOURCE.json')
    args = parser.parse_args(argv)
    try:
        if args.action == 'bundle':
            runs = {k:v for k,v in [('L2',args.l2_run),('L3',args.l3_run)] if v is not None}
            manifest = bundle(runs, args.output)
        else:
            manifest = args.data
        data = PmuData(manifest)
        print(f'PMU verified: {len(data.rows)} complete million-shot calls; levels={list(data.plans)}')
        for level in data.plans:
            for method in ['Upstream','GridSIMD']:
                value, operands = data.calculate(dict(level=level,method=method,d=33,p='.003'), {method:method})
                print(f'{level} d33/p0.3% {method}: {value:.3f}% ({operands[0]["misses"]}/{operands[0]["loads"]})')
        return 0
    except (PmuError, OSError, KeyError, TypeError, ValueError) as error:
        print(f'PMU失败：{error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
