#!/usr/bin/env python3
"""Reproduce the Apple M4 Pro comparison from the linked Feishu CSV.

No benchmark is executed. Each Beyond Blossom point is the fastest of the
five measured optimized configurations for that (distance, error rate).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
from collections import defaultdict
from decimal import Decimal
from pathlib import Path
from statistics import median

os.environ.setdefault("MPLCONFIGDIR", "/tmp/beyond-blossom-mpl")

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.ticker import FixedLocator, FuncFormatter, LogLocator, NullFormatter


ROOT = Path(__file__).resolve().parent
DISTANCES = (9, 13, 17, 21, 25, 29, 33)
ERROR_RATES = (Decimal("0.0005"), Decimal("0.001"), Decimal("0.003"))
ROLES = ("upstream", "scalar", "predecode", "component", "tailreset", "spatial")
FIGURE_SIZE = (3.5, 2.1)
SOURCE_URL = "https://wcncbam07gf8.feishu.cn/wiki/McsNwrj64iyHVrkbSHlc4SkAn3d"
FIELDS = (
    "method", "distance", "measurement_rounds", "error_rate", "error_rate_percent",
    "block_time_us", "time_us_per_round", "selected_role", "actual_threads",
    "implementation_id", "implementation_commit", "variant_id", "binary_sha256",
    "input_sha256", "source_analysis_id", "origin", "batch_aggregation",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def percent_label(value: Decimal) -> str:
    return f"{format(value * 100, 'f').rstrip('0').rstrip('.')}%"


def load_points(source: Path) -> list[dict[str, str | int]]:
    with source.open(newline="", encoding="utf-8-sig") as stream:
        rows = list(csv.DictReader(stream))
    groups: dict[tuple[int, Decimal], dict[str, dict[str, str]]] = defaultdict(dict)
    for row in rows:
        key = (int(row["distance"]), Decimal(row["error_rate"]))
        role = row["role"]
        if role not in ROLES or role in groups[key]:
            raise ValueError(f"Unexpected or duplicate source role: {key}, {role}")
        if row["batch_aggregation"] != "median_of_three_complete_sequence_ns_per_shot":
            raise ValueError(f"Unexpected aggregation: {key}, {role}")
        time_us = float(row["time_us"])
        expected_us = median(float(row[f"repeat{i}_ns"]) for i in range(3)) / 1000
        if not math.isfinite(time_us) or time_us <= 0:
            raise ValueError(f"Nonpositive or nonfinite time: {key}, {role}")
        if not math.isclose(time_us, expected_us, rel_tol=1e-12, abs_tol=1e-10):
            raise ValueError(f"Source median mismatch: {key}, {role}")
        groups[key][role] = row

    expected = {(d, p) for d in DISTANCES for p in ERROR_RATES}
    if set(groups) != expected or len(rows) != len(expected) * len(ROLES):
        raise ValueError("Source must cover exactly 7 distances x 3 error rates x 6 roles")

    points = []
    for p in ERROR_RATES:
        for d in DISTANCES:
            group = groups[d, p]
            if set(group) != set(ROLES):
                raise ValueError(f"Incomplete roles: d={d}, p={p}")
            if len({r["input_sha256"] for r in group.values()}) != 1:
                raise ValueError(f"Input hashes differ across roles: d={d}, p={p}")
            best = min((group[role] for role in ROLES[1:]),
                       key=lambda row: Decimal(row["time_us"]))
            for method, row in (("Upstream baseline", group["upstream"]),
                                ("Beyond Blossom", best)):
                block_time = Decimal(row["time_us"])
                points.append({
                    "method": method,
                    "distance": d,
                    "measurement_rounds": d,
                    "error_rate": str(p),
                    "error_rate_percent": percent_label(p),
                    "block_time_us": str(block_time),
                    "time_us_per_round": str(block_time / d),
                    "selected_role": row["role"],
                    **{key: row[key] for key in FIELDS[8:]},
                })
    return points


def plot(points: list[dict], palette: dict, target: Path, yscale: str) -> None:
    colors = palette["color_tokens"]
    neutral = palette["neutrals"]
    plt.rcParams.update({
        "font.family": "DejaVu Sans", "font.size": 8,
        "mathtext.fontset": "dejavusans", "text.color": neutral["text"],
        "axes.labelsize": 8.5, "axes.labelcolor": neutral["text"],
        "axes.edgecolor": neutral["secondary_text"], "axes.linewidth": 0.65,
        "axes.facecolor": neutral["background"],
        "axes.spines.top": False, "axes.spines.right": False,
        "xtick.color": neutral["secondary_text"],
        "ytick.color": neutral["secondary_text"],
        "xtick.labelsize": 7.5, "ytick.labelsize": 7.5,
        "figure.facecolor": neutral["background"],
        "savefig.facecolor": neutral["background"],
        "pdf.fonttype": 42, "ps.fonttype": 42, "svg.fonttype": "none",
    })
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    fig.subplots_adjust(left=0.15, right=0.985, bottom=0.185, top=0.80)
    for method in ("Upstream baseline", "Beyond Blossom"):
        for p, color_name, marker in zip(ERROR_RATES,
                                         ("BLUE", "TEAL", "ORANGE"),
                                         ("o", "s", "^")):
            series = sorted((row for row in points
                             if row["method"] == method
                             and Decimal(row["error_rate"]) == p),
                            key=lambda row: row["distance"])
            beyond = method == "Beyond Blossom"
            ax.plot(
                [row["distance"] for row in series],
                [float(row["time_us_per_round"]) for row in series],
                color=colors[color_name], linewidth=1.15,
                linestyle="-" if beyond else (0, (3.2, 1.8)),
                marker=marker, markersize=3.5, markeredgewidth=0.75,
                markerfacecolor=colors[color_name] if beyond else neutral["background"],
                label=f"{method}, {percent_label(p)}", zorder=3 if beyond else 4,
            )
    ax.set_yscale(yscale)
    if yscale == "log":
        ax.set_ylim(0.012, 15)
        ax.yaxis.set_major_locator(FixedLocator([0.01, 0.1, 1, 10]))
        ax.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:g}"))
        ax.yaxis.set_minor_locator(LogLocator(base=10, subs=(2, 5)))
        ax.yaxis.set_minor_formatter(NullFormatter())
    else:
        ymax = max(float(row["time_us_per_round"]) for row in points)
        ax.set_ylim(0, ymax * 1.13)
    ax.set_xlim(8, 34)
    ax.set_xticks(DISTANCES)
    ax.set_xlabel(r"Code distance, $d$")
    ax.set_ylabel(r"Time ($\mu$s/round)", labelpad=3)
    ax.tick_params(axis="both", which="major", pad=2, length=3)
    ax.grid(axis="y", which="major", color=neutral["grid"], linewidth=0.55)
    ax.axhline(1, color=neutral["text"], linestyle=(0, (1.2, 2)),
               linewidth=0.95, zorder=2)
    ax.annotate(r"1 $\mu$s/round", xy=(9, 1), xytext=(0, 7),
                textcoords="offset points", ha="left", va="bottom",
                fontsize=7.2, color=neutral["text"])
    # Separate the two independent encodings, avoiding six repeated labels.
    method_handles = [
        Line2D([], [], color=neutral["text"], linewidth=1.15,
               linestyle=(0, (3.2, 1.8)), marker="o", markersize=3.5,
               markerfacecolor=neutral["background"], markeredgewidth=0.75,
               label="Upstream baseline"),
        Line2D([], [], color=neutral["text"], linewidth=1.15,
               marker="o", markersize=3.5, markeredgewidth=0.75,
               label="Beyond Blossom"),
    ]
    probability_handles = [
        Line2D([], [], color=colors[name], linestyle="none", marker=marker,
               markersize=3.5, markeredgewidth=0.75,
               label=f"$p$ = {percent_label(p)}")
        for p, name, marker in zip(ERROR_RATES,
                                   ("BLUE", "TEAL", "ORANGE"),
                                   ("o", "s", "^"))
    ]
    method_legend = fig.legend(
        handles=method_handles, loc="upper center", bbox_to_anchor=(0.56, 0.99),
        ncol=2, frameon=False, fontsize=7.2, borderaxespad=0,
        columnspacing=1.6, handlelength=2.0, handletextpad=0.6)
    probability_legend = fig.legend(
        handles=probability_handles, loc="upper center", bbox_to_anchor=(0.56, 0.907),
        ncol=3, frameon=False, fontsize=7.2, borderaxespad=0,
        columnspacing=1.4, handlelength=0.8, handletextpad=0.4)
    fig.canvas.draw()
    # Fail before exporting if the legend or axis labels leave the fixed canvas.
    renderer = fig.canvas.get_renderer()
    for artist in (method_legend, probability_legend, ax.xaxis.label, ax.yaxis.label):
        box = artist.get_window_extent(renderer).transformed(fig.transFigure.inverted())
        if box.x0 < 0 or box.y0 < 0 or box.x1 > 1 or box.y1 > 1:
            raise ValueError(f"Chart label leaves canvas: {box}")
    for suffix in ("pdf", "svg", "png"):
        fig.savefig(target / f"m4_pro_per_round.{suffix}", dpi=300)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=ROOT / "data/feishu_m4_six_roles.csv")
    parser.add_argument("--palette", type=Path,
                        default=ROOT / "palette.json")
    parser.add_argument("--output-dir", type=Path,
                        default=ROOT / "figures")
    parser.add_argument("--yscale", choices=("log", "linear"), default="log")
    args = parser.parse_args()
    points = load_points(args.source)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with (args.output_dir / "m4_pro_per_round.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(points)
    plot(points, json.loads(args.palette.read_text()), args.output_dir, args.yscale)
    counts = []
    for method in ("Upstream baseline", "Beyond Blossom"):
        for p in ERROR_RATES:
            group = [row for row in points if row["method"] == method
                     and Decimal(row["error_rate"]) == p]
            passing = [row["distance"] for row in group
                       if Decimal(row["time_us_per_round"]) <= 1]
            counts.append({"method": method, "error_rate": str(p),
                           "distances_at_or_below_1_us_per_round": passing})
    summary = {
        "source_url": SOURCE_URL,
        "source_csv_sha256": sha256(args.source),
        "palette_sha256": sha256(args.palette),
        "matplotlib_version": matplotlib.__version__,
        "source_rows": 126, "plotted_points": len(points),
        "decoding_curves": 6, "measurement_rounds": "d",
        "aggregation": "median of three complete-batch mean times per shot",
        "beyond_blossom_selection": "minimum measured time_us among scalar, predecode, component, tailreset, spatial for each (d,p)",
        "formula": "time_us_per_round = source.time_us / distance",
        "yscale": args.yscale, "figure_inches": list(FIGURE_SIZE),
        "legend_layout": "two rows: method line/fill styles; error-rate colors/shapes",
        "threshold_us_per_round": 1, "threshold_counts": counts,
        "source_caveats": [
            "TailReset/Spatial use new component-chain measurements; the first four roles retain the parent acquisition window.",
            "Best-of-configurations points are a descriptive envelope, not a measured adaptive selector.",
            "Block time divided by d is an average processing budget; it does not establish a per-shot latency bound.",
        ],
    }
    per_p = []
    for p in ERROR_RATES:
        groups = {d: {row["method"]: row for row in points
                      if row["distance"] == d and Decimal(row["error_rate"]) == p}
                  for d in DISTANCES}
        ratios = {d: float(group["Upstream baseline"]["block_time_us"])
                  / float(group["Beyond Blossom"]["block_time_us"])
                  for d, group in groups.items()}
        best_d = max(ratios, key=ratios.get)
        per_p.append({
            "error_rate": str(p),
            "max_speedup_vs_reference": ratios[best_d],
            "distance_at_max_speedup": best_d,
            "d33_block_time_us": float(groups[33]["Beyond Blossom"]["block_time_us"]),
            "d33_time_us_per_round": float(groups[33]["Beyond Blossom"]["time_us_per_round"]),
        })
    summary["paper_statistics"] = per_p
    (args.output_dir / "plot_summary.json").write_text(
        json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"Validated 126 source rows; exported six curves and {len(points)} points to {args.output_dir}")


if __name__ == "__main__":
    main()
