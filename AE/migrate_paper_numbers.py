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
    need(path.is_file(), f"Missing file: {path}")
    return json.loads(path.read_bytes(), parse_float=Decimal)


def resolve_path(value, base):
    path = Path(value)
    return (path if path.is_absolute() else base / path).resolve()


def reference(path):
    path = Path(path).resolve()
    return {"path": str(path), "sha256": digest(path)}


def checked_reference(value, base, expected=None):
    need(isinstance(value, dict) and {"path", "sha256"} <= value.keys(), "Source requires path and sha256")
    path = resolve_path(value["path"], base)
    need(path.is_file(), f"Missing source receipt: {path}")
    need(expected is None or path == expected.resolve(), f"Receipt points to a different file: {path}")
    need(digest(path) == value["sha256"], f"Source SHA256 differs: {path}")
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
        need(key not in seen, f"Duplicate template definition: {key}")
        need(NUMBER.fullmatch(value), f"Template value is not a plain decimal: {key}={value}")
        seen.add(key)
        result.append(Declaration(key, value, match.start("value"), match.end("value")))
    need(result, "Template has no single-line DefinePerfValue definitions")
    return result


def is_m4(key):
    return "m4" in key.split(".", 1)[0].casefold()


def validate_mapping(config, items):
    need(config.get("schema_version") == 1, "Mapping schema_version must be 1")
    need(not config.get("pending_confirmation"), "Mapping contains unresolved confirmations: " + str(config.get("pending_confirmation")))
    keys = {item.key for item in items}
    rules, preserve = config.get("rules", {}), config.get("preserve", {})
    need(not (set(rules) & set(preserve)), "A field cannot be both updated and preserved")
    need(not ((set(rules) | set(preserve)) - keys), "Mapping contains fields absent from the template")
    pending = []
    for item in items:
        if is_m4(item.key):
            need(item.key not in rules, f"M4 fields cannot have update rules: {item.key}")
        elif item.key in preserve:
            need(isinstance(preserve[item.key], str) and preserve[item.key].strip(),
                 f"Preserved field requires a confirmed reason: {item.key}")
        elif item.key not in rules:
            pending.append(item.key)
    need(not pending, "Confirm the unresolved field mappings before configuring rules/preserve: \n  " + "\n  ".join(pending))
    need(config.get("metric") in METRICS, "Set the confirmed metric: batch3_us / min3_us / median3_us")
    aliases = config.get("aliases", {})
    for key, rule in rules.items():
        op = rule.get("op")
        need(op in {"time", "time_per_round", "speedup", "best_max_speedup", "min_speedup", "max_speedup", "geomean_speedup", "count_faster", "pmu_miss_percent"},
             f"Unknown rule operation: {key}: {op}")
        if op == "pmu_miss_percent":
            need(config.get("pmu_source") and rule.get("level") in {"L2", "L3"} and
                 rule.get("unit") == "percent" and "d" in rule and "p" in rule,
                 f"PMU rule requires an explicit source, L2/L3, d/p and percent unit: {key}")
            need(key.rsplit('.', 1)[-1] == rule['level']+'miss', f"PMU macro name differs from its cache level: {key}")
        required = [rule.get(name) for name in ("method", "reference") if name in rule]
        if op == "best_max_speedup":
            need(config.get("best_methods"), "Set the confirmed candidate configurations in best_methods")
            required += config["best_methods"]
        for alias in required:
            need(alias in aliases and isinstance(aliases[alias], str) and aliases[alias],
                 f"Confirm the configuration mapping aliases[{alias!r}] (used by {key})")
        if op not in {"time", "time_per_round", "pmu_miss_percent"}:
            need(rule.get("reference"), f"Missing explicit baseline/reference: {key}")


