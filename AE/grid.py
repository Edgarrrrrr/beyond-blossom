"""Three structured-grid figures from frozen latency and PMU measurements."""
from pathlib import Path
import csv
import gzip
import itertools
import json
import math

from common import DATA, configure_style, save_figure

import matplotlib.pyplot as plt
from matplotlib.patches import Patch
import numpy as np

SRC = DATA / "grid"
DISTANCES = [9, 13, 17, 21, 25, 29, 33]
PROBABILITIES = [0.0005, 0.001, 0.003]
NAME = "SIMD-Enabled Structured Grid"
COLORS = {"Upstream": "#636A73", "GridSIMD": "#325A7E"}
LABELS = {"Upstream": "Upstream baseline", "GridSIMD": NAME}
P_COLORS = ["#325A7E", "#BF7A49", "#7E64A0"]


def write_csv(output, name, rows):
    with (output / name).open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def read_csv(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def validate_and_save(fig, output, name, checks):
    """Retain the original clipping and text-overlap checks before export."""
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    width, height = fig.canvas.get_width_height()
    texts = list(fig.texts)
    for ax in fig.axes:
        texts += list(ax.get_xticklabels()) + list(ax.get_yticklabels())
        texts += list(ax.texts) + [ax.title, ax.xaxis.label, ax.yaxis.label]
    for legend in fig.legends:
        texts += legend.get_texts() + [legend.get_title()]
    texts = list({
        id(text): text for text in texts if text.get_visible() and text.get_text()
    }.values())
    for text in texts:
        box = text.get_window_extent(renderer)
        assert (
            box.x0 >= 0 and box.y0 >= 0 and box.x1 <= width and box.y1 <= height
        ), (name, text.get_text())
    for first, second in itertools.combinations(texts, 2):
        assert not first.get_window_extent(renderer).overlaps(
            second.get_window_extent(renderer)
        ), (name, first.get_text(), second.get_text())
    checks[name] = {"text_inside_canvas": True, "text_nonoverlapping": True}
    save_figure(fig, output, name)


def render(output: Path):
    """Validate frozen inputs and export three figures and their plotted values."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    checks = {}
    configure_style({
        "font.size": 11.5,
        "text.color": "#262B33",
        "axes.labelcolor": "#262B33",
        "axes.edgecolor": "#262B33",
        "axes.linewidth": 0.7,
        "axes.labelsize": 12.5,
        "axes.titlesize": 12,
        "xtick.labelsize": 11,
        "ytick.labelsize": 11,
        "xtick.color": "#262B33",
        "ytick.color": "#262B33",
        "xtick.direction": "in",
        "ytick.direction": "in",
        "xtick.major.size": 3,
        "ytick.major.size": 3,
        "xtick.major.width": 0.65,
        "ytick.major.width": 0.65,
        "axes.axisbelow": True,
        "grid.color": "#E3E7EB",
        "grid.linewidth": 0.6,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "svg.fonttype": "none",
        "figure.facecolor": "white",
        "savefig.facecolor": "white",
        "mathtext.fontset": "dejavusans",
        "path.simplify": True,
    })

    latency = json.loads((SRC / "clean-latency-summary.json").read_text())
    settings = latency["settings"]
    assert len(settings) == 21
    assert {
        (setting["distance"], setting["error_rate"]) for setting in settings
    } == set(itertools.product(DISTANCES, PROBABILITIES))
    latency_rows = []
    ratios = {}
    for setting in settings:
        assert setting["shots"] == 1000000
        distance, probability = setting["distance"], setting["error_rate"]
        for variant, variant_data in setting["variants"].items():
            assert (
                variant_data["threads"] == 1
                and variant_data["execution"] == "serial"
                and variant_data["decode_entry"] == "normal"
            )
            assert variant_data["min3"]["n"] == 1000000
        upstream_metrics = setting["variants"]["Upstream"]["min3"]
        grid_metrics = setting["variants"]["GridSIMD"]["min3"]
        for metric in ["mean", "p99", "p99_9", "p99_99"]:
            baseline_ns = (
                upstream_metrics["mean_ns"]
                if metric == "mean" else upstream_metrics["quantiles_ns"][metric]
            )
            candidate_ns = (
                grid_metrics["mean_ns"]
                if metric == "mean" else grid_metrics["quantiles_ns"][metric]
            )
            ratios[distance, probability, metric] = baseline_ns / candidate_ns
            latency_rows.append(dict(
                d=distance, p=probability, metric=metric,
                baseline="Upstream baseline", candidate=NAME,
                baseline_us=baseline_ns / 1000, candidate_us=candidate_ns / 1000,
                speedup=baseline_ns / candidate_ns,
                independent_shots=1000000, repeats=3,
                shot_statistic="Min3", loader_condition="Clean",
            ))
    write_csv(output, "grid_latency_values.csv", latency_rows)

    fig, axes = plt.subplots(1, 2, figsize=(9.6, 4.7), dpi=170, sharex=True, sharey=True)
    fig.subplots_adjust(left=0.095, right=0.985, bottom=0.16, top=0.75, wspace=0.12)
    for ax, metric, title in zip(
        axes, ["mean", "p99"], ["(a) Mean latency", "(b) 99th-percentile latency"]
    ):
        for probability, color, marker in zip(PROBABILITIES, P_COLORS, ["o", "s", "^"]):
            ax.plot(
                DISTANCES, [ratios[distance, probability, metric] for distance in DISTANCES],
                color=color, marker=marker, markersize=5, linewidth=1.5, markeredgewidth=0.9,
                markerfacecolor="white", label=f"$p = {probability * 100:g}\\%$",
            )
        ax.axhline(1, color="#636A73", ls=(0, (4, 3)), linewidth=0.9)
        ax.set_xlim(8, 34)
        ax.set_xticks(DISTANCES)
        ax.set_ylim(0.9, 1.55)
        ax.set_yticks([0.9, 1, 1.1, 1.2, 1.3, 1.4, 1.5])
        ax.grid(axis="y")
        ax.set_title(title, pad=10)
        ax.tick_params(top=True, right=True)
    axes[0].set_ylabel("Speedup over Upstream baseline", labelpad=8)
    fig.supxlabel("Code distance, $d$", x=0.54, y=0.035, fontsize=13)
    legend = fig.legend(
        *axes[0].get_legend_handles_labels(), loc="upper center",
        bbox_to_anchor=(0.54, 0.985), ncol=3, frameon=False, columnspacing=2.2,
        title=NAME, title_fontsize=12.5, fontsize=11.5,
    )
    validate_and_save(fig, output, "grid_speedup", checks)

    # PMU totals: use simultaneous denominators within the same event group.
    raw_counts = read_csv(SRC / "pmu-raw-counts.csv")
    lookup = {
        (row["scope"], row["variant"], row["group"], int(row["repeat"]), row["event"]): row
        for row in raw_counts
    }
    assert len(lookup) == len(raw_counts) == 24
    assert all(
        float(row["running_ratio"]) == 1 and int(row["shots"]) == 1000000
        for row in raw_counts
    )
    assert all(
        math.isclose(
            float(row["count"]) / int(row["shots"]),
            float(row["count_per_shot"]), rel_tol=1e-12,
        )
        for row in raw_counts
    )
    published = {
        (row["scope"], row["variant"], row["event"]): row
        for row in read_csv(SRC / "pmu-paper-compact-values.csv")
    }
    specs = [
        (
            "retired_loads", "work7", None, 1e-6,
            "(a) Retired load events", "Million events / shot", 0.9,
        ),
        (
            "retired_l2_miss_loads", "miss7", "retired_loads", 1000,
            "(b) L2 load misses", "Misses / 1,000 load events", 50,
        ),
        (
            "l2_lines_in", "work7", None, 64 / (1024 ** 2),
            "(c) L2 cache-line fills", "Fill payload (MiB / shot)", 5,
        ),
    ]
    pmu_rows = []
    bar_values = {}
    for event, group, denominator_event, factor, *_ in specs:
        for variant in ["Upstream", "GridSIMD"]:
            values = []
            numerators = []
            denominators = []
            for repeat in range(3):
                row = lookup["sequence", variant, group, repeat, event]
                numerator = float(row["count"])
                denominator = (
                    float(lookup["sequence", variant, group, repeat, denominator_event]["count"])
                    if denominator_event else int(row["shots"])
                )
                assert int(row["shots"]) == 1000000
                numerators.append(numerator)
                denominators.append(denominator)
                values.append(numerator / denominator * factor)
            value = sum(numerators) / sum(denominators) * factor
            assert math.isclose(
                value, float(published["sequence", variant, event]["value"]), rel_tol=1e-12
            )
            for repeat in range(3):
                assert math.isclose(
                    values[repeat],
                    float(published["sequence", variant, event][f"round{repeat}"]),
                    rel_tol=1e-12,
                )
            record = dict(
                decoder=LABELS[variant], source_variant=variant, event=event, group=group,
                value=value, round_1=values[0], round_2=values[1], round_3=values[2],
                observed_min=min(values), observed_max=max(values),
            )
            pmu_rows.append(record)
            bar_values[event, variant] = record
    write_csv(output, "grid_cache_locality_values.csv", pmu_rows)

    fig, axes = plt.subplots(1, 3, figsize=(10.5, 4.6), dpi=170)
    fig.subplots_adjust(left=0.075, right=0.985, bottom=0.12, top=0.73, wspace=0.43)
    for ax, spec in zip(axes, specs):
        event, group, denominator_event, factor, title, ylabel, ymax = spec
        for bar_x, variant in enumerate(["Upstream", "GridSIMD"]):
            record = bar_values[event, variant]
            ax.bar(
                bar_x, record["value"], width=0.52, color=COLORS[variant], linewidth=0,
                yerr=[
                    [record["value"] - record["observed_min"]],
                    [record["observed_max"] - record["value"]],
                ],
                error_kw=dict(elinewidth=0.8, capsize=3, capthick=0.8, ecolor="#262B33"),
            )
            ax.annotate(
                f"{record['value']:.2f}", (bar_x, record["observed_max"]), xytext=(0, 5),
                textcoords="offset points", ha="center", va="bottom", fontsize=11.5,
            )
        ax.set_xticks([])
        ax.set_xlim(-0.65, 1.65)
        ax.set_ylim(0, ymax)
        ax.set_ylabel(ylabel, labelpad=7)
        ax.set_title(title, pad=10)
        ax.grid(axis="y")
        ax.tick_params(right=True)
    fig.legend(
        handles=[Patch(color=COLORS[variant], label=LABELS[variant])
                 for variant in ["Upstream", "GridSIMD"]],
        loc="upper center", bbox_to_anchor=(0.53, 0.995), ncol=2, frameon=False,
        fontsize=11.5, columnspacing=2,
    )
    fig.text(0.53, 0.855, "$d = 29$, $p = 0.3\\%$", ha="center", va="center", fontsize=12)
    validate_and_save(fig, output, "grid_cache_locality", checks)

    # Exact empirical survival function for the same million Clean Min3 samples.
    target = next(
        setting for setting in settings
        if setting["distance"] == 29 and setting["error_rate"] == 0.003
    )
    tail_checks = {}
    tail_quantiles = []
    fig, ax = plt.subplots(figsize=(8.5, 4.8), dpi=170)
    fig.subplots_adjust(left=0.115, right=0.98, bottom=0.16, top=0.76)
    for variant in ["Upstream", "GridSIMD"]:
        with gzip.open(SRC / f"{variant}-min3-ecdf.csv.gz", "rt") as stream:
            rows = list(csv.DictReader(stream))
        latencies_ns = np.array([int(row["latency_ns"]) for row in rows], dtype=np.int64)
        counts = np.array([int(row["count"]) for row in rows], dtype=np.int64)
        cumulative_counts = np.array(
            [int(row["cumulative_count"]) for row in rows], dtype=np.int64
        )
        greater_counts = np.array(
            [int(row["strict_greater_count"]) for row in rows], dtype=np.int64
        )
        assert counts.sum() == 1000000 and np.all(counts > 0) and np.all(np.diff(latencies_ns) > 0)
        assert np.array_equal(cumulative_counts, np.cumsum(counts))
        assert np.array_equal(greater_counts, 1000000 - cumulative_counts)
        metrics = target["variants"][variant]["min3"]
        assert math.isclose(
            float(np.dot(latencies_ns, counts)) / 1000000, metrics["mean_ns"], abs_tol=1e-8
        )
        assert latencies_ns[-1] == metrics["max_ns"]
        for key, probability in [
            ("p50", 0.5), ("p99", 0.99), ("p99_9", 0.999), ("p99_99", 0.9999)
        ]:
            quantile = int(latencies_ns[
                np.searchsorted(cumulative_counts, math.ceil(probability * 1000000))
            ])
            assert quantile == metrics["quantiles_ns"][key]
            tail_quantiles.append(dict(
                decoder=LABELS[variant], quantile=key, latency_us=quantile / 1000
            ))
        ax.step(
            latencies_ns / 1000, greater_counts / 1000000, where="post",
            color=COLORS[variant], linewidth=1.5, label=LABELS[variant],
        )
        tail_checks[variant] = dict(
            input_count=1000000, unique_latency_values=len(rows),
            mean_and_max_and_quantiles_recomputed=True, strict_greater_counts_verified=True,
            full_ecdf_plotted=True, zero_probability_below_log_axis=True,
        )
    write_csv(output, "grid_tail_quantiles.csv", tail_quantiles)
    ax.set_yscale("log", nonpositive="clip")
    ax.set_ylim(8e-7, 1.1)
    ax.set_xlim(300, 2050)
    ax.set_xticks([400, 800, 1200, 1600, 2000])
    ax.set_yticks([1, 1e-1, 1e-2, 1e-3, 1e-4, 1e-5, 1e-6])
    ax.set_xlabel("Decoding time (μs)", labelpad=9)
    ax.set_ylabel("Tail probability, $P(L > t)$", labelpad=9)
    ax.grid(axis="y", which="major")
    ax.tick_params(top=True, right=True)
    fig.legend(
        *ax.get_legend_handles_labels(), loc="upper center", bbox_to_anchor=(0.54, 0.99),
        ncol=2, frameon=False, fontsize=11.5, columnspacing=1.6,
    )
    fig.text(0.54, 0.845, "$d = 29$, $p = 0.3\\%$", ha="center", va="center", fontsize=12)
    validate_and_save(fig, output, "grid_tail_latency", checks)

    checks.update(all_pmu_bars_and_rounds_recomputed=True, tail_verification=tail_checks)
    (output / "grid_validation.json").write_text(json.dumps(checks, indent=2) + "\n")
