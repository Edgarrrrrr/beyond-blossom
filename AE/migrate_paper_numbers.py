#!/usr/bin/env python3
"""Fill confirmed LaTeX performance keys from a completed matrix, using stdlib only."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP, localcontext
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile

from pmu_data import PmuData

HERE = Path(__file__).resolve().parent
DECLARATION = re.compile(
    r"^[ \t]*\\DefinePerfValue\{(?P<key>[^{}\r\n]+)\}\{(?P<value>[^{}\r\n]*)\}", re.MULTILINE
)
NUMBER = re.compile(r"-?[0-9]+(?:\.[0-9]+)?\Z")
METRICS = {"batch3_us", "min3_us", "median3_us"}


class MigrationError(Exception):
    """Invalid/incomplete evidence or an unresolved mapping; no TeX is written."""


def need(condition, message):
    if not condition:
        raise MigrationError(message)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            result.update(block)
    return result.hexdigest()


def read_json(path):
    path = Path(path)
    need(path.is_file(), f"缺少文件：{path}")
    return json.loads(path.read_bytes(), parse_float=Decimal)


def resolve_path(value, base):
    path = Path(value)
    return (path if path.is_absolute() else base / path).resolve()


def reference(path):
    path = Path(path).resolve()
    return {"path": str(path), "sha256": digest(path)}


def checked_reference(value, base, expected=None):
    need(isinstance(value, dict) and {"path", "sha256"} <= value.keys(), "来源缺少 path/sha256")
    path = resolve_path(value["path"], base)
    need(path.is_file(), f"缺少来源凭据：{path}")
    need(expected is None or path == expected.resolve(), f"凭据指向其他文件：{path}")
    need(digest(path) == value["sha256"], f"来源 SHA256 不符：{path}")
    return path


def json_text(value):
    def encode(obj):
        if isinstance(obj, Decimal):
            return str(obj)
        raise TypeError(type(obj).__name__)
    return json.dumps(value, ensure_ascii=False, indent=2, default=encode) + "\n"


@dataclass(frozen=True)
class Declaration:
    key: str
    value: str
    start: int
    end: int


def declarations(text):
    result = []
    seen = set()
    for match in DECLARATION.finditer(text):
        key, value = match.group("key", "value")
        need(key not in seen, f"模板重复定义：{key}")
        need(NUMBER.fullmatch(value), f"模板值不是裸十进制数：{key}={value}")
        seen.add(key)
        result.append(Declaration(key, value, match.start("value"), match.end("value")))
    need(result, "模板没有单行 DefinePerfValue 定义")
    return result


def is_m4(key):
    return "m4" in key.split(".", 1)[0].casefold()


def validate_mapping(config, items):
    need(config.get("schema_version") == 1, "映射 schema_version 必须为 1")
    need(not config.get("pending_confirmation"), "仍有待用户确认的映射：" + str(config.get("pending_confirmation")))
    keys = {item.key for item in items}
    rules, preserve = config.get("rules", {}), config.get("preserve", {})
    need(not (set(rules) & set(preserve)), "同一个字段不能同时修改和保留")
    need(not ((set(rules) | set(preserve)) - keys), "映射中含模板不存在的字段")
    pending = []
    for item in items:
        if is_m4(item.key):
            need(item.key not in rules, f"M4 字段禁止配置更新规则：{item.key}")
        elif item.key in preserve:
            need(isinstance(preserve[item.key], str) and preserve[item.key].strip(),
                 f"保留字段需要注明已确认原因：{item.key}")
        elif item.key not in rules:
            pending.append(item.key)
    need(not pending, "以下字段尚未确认映射，请向用户确认后配置 rules/preserve：\n  " + "\n  ".join(pending))
    need(config.get("metric") in METRICS, "请先确认 metric：batch3_us / min3_us / median3_us")
    aliases = config.get("aliases", {})
    for key, rule in rules.items():
        op = rule.get("op")
        need(op in {"time", "time_per_round", "speedup", "best_max_speedup", "min_speedup", "max_speedup", "geomean_speedup", "count_faster", "pmu_miss_percent"},
             f"未知规则 op：{key}: {op}")
        if op == "pmu_miss_percent":
            need(config.get("pmu_source") and rule.get("level") in {"L2", "L3"} and
                 rule.get("unit") == "percent" and "d" in rule and "p" in rule,
                 f"PMU规则需要明确来源、L2/L3、d/p和percent单位：{key}")
            need(key.rsplit('.', 1)[-1] == rule['level']+'miss', f"PMU宏名与缓存级别不符：{key}")
        required = [rule.get(name) for name in ("method", "reference") if name in rule]
        if op == "best_max_speedup":
            need(config.get("best_methods"), "请确认 best 所包含的候选版本 best_methods")
            required += config["best_methods"]
        for alias in required:
            need(alias in aliases and isinstance(aliases[alias], str) and aliases[alias],
                 f"请先确认版本映射 aliases[{alias!r}]（用于 {key}）")
        if op not in {"time", "time_per_round", "pmu_miss_percent"}:
            need(rule.get("reference"), f"缺少明确分母/reference：{key}")


class Matrix:
    def __init__(self, config, config_dir, run_override=None, analysis_override=None, pmu_override=None):
        spec = config["source"]
        self.run = resolve_path(run_override or spec["run"], config_dir)
        self.analysis = resolve_path(analysis_override or spec["analysis"], self.run)
        receipt_path = self.analysis / "complete.json"
        need(receipt_path.is_file(),
             f"最新矩阵分析尚未完成：{receipt_path}\n不会使用部分数据，也不会自动回退到旧实验。")
        receipt = read_json(receipt_path)
        need(receipt.get("complete") is True, "分析未完成")
        revision = self.analysis.name.removeprefix("analysis-")
        submission_path = self.run / f"analysis-submission-{revision}.json"
        analysis_exit = None
        if submission_path.is_file():
            submission = read_json(submission_path)
            need(submission.get("returncode") == 0 and submission.get("job_id"), "分析提交未成功")
            analysis_exit = self.run / f"exit-{submission['job_id']}_0.txt"
            need(analysis_exit.is_file() and analysis_exit.read_text().strip() == "0", "分析作业尚未成功退出")
        summary_path = checked_reference(receipt.get("summary"), self.analysis, self.analysis / "summary.json")
        plan_path = checked_reference(receipt.get("plan"), self.analysis, self.run / "plan.json")
        summary, plan = read_json(summary_path), read_json(plan_path)
        need(summary.get("complete") is True, "summary 未完成")
        for name in ("experiment_id", "implementation_commit"):
            need(summary.get(name) == plan.get(name) == spec.get(name), f"数据身份不符：{name}")
        need(summary.get("lto") == spec.get("lto") == plan.get("protocol", {}).get("lto"), "LTO 配置不符")
        checked_reference(summary.get("plan"), self.analysis, plan_path)
        need(plan.get("shots") == 1_000_000 and plan["protocol"].get("repeats") == 3,
             "需要每轮百万 shots、三轮的完整矩阵")
        need(plan["protocol"].get("warmup_seconds", 0) >= 2, "计划预热不足2秒")
        need(receipt.get("minimum_warmup_seconds", 0) >= 2, "实际预热不足2秒")
        expected_calls = len(plan["cases"]) * len(plan["methods"]) * len(plan["protocol"]["modes"]) * 3
        need(receipt.get("validated_calls") == summary.get("formal_calls") == expected_calls,
             "正式调用数不完整")
        need(receipt.get("exact_differences") == 0, "正式结果存在正确性差异")
        need(len(summary.get("lanes", [])) == len(plan["lanes"]), "节点完成凭据不完整")
        need({lane["node"] for lane in summary["lanes"]} == {lane["node"] for lane in plan["lanes"]},
             "节点身份不完整或有重复")
        for lane in summary["lanes"]:
            path = checked_reference(lane["complete"], self.analysis)
            done = read_json(path)
            need(done.get("complete") is True and done.get("status") == "complete", f"节点未完成：{path}")
            checked_reference(done.get("plan"), self.analysis, plan_path)
            exit_path = checked_reference(lane["exit"], self.analysis)
            need(exit_path.read_text().strip() == "0", f"节点退出非0：{exit_path}")
        self.rows = []
        seen = set()
        plan_metadata = {(int(c["d"]), Decimal(str(c["p"]))): c for c in plan["cases"]}
        plan_cases = set(plan_metadata)
        need(len(plan_cases) == len(plan["cases"]), "冻结计划中有重复 d/p")
        for case in summary["cases"]:
            identity = (int(case["d"]), Decimal(str(case["p"])))
            need(identity not in seen, f"重复 d/p：{identity}")
            seen.add(identity)
            need(set(case["methods"]) == set(plan["methods"]), f"设置缺少版本：{case['id']}")
            self.rows.append(dict(case, rounds=plan_metadata.get(identity, {}).get("rounds")))
        need(seen == plan_cases and len(summary["cases"]) == len(plan["cases"]), "d/p 矩阵不完整")
        distances = set(spec["distances"])
        probabilities = {Decimal(str(p)) for p in spec["probabilities"]}
        need(seen == {(d, p) for d in distances for p in probabilities}, "数据不符合确认的完整 d/p 范围")
        self.metric = config["metric"]
        self.aliases = config["aliases"]
        self.config = config
        self.plan = plan
        self.config_dir = Path(config_dir)
        self.pmu_override = pmu_override
        self.pmu = None
        self.provenance = {"run": str(self.run), "analysis": str(self.analysis), "summary": reference(summary_path),
                           "completion": reference(receipt_path), "plan": reference(plan_path),
                           "analysis_exit": reference(analysis_exit) if analysis_exit else None,
                           "experiment_id": summary["experiment_id"], "implementation_commit": summary["implementation_commit"],
                           "metric": self.metric, "lto": summary["lto"], "validated_calls": expected_calls}

    def selected(self, rule):
        result = self.rows
        if "d" in rule:
            result = [c for c in result if c["d"] == rule["d"]]
        if "distances" in rule:
            result = [c for c in result if c["d"] in rule["distances"]]
        if "p" in rule:
            result = [c for c in result if Decimal(str(c["p"])) == Decimal(str(rule["p"]))]
        need(result, f"规则没有匹配的实测设置：{rule}")
        if "distances" in rule:
            need({c["d"] for c in result} == set(rule["distances"]), "规则要求的距离未全部实测")
        return result

    def time(self, case, alias):
        method = self.aliases[alias]
        need(method in case["methods"], f"设置 {case['id']} 不含版本 {method}")
        raw = case["methods"][method].get(self.metric)
        need(raw is not None and not isinstance(raw, bool), f"缺少指标：{case['id']} / {method} / {self.metric}")
        value = Decimal(str(raw))
        need(value.is_finite() and value > 0, f"耗时必须为有限正数：{case['id']} / {method}")
        return value

    def calculate(self, rule):
        if rule['op'] == 'pmu_miss_percent':
            if self.pmu is None:
                spec = self.config['pmu_source']
                manifest = resolve_path(self.pmu_override or spec['manifest'], self.config_dir)
                self.pmu = PmuData(manifest, None if self.pmu_override else spec['sha256'], self.plan)
                self.provenance['pmu'] = self.pmu.provenance
            return self.pmu.calculate(rule, self.aliases)
        cases = self.selected(rule)
        op = rule["op"]
        operands = []
        values = []
        for case in cases:
            candidate_times = None
            if op == "best_max_speedup":
                times = {alias: self.time(case, alias) for alias in self.config["best_methods"]}
                candidate_times = {self.aliases[alias]: value for alias, value in times.items()}
                method = min(times, key=times.get)
                denominator = times[method]
            else:
                method = rule["method"]
                denominator = self.time(case, method)
            numerator = self.time(case, rule["reference"]) if op not in {"time", "time_per_round"} else None
            value = numerator / denominator if numerator is not None else denominator
            rounds = None
            if op == "time_per_round":
                rounds = case.get("rounds")
                need(isinstance(rounds, int) and not isinstance(rounds, bool) and rounds > 0,
                     f"每轮耗时需要该设置冻结的正整数 rounds：{case['id']}")
                need("expected_rounds" not in rule or rounds == rule["expected_rounds"], "冻结 rounds 与确认的每轮归一化分母不符")
                value /= rounds
            operands.append({"case": case["id"], "method": self.aliases[method], "method_us": denominator,
                             "reference": self.aliases[rule["reference"]] if numerator is not None else None,
                             "reference_us": numerator, "speedup": value if numerator is not None else None,
                             "divide_by_rounds": rounds, "candidate_times_us": candidate_times})
            values.append(value)
        if op in {"time", "time_per_round", "speedup"}:
            need(len(values) == 1, f"单设置规则匹配了多项：{rule}")
            value = values[0]
        elif op == "min_speedup":
            value = min(values)
        elif op in {"max_speedup", "best_max_speedup"}:
            value = max(values)
        elif op == "geomean_speedup":
            value = (sum(v.ln() for v in values) / len(values)).exp()
        else:
            value = Decimal(sum(v > 1 for v in values))
        return value, operands


def render(text, items, config, matrix):
    replacements = []
    audit = []
    with localcontext() as context:
        context.prec = 60
        for item in items:
            if is_m4(item.key) or item.key in config.get("preserve", {}):
                reason = "用户要求所有 M4 字段原样保留" if is_m4(item.key) else config["preserve"][item.key]
                audit.append({"key": item.key, "status": "preserved", "value": item.value, "reason": reason})
                continue
            rule = config["rules"][item.key]
            value, operands = matrix.calculate(rule)
            places = len(item.value.partition(".")[2])
            rounded = format(value.quantize(Decimal(1).scaleb(-places), rounding=ROUND_HALF_UP), "f")
            if rule["op"] == "count_faster":
                need(places == 0, f"设置数应为整数：{item.key}")
            replacements.append((item.start, item.end, rounded))
            audit.append({"key": item.key, "status": "updated", "old": item.value, "new": rounded,
                          "raw_value": value, "decimal_places": places, "rule": rule, "operands": operands})
    output = text
    for start, end, value in reversed(replacements):
        output = output[:start] + value + output[end:]
    # Only numeric spans may change. This also enforces byte preservation of M4 lines.
    after = declarations(output)
    old_skeleton, new_skeleton = text, output
    for item in reversed(items):
        old_skeleton = old_skeleton[:item.start] + "<value>" + old_skeleton[item.end:]
    for item in reversed(after):
        new_skeleton = new_skeleton[:item.start] + "<value>" + new_skeleton[item.end:]
    need(old_skeleton == new_skeleton, "发生了数值以外的模板修改")
    need({i.key: i.value for i in items if is_m4(i.key)} == {i.key: i.value for i in after if is_m4(i.key)}, "M4 字段发生变化")
    return output, audit


def atomic_write(path, data):
    path = Path(path)
    need(path.parent.is_dir(), f"输出目录不存在：{path.parent}")
    fd, name = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        os.fchmod(fd, stat.S_IMODE(path.stat().st_mode) if path.exists() else 0o644)
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path, default=HERE / "paper_num_template.tex")
    parser.add_argument("--mapping", type=Path, default=HERE / "paper_num_mapping.json")
    parser.add_argument("--run", type=Path, help="覆盖映射中的矩阵 run 目录")
    parser.add_argument("--analysis", help="覆盖分析目录名，例如 analysis-r02")
    parser.add_argument("--pmu-data", type=Path, help="显式改用另一个完整PMU包的SOURCE.json；仍核验全部计数和来源")
    destination = parser.add_mutually_exclusive_group()
    destination.add_argument("--output", type=Path, help="生成新 TeX，模板保留")
    destination.add_argument("--in-place", action="store_true", help="替换模板数值，并保留带 SHA256 的原模板备份")
    parser.add_argument("--check", action="store_true", help="完成校验和计算，仅输出预览，不写文件")
    parser.add_argument("--audit", type=Path, help="溯源 JSON 路径，默认 <输出>.audit.json")
    args = parser.parse_args(argv)
    try:
        raw = args.template.read_bytes()
        text = raw.decode("utf-8")
        items = declarations(text)
        config = read_json(args.mapping)
        validate_mapping(config, items)
        matrix = Matrix(config, args.mapping.resolve().parent, args.run, args.analysis, args.pmu_data)
        output, changes = render(text, items, config, matrix)
        for change in changes:
            if change["status"] == "updated":
                print(f"{change['key']}: {change['old']} -> {change['new']}")
        print(f"已计算 {sum(c['status'] == 'updated' for c in changes)} 项；保留 {sum(c['status'] == 'preserved' for c in changes)} 项（含所有 M4）。")
        if args.check:
            return 0
        need(args.in_place or args.output is not None, "请指定 --output 或 --in-place；预览使用 --check")
        target = args.template.resolve() if args.in_place else args.output.resolve()
        need(args.in_place or target != args.template.resolve(), "写回模板必须明确指定 --in-place")
        audit_path = args.audit.resolve() if args.audit else target.with_name(target.name + ".audit.json")
        protected = {args.template.resolve(), args.mapping.resolve(), Path(matrix.provenance['summary']['path']),
                     Path(matrix.provenance['completion']['path']), Path(matrix.provenance['plan']['path']),
                     Path(__file__).resolve()}
        if matrix.pmu is not None:
            protected.update(matrix.pmu.paths)
            protected.add(Path(sys.modules[PmuData.__module__].__file__).resolve())
        need(audit_path != target and audit_path not in protected, "audit 路径不能覆盖模板、输出或数据来源")
        need(args.in_place or target not in protected, "输出路径不能覆盖数据来源或映射")
        need(target.parent.is_dir() and audit_path.parent.is_dir(), "输出目录不存在")
        backup = None
        if args.in_place:
            backup = target.with_name(target.name + ".bak-" + hashlib.sha256(raw).hexdigest()[:16])
            if backup.exists():
                need(backup.read_bytes() == raw, f"已有备份内容不同：{backup}")
            else:
                with backup.open("xb") as stream:
                    stream.write(raw)
        data = output.encode("utf-8")
        audit = {"complete": True, "source": matrix.provenance, "mapping": reference(args.mapping),
                 "mapping_snapshot": config,
                 "script": reference(__file__), "template": {"path": str(args.template.resolve()), "sha256": hashlib.sha256(raw).hexdigest()},
                 "output": {"path": str(target), "sha256": hashlib.sha256(data).hexdigest()},
                 "backup": str(backup) if backup else None, "rounding": "Decimal, 60 significant digits; preserve template decimal places, ROUND_HALF_UP",
                 "changes": changes}
        # Prepare/validate everything before the atomic TeX replacement.
        audit_bytes = json_text(audit).encode("utf-8")
        atomic_write(target, data)
        atomic_write(audit_path, audit_bytes)
        print(f"TeX: {target}\n溯源: {audit_path}")
        return 0
    except (MigrationError, OSError, ValueError, KeyError, TypeError, InvalidOperation) as exc:
        print(f"迁移失败：{exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