class Matrix:
    def __init__(self, config, config_dir, run_override=None, analysis_override=None, pmu_override=None):
        spec = config["source"]
        self.run = resolve_path(run_override or spec["run"], config_dir)
        self.analysis = resolve_path(analysis_override or spec["analysis"], self.run)
        receipt_path = self.analysis / "complete.json"
        need(receipt_path.is_file(),
             f"Latest matrix analysis is incomplete: {receipt_path}\nPartial data and automatic fallback to older experiments are disabled.")
        receipt = read_json(receipt_path)
        need(receipt.get("complete") is True, "Analysis is incomplete")
        revision = self.analysis.name.removeprefix("analysis-")
        submission_path = self.run / f"analysis-submission-{revision}.json"
        analysis_exit = None
        if submission_path.is_file():
            submission = read_json(submission_path)
            need(submission.get("returncode") == 0 and submission.get("job_id"), "Analysis submission failed")
            analysis_exit = self.run / f"exit-{submission['job_id']}_0.txt"
            need(analysis_exit.is_file() and analysis_exit.read_text().strip() == "0", "Analysis job has not exited successfully")
        summary_path = checked_reference(receipt.get("summary"), self.analysis, self.analysis / "summary.json")
        plan_path = checked_reference(receipt.get("plan"), self.analysis, self.run / "plan.json")
        summary, plan = read_json(summary_path), read_json(plan_path)
        need(summary.get("complete") is True, "Summary is incomplete")
        for name in ("experiment_id", "implementation_commit"):
            need(summary.get(name) == plan.get(name) == spec.get(name), f"Data identity differs: {name}")
        need(summary.get("lto") == spec.get("lto") == plan.get("protocol", {}).get("lto"), "LTO configuration differs")
        checked_reference(summary.get("plan"), self.analysis, plan_path)
        need(plan.get("shots") == 1_000_000 and plan["protocol"].get("repeats") == 3,
             "A complete matrix with three repeats of one million shots is required")
        need(plan["protocol"].get("warmup_seconds", 0) >= 2, "Planned warmup is shorter than two seconds")
        need(receipt.get("minimum_warmup_seconds", 0) >= 2, "Actual warmup is shorter than two seconds")
        expected_calls = len(plan["cases"]) * len(plan["methods"]) * len(plan["protocol"]["modes"]) * 3
        need(receipt.get("validated_calls") == summary.get("formal_calls") == expected_calls,
             "Formal call count is incomplete")
        need(receipt.get("exact_differences") == 0, "Formal results contain correctness differences")
        need(len(summary.get("lanes", [])) == len(plan["lanes"]), "Node completion receipts are incomplete")
        need({lane["node"] for lane in summary["lanes"]} == {lane["node"] for lane in plan["lanes"]},
             "Node identities are incomplete or duplicated")
        for lane in summary["lanes"]:
            path = checked_reference(lane["complete"], self.analysis)
            done = read_json(path)
            need(done.get("complete") is True and done.get("status") == "complete", f"Node is incomplete: {path}")
            checked_reference(done.get("plan"), self.analysis, plan_path)
            exit_path = checked_reference(lane["exit"], self.analysis)
            need(exit_path.read_text().strip() == "0", f"Node exited with a nonzero status: {exit_path}")
        self.rows = []
        seen = set()
        plan_metadata = {(int(c["d"]), Decimal(str(c["p"]))): c for c in plan["cases"]}
        plan_cases = set(plan_metadata)
        need(len(plan_cases) == len(plan["cases"]), "Frozen plan contains duplicate d/p settings")
        for case in summary["cases"]:
            identity = (int(case["d"]), Decimal(str(case["p"])))
            need(identity not in seen, f"Duplicate d/p setting: {identity}")
            seen.add(identity)
            need(set(case["methods"]) == set(plan["methods"]), f"Setting is missing configurations: {case['id']}")
            self.rows.append(dict(case, rounds=plan_metadata.get(identity, {}).get("rounds")))
        need(seen == plan_cases and len(summary["cases"]) == len(plan["cases"]), "The d/p matrix is incomplete")
        distances = set(spec["distances"])
        probabilities = {Decimal(str(p)) for p in spec["probabilities"]}
        need(seen == {(d, p) for d in distances for p in probabilities}, "Data does not cover the confirmed d/p range")
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
        need(result, f"Rule matches no measured settings: {rule}")
        if "distances" in rule:
            need({c["d"] for c in result} == set(rule["distances"]), "Some distances required by the rule were not measured")
        return result

    def time(self, case, alias):
        method = self.aliases[alias]
        need(method in case["methods"], f"Setting {case['id']} does not contain configuration {method}")
        raw = case["methods"][method].get(self.metric)
        need(raw is not None and not isinstance(raw, bool), f"Missing metric: {case['id']} / {method} / {self.metric}")
        value = Decimal(str(raw))
        need(value.is_finite() and value > 0, f"Latency must be finite and positive: {case['id']} / {method}")
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
                     f"Per-round latency requires the setting's frozen positive integer rounds: {case['id']}")
                need("expected_rounds" not in rule or rounds == rule["expected_rounds"], "Frozen rounds differs from the confirmed per-round normalization denominator")
                value /= rounds
            operands.append({"case": case["id"], "method": self.aliases[method], "method_us": denominator,
                             "reference": self.aliases[rule["reference"]] if numerator is not None else None,
                             "reference_us": numerator, "speedup": value if numerator is not None else None,
                             "divide_by_rounds": rounds, "candidate_times_us": candidate_times})
            values.append(value)
        if op in {"time", "time_per_round", "speedup"}:
            need(len(values) == 1, f"Single-setting rule matched multiple settings: {rule}")
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
                reason = "All M4 fields are preserved as specified" if is_m4(item.key) else config["preserve"][item.key]
                audit.append({"key": item.key, "status": "preserved", "value": item.value, "reason": reason})
                continue
            rule = config["rules"][item.key]
            value, operands = matrix.calculate(rule)
            places = len(item.value.partition(".")[2])
            rounded = format(value.quantize(Decimal(1).scaleb(-places), rounding=ROUND_HALF_UP), "f")
            if rule["op"] == "count_faster":
                need(places == 0, f"Setting count must be an integer: {item.key}")
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
    need(old_skeleton == new_skeleton, "Template changed beyond numerical values")
    need({i.key: i.value for i in items if is_m4(i.key)} == {i.key: i.value for i in after if is_m4(i.key)}, "M4 fields changed")
    return output, audit


