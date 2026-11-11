#!/usr/bin/env python3
"""Render a source-backed, single-image performance experiment summary.

Usage (from any working directory):
  python /path/to/render_summary.py /path/to/summary.json --output /path/to/summary

Writes .png, .svg and .derived.json; add --pdf for PDF. Input paths and the
output prefix are resolved against the caller's working directory. Source
references may be absolute paths or relative to the input JSON. A source SHA
is checked when the local file exists; --require-local-sources requires every
reference to resolve locally. No network requests are made.

Only latency / lower_is_better is supported. Speedup = baseline / candidate.
Input must contain all measured cases; for >10 cases supply display_case_ids
explicitly. Geomean and win/loss counts always use every supplied case. This
renderer verifies representation and arithmetic, not experimental validity or
arbitrary prose claims; use the experiment validator and review the sources.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile


class SummaryError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise SummaryError(message)


def text(value, name, limit=300):
    require(isinstance(value, str) and value.strip(), f"{name}: expected nonempty text")
    require(len(value) <= limit, f"{name}: exceeds {limit} characters; shorten the card text")
    return value.strip()


def positive(value, name):
    require(type(value) in (int, float), f"{name}: expected a finite positive number")
    try:
        number = float(value)
    except OverflowError as exc:
        raise SummaryError(f"{name}: number is too large") from exc
    require(math.isfinite(number) and number > 0, f"{name}: expected a finite positive number")
    return number


def validate(data, input_path, require_local=False):
    require(isinstance(data, dict), "summary: expected a JSON object")
    require(type(data.get("schema_version")) is int and data["schema_version"] == 1,
            "schema_version must be 1")
    for name, limit in (("experiment_id", 90), ("title", 48), ("baseline_label", 45),
                        ("candidate_label", 45), ("decision", 180)):
        text(data.get(name), name, limit)
    require(data.get("status") in ("final", "historical_snapshot"),
            "status must be final or historical_snapshot; unmeasured templates cannot be rendered")
    if data["status"] == "historical_snapshot":
        text(data.get("snapshot_notice"), "snapshot_notice", 120)
    scope = data.get("scope")
    require(isinstance(scope, dict), "scope: expected an object")
    require(scope.get("metric") == "latency" and scope.get("direction") == "lower_is_better",
            "scope must specify metric=latency and direction=lower_is_better")
    for name, limit in (("workload", 180), ("measurement", 180), ("environment", 160),
                        ("coverage", 100)):
        text(scope.get(name), f"scope.{name}", limit)

    changes = data.get("changes")
    require(isinstance(changes, list) and 1 <= len(changes) <= 3,
            "changes must contain 1–3 implementation changes")
    for i, change in enumerate(changes):
        require(isinstance(change, dict), f"changes[{i}]: expected an object")
        for name, limit in (("before", 65), ("after", 65), ("mechanism", 155)):
            text(change.get(name), f"changes[{i}].{name}", limit)

    cases = data.get("cases")
    require(isinstance(cases, list) and cases, "cases: measured data is required")
    require(type(scope.get("total_cases")) is int and scope["total_cases"] == len(cases),
            "scope.total_cases must equal len(cases); provide every measured case")
    ids = set()
    rows = []
    for i, case in enumerate(cases):
        require(isinstance(case, dict), f"cases[{i}]: expected an object")
        cid = text(case.get("case_id"), f"cases[{i}].case_id", 100)
        require(cid not in ids, f"duplicate case_id: {cid}")
        ids.add(cid)
        label = text(case.get("label"), f"cases[{i}].label", 28)
        unit = text(case.get("unit"), f"cases[{i}].unit", 15)
        baseline = positive(case.get("baseline"), f"cases[{i}].baseline")
        candidate = positive(case.get("candidate"), f"cases[{i}].candidate")
        ratio = baseline / candidate
        require(math.isfinite(ratio) and ratio > 0, f"cases[{i}]: unrepresentable ratio")
        rows.append(dict(case_id=cid, label=label, baseline=baseline, candidate=candidate,
                         unit=unit, speedup=ratio, time_delta_pct=(candidate / baseline - 1) * 100))
    require(len({r["unit"] for r in rows}) == 1, "all cases must use the same latency unit")
    display_ids = data.get("display_case_ids")
    if display_ids is None:
        require(len(cases) <= 10, "more than 10 cases: supply explicit display_case_ids (1–10)")
        display_ids = [r["case_id"] for r in rows]
    require(isinstance(display_ids, list) and 1 <= len(display_ids) <= 10 and
            all(isinstance(cid, str) for cid in display_ids), "display_case_ids must contain 1–10 IDs")
    require(len(set(display_ids)) == len(display_ids) and set(display_ids) <= ids,
            "display_case_ids must be unique existing case IDs")
    if len(display_ids) != len(cases):
        text(data.get("display_selection_reason"), "display_selection_reason", 150)

    metrics = data.get("metrics", [])
    require(isinstance(metrics, list) and len(metrics) <= 2, "metrics: at most two additional metrics")
    for i, metric in enumerate(metrics):
        require(isinstance(metric, dict), f"metrics[{i}]: expected an object")
        for name, limit in (("label", 24), ("value", 30), ("scope", 90)):
            text(metric.get(name), f"metrics[{i}].{name}", limit)
    limitations = data.get("limitations")
    require(isinstance(limitations, list) and 1 <= len(limitations) <= 4,
            "limitations: provide 1–4 material boundaries")
    for i, limitation in enumerate(limitations):
        text(limitation, f"limitations[{i}]", 220)
    refs = data.get("source_refs")
    require(isinstance(refs, list) and 1 <= len(refs) <= 6,
            "source_refs: provide 1–6 source references with SHA256")
    checked_refs = []
    for i, ref in enumerate(refs):
        require(isinstance(ref, dict), f"source_refs[{i}]: expected an object")
        label = text(ref.get("label"), f"source_refs[{i}].label", 65)
        path_text = text(ref.get("path"), f"source_refs[{i}].path", 600)
        sha = ref.get("sha256", "")
        require(isinstance(sha, str) and re.fullmatch(r"[0-9a-f]{64}", sha),
                f"source_refs[{i}].sha256: expected lowercase SHA256")
        source = Path(path_text).expanduser()
        if not source.is_absolute():
            source = input_path.parent / source
        exists = source.is_file()
        require(not require_local or exists, f"source not locally available: {source}")
        if exists:
            require(hashlib.sha256(source.read_bytes()).hexdigest() == sha,
                    f"source SHA256 mismatch: {source}")
        checked_refs.append(dict(label=label, path=path_text, sha256=sha, verified_local=exists))
    speeds = [r["speedup"] for r in rows]
    wins = sum(r["candidate"] < r["baseline"] for r in rows)
    ties = sum(r["candidate"] == r["baseline"] for r in rows)
    return dict(case_count=len(rows), display_case_ids=display_ids,
                display_case_count=len(display_ids), cases=rows, source_refs=checked_refs,
                geometric_mean_speedup=math.exp(sum(math.log(v) for v in speeds) / len(speeds)),
                wins=wins, ties=ties, losses=len(rows)-wins-ties,
                min_speedup=min(speeds), max_speedup=max(speeds),
                formula="speedup = baseline latency / candidate latency (>1 is better)",
                aggregation="equal-weight geometric mean of case speedups; descriptive, not significance")


def render(data, derived, output, font_path=None, pdf=False):
    # Keep font caches out of the caller's home directory. No runtime installs.
    os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "experiment-summary-mpl"))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib import font_manager
        from matplotlib.patches import FancyBboxPatch
    except ImportError as exc:
        raise SummaryError("rendering requires matplotlib; run with the experiment's Python environment") from exc

    if font_path:
        require(Path(font_path).is_file(), f"font file missing: {font_path}")
        font = font_manager.FontProperties(fname=str(Path(font_path).resolve()))
    else:
        families = ("Noto Sans CJK SC", "Noto Sans CJK JP", "WenQuanYi Zen Hei",
                    "Microsoft YaHei", "PingFang SC", "Arial Unicode MS")
        available = {f.name: f.fname for f in font_manager.fontManager.ttflist}
        found = next((available[n] for n in families if n in available), None)
        require(found is not None, "a CJK font is required; install one or pass --font /path/font.ttf")
        font = font_manager.FontProperties(fname=found)
    plt.rcParams.update({"font.family": font.get_name(), "axes.unicode_minus": False,
                         "svg.fonttype": "path", "pdf.fonttype": 42})
    fig = plt.figure(figsize=(18, 13), dpi=150, facecolor="#FFFFFF")
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    ink, muted, blue, orange = "#182330", "#526071", "#1765A1", "#AB5E23"
    tracked_text = []

    def width(s, size):
        prop = font.copy()
        prop.set_size(size)
        return renderer.get_text_width_height_descent(s, prop, False)[0] / fig.bbox.width

    def wrap(s, size, available_width):
        lines = []
        for paragraph in s.split("\n"):
            line = ""
            for char in paragraph:
                if line and width(line + char, size) > available_width:
                    lines.append(line)
                    line = char
                else:
                    line += char
            lines.append(line)
        return "\n".join(lines)

    def put(s, x, y, w, h, size=12, color=ink, weight="normal", name="text"):
        wrapped = wrap(str(s), size, w)
        item = fig.text(x, y, wrapped, fontproperties=font, fontsize=size, color=color,
                        weight=weight, va="top", ha="left", linespacing=1.45)
        tracked_text.append((item, x, y, w, h, name))
        return item

    def panel(x, y, w, h, color="#F4F7FA"):
        fig.add_artist(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.008,rounding_size=0.008",
                                     linewidth=0, facecolor=color, transform=fig.transFigure, zorder=0))

    status = "历史实测快照 · 非新 commit 合规实验" if data["status"] == "historical_snapshot" else "实验结果总览"
    put(status, .035, .975, .92, .023, 11, blue, name="status")
    put(data["title"], .035, .94, .93, .05, 25, name="title")
    put(f'{data["experiment_id"]}   |   {data["baseline_label"]} → {data["candidate_label"]}',
        .035, .885, .93, .033, 12, muted, name="identity")
    scope = data["scope"]
    put(scope["workload"] + "\n" + scope["measurement"] + "  |  " + scope["environment"],
        .035, .843, .93, .068, 10.5, muted, name="scope")

    n = derived["case_count"]
    cards = [dict(label="等权几何平均加速比", value=f'{derived["geometric_mean_speedup"]:.3f}×',
                  scope=f"全部 {n} 配置；不代表生产负载权重"),
             dict(label="均值更快的配置", value=f'{derived["wins"]} / {n}',
                  scope=f'{derived["losses"]} 更慢，{derived["ties"]} 相等；非显著性判定'),
             dict(label="加速比范围", value=f'{derived["min_speedup"]:.3f}–{derived["max_speedup"]:.3f}×',
                  scope="baseline / candidate；大于 1 更好")] + data.get("metrics", [])
    gap = .016
    card_w = (.93 - gap * (len(cards) - 1)) / len(cards)
    for i, card in enumerate(cards):
        x = .035 + i * (card_w + gap)
        panel(x, .642, card_w, .111)
        put(card["label"], x + .009, .739, card_w - .018, .021, 10.5, muted, name=f"metric-{i}-label")
        put(card["value"], x + .009, .709, card_w - .018, .03, 20, blue, name=f"metric-{i}-value")
        put(card["scope"], x + .009, .671, card_w - .018, .033, 9, muted, name=f"metric-{i}-scope")

    put("优化方法", .035, .608, .45, .028, 16, name="methods-heading")
    put("实测效果", .535, .608, .43, .028, 16, name="effects-heading")
    method_h = .266 / len(data["changes"])
    for i, change in enumerate(data["changes"]):
        top = .562 - i * method_h
        put(f'{i+1:02d}  {change["before"]} → {change["after"]}', .035, top, .448,
            method_h * .41, 12, blue, name=f"method-{i}-transition")
        put(change["mechanism"], .035, top - method_h * .44, .448, method_h * .52,
            11, muted, name=f"method-{i}-mechanism")

    selected = {r["case_id"]: r for r in derived["cases"]}
    rows = [selected[cid] for cid in derived["display_case_ids"]]
    ax = fig.add_axes([.644, .305, .271, .261])
    speeds = [r["speedup"] for r in rows]
    low, high = min(1, min(speeds)), max(1, max(speeds))
    span = max(.1, high-low)
    ax.set_xlim(max(0, low-span*.25), high+span*.30)
    ax.set_ylim(len(rows)-.5, -.7)
    ax.axvline(1, color="#75818B", linewidth=1)
    chart_labels = []
    for y, row in enumerate(rows):
        color = blue if row["speedup"] >= 1 else orange
        ax.plot([1, row["speedup"]], [y, y], linewidth=1.6, color=color)
        ax.scatter(row["speedup"], y, s=43, color=color, marker="o" if row["speedup"] >= 1 else "D", zorder=3)
        positive_side = row["speedup"] >= 1
        chart_labels.append(ax.annotate(f'{row["speedup"]:.3f}×', (row["speedup"], y),
                    xytext=(7 if positive_side else -7, 0), textcoords="offset points",
                    ha="left" if positive_side else "right", va="center", fontsize=9.5,
                    color=color, bbox=dict(facecolor="white", edgecolor="none", pad=.4)))
    ax.set_yticks(range(len(rows)), [r["label"] for r in rows], fontsize=10)
    ax.tick_params(axis="y", length=0, pad=10)
    ax.tick_params(axis="x", labelsize=10, color="#ADB5BD")
    ax.set_xlabel("加速比 = baseline 耗时 / candidate 耗时（>1 更好）", fontsize=10, labelpad=7)
    ax.spines[["top", "right", "left"]].set_visible(False)
    ax.spines["bottom"].set_color("#ADB5BD")
    ax.grid(axis="x", color="#E7EAED", linewidth=.6)
    ax.set_axisbelow(True)
    coverage = f'展示 {len(rows)}/{n} 配置；{scope["coverage"]}'
    if len(rows) != n:
        coverage += "；其余配置计入上方汇总，完整数值见 JSON。" + data["display_selection_reason"]
    put(coverage, .535, .260, .43, .035, 9, muted, name="chart-coverage")

    panel(.035, .174, .93, .051, "#EDF4F9")
    put("结论 / 采用策略", .044, .213, .12, .033, 10.5, blue, name="decision-label")
    put(data["decision"], .172, .214, .78, .041, 11.5, ink, name="decision")
    limits = "\n".join("• " + s for s in data["limitations"])
    if data["status"] == "historical_snapshot":
        limits += "\n• " + data["snapshot_notice"]
    put(limits, .035, .153, .93, .085, 9.5, muted, name="limitations")
    source_lines = []
    for i, ref in enumerate(derived["source_refs"], 1):
        verification = "已核对本地文件" if ref["verified_local"] else "本机未核对原文件"
        source_lines.append(f'[{i}] {ref["label"]} · SHA256 {ref["sha256"][:12]}… · {verification}')
    source_lines.append("原始数值、完整路径与哈希见同名 .derived.json；图中文字解释须与完整技术报告一起阅读。")
    put("\n".join(source_lines), .035, .055, .93, .05, 8, muted, name="sources")

    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    for item, x, y, w, h, name in tracked_text:
        box = item.get_window_extent(renderer).transformed(fig.transFigure.inverted())
        require(box.width <= w + .002 and box.height <= h + .002 and
                box.x0 >= x-.002 and box.x1 <= x+w+.002 and box.y0 >= y-h-.002,
                f"text overflow in {name}; shorten that field (text is never silently truncated)")
    for item in chart_labels + ax.get_yticklabels() + ax.get_xticklabels() + [ax.xaxis.label]:
        box = item.get_window_extent(renderer).transformed(fig.transFigure.inverted())
        require(box.x0 >= .529 and box.x1 <= .973 and box.y0 >= .263 and box.y1 <= .581,
                "chart label overflow; shorten case labels or use a supplementary chart")
    output.parent.mkdir(parents=True, exist_ok=True)
    formats = ["png", "svg"] + (["pdf"] if pdf else [])
    paths = []
    for extension in formats:
        target = Path(str(output) + "." + extension)
        fig.savefig(target, dpi=150, facecolor="white", metadata={"Creator": "performance-experiment/render_summary.py"})
        paths.append(str(target))
    plt.close(fig)
    return paths


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", type=Path, help="summary JSON")
    parser.add_argument("--output", type=Path, help="output file prefix (default: beside input, summary)")
    parser.add_argument("--font", help="explicit path to a font supporting Chinese")
    parser.add_argument("--pdf", action="store_true", help="also export PDF")
    parser.add_argument("--require-local-sources", action="store_true", help="require source files and check all SHA256 values")
    parser.add_argument("--validate-only", action="store_true", help="check schema, arithmetic and sources without rendering")
    args = parser.parse_args()
    try:
        source = args.input.expanduser().resolve()
        data = json.loads(source.read_text(encoding="utf-8"))
        derived = validate(data, source, args.require_local_sources)
        derived.update(experiment_id=data["experiment_id"], status=data["status"],
                       input_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
        if args.validate_only:
            print(json.dumps(derived, ensure_ascii=False, indent=2, allow_nan=False))
            return 0
        output = (args.output.expanduser().resolve() if args.output else source.parent / "summary")
        paths = render(data, derived, output, args.font, args.pdf)
        target = Path(str(output) + ".derived.json")
        target.write_text(json.dumps(derived, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        print(json.dumps(dict(status="rendered", artifacts=paths+[str(target)],
                              cases=derived["case_count"], displayed=derived["display_case_count"]), ensure_ascii=False))
        return 0
    except (SummaryError, OSError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