def atomic_write(path, data):
    path = Path(path)
    need(path.parent.is_dir(), f"Output directory does not exist: {path.parent}")
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
    parser.add_argument("--run", type=Path, help="Override the matrix run directory in the mapping")
    parser.add_argument("--analysis", help="Override the analysis directory name, such as analysis-r02")
    parser.add_argument("--pmu-data", type=Path, help="Use another complete PMU bundle SOURCE.json; all counts and sources are still verified")
    destination = parser.add_mutually_exclusive_group()
    destination.add_argument("--output", type=Path, help="Generate a new TeX file and preserve the template")
    destination.add_argument("--in-place", action="store_true", help="Replace template values and retain a SHA256-identified backup")
    parser.add_argument("--check", action="store_true", help="Validate and compute a preview without writing files")
    parser.add_argument("--audit", type=Path, help="Provenance JSON path; defaults to <output>.audit.json")
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
        print(f"Computed {sum(c['status'] == 'updated' for c in changes)} values; preserved {sum(c['status'] == 'preserved' for c in changes)} values, including all M4 values.")
        if args.check:
            return 0
        need(args.in_place or args.output is not None, "Specify --output or --in-place; use --check for a preview")
        target = args.template.resolve() if args.in_place else args.output.resolve()
        need(args.in_place or target != args.template.resolve(), "Overwriting the template requires --in-place")
        audit_path = args.audit.resolve() if args.audit else target.with_name(target.name + ".audit.json")
        protected = {args.template.resolve(), args.mapping.resolve(), Path(matrix.provenance['summary']['path']),
                     Path(matrix.provenance['completion']['path']), Path(matrix.provenance['plan']['path']),
                     Path(__file__).resolve()}
        if matrix.pmu is not None:
            protected.update(matrix.pmu.paths)
            protected.add(Path(sys.modules[PmuData.__module__].__file__).resolve())
        need(audit_path != target and audit_path not in protected, "Audit path must not overwrite the template, output or data sources")
        need(args.in_place or target not in protected, "Output path must not overwrite data sources or the mapping")
        need(target.parent.is_dir() and audit_path.parent.is_dir(), "Output directory does not exist")
        backup = None
        if args.in_place:
            backup = target.with_name(target.name + ".bak-" + hashlib.sha256(raw).hexdigest()[:16])
            if backup.exists():
                need(backup.read_bytes() == raw, f"Existing backup has different content: {backup}")
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
        print(f"TeX: {target}\nProvenance: {audit_path}")
        return 0
    except (MigrationError, OSError, ValueError, KeyError, TypeError, InvalidOperation) as exc:
        print(f"Migration failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
