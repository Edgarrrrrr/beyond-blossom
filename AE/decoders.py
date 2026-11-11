"""Seven decoder series on three uniform-noise panels.

Each setting has 1,000 independent shots and three native timing repeats.
Beyond blossom selects the smallest three-repeat mean of six configurations.
The bundled CSV retains all twelve measured methods for that selection.
"""
import csv
import itertools
import math
from collections import defaultdict
from pathlib import Path

from common import DATA, configure_style, save_figure
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, LogFormatterMathtext, NullFormatter

DISTANCES = [9, 13, 17, 21, 25, 29, 33]
PROBABILITIES = [0.0005, 0.001, 0.003]
CANDIDATES = ["GridScalar", "GridSIMD", "Predecode", "Component", "TailReset", "Spatial"]
CORES = dict(zip(CANDIDATES, [1, 1, 1, 2, 4, 8]))
SERIES = [
    ("best", "Beyond blossom", "#325A7E", "o", "-"),
    ("Upstream", "Upstream baseline", "#636A73", "s", "--"),
    ("parity", "Parity blossom", "#BF7A49", "^", "-"),
    ("fusion", "Fusion blossom", "#7E64A0", "D", "-"),
    ("unionfind", "UnionFind", "#8B7535", "v", "-."),
    ("parity-uf", "Parity-UF", "#D1A17B", "x", "--"),
    ("ldpc-uf", "LDPC-UF", "#A18BBB", "+", ":"),
]
STYLE = {
    "font.size": 12, "text.color": "#262B33", "axes.labelcolor": "#262B33",
    "axes.edgecolor": "#262B33", "axes.linewidth": 0.65,
    "axes.labelsize": 13, "axes.titlesize": 13,
    "xtick.labelsize": 11.5, "ytick.labelsize": 11.5,
    "xtick.color": "#262B33", "ytick.color": "#262B33",
    "xtick.direction": "in", "ytick.direction": "in",
    "xtick.major.size": 2.8, "ytick.major.size": 2.8,
    "xtick.major.width": 0.6, "ytick.major.width": 0.6,
    "ytick.minor.size": 1.7, "ytick.minor.width": 0.5,
    "axes.axisbelow": True, "grid.color": "#E3E7EB", "grid.linewidth": 0.5,
    "mathtext.fontset": "dejavusans",
}


def _load_data():
    """Check repeat means and the complete paired selection pool."""
    path = DATA / "decoders.csv"
    with path.open(newline="") as stream:
        records = list(csv.DictReader(stream))
    expected_methods = set(CANDIDATES) | {key for key, *_ in SERIES if key != "best"}
    by_setting = defaultdict(dict)
    for row in records:
        for key in ("d", "shots", "repeats"):
            row[key] = int(row[key])
        for key in ("p", "sigma", "mean_us_per_shot",
                    "repeat_1_mean_us", "repeat_2_mean_us", "repeat_3_mean_us"):
            row[key] = float(row[key])
        setting = (row["d"], row["p"], row["sigma"])
        repeats = [row[f"repeat_{i}_mean_us"] for i in (1, 2, 3)]
        valid = (row["shots"] == 1000 and row["repeats"] == 3
                 and row["case"] == f'd{row["d"]:02d}-p{row["p"]:.4f}-s0.00-c00'
                 and all(math.isfinite(value) and value > 0 for value in repeats)
                 and math.isclose(row["mean_us_per_shot"], math.fsum(repeats) / 3,
                                  rel_tol=1e-12)
                 and row["method"] not in by_setting[setting])
        if not valid:
            raise ValueError(f'Invalid repeat or duplicate method: {row["case"]}/{row["method"]}')
        by_setting[setting][row["method"]] = row
    expected_settings = set(itertools.product(DISTANCES, PROBABILITIES, [0.0]))
    if len(records) != 252 or set(by_setting) != expected_settings:
        raise ValueError("Expected all 252 measurements from 21 uniform-noise settings")
    for methods in by_setting.values():
        if set(methods) != expected_methods or len({r["host"] for r in methods.values()}) != 1:
            raise ValueError("Every setting must contain twelve methods measured on one host")
    return by_setting


def _select(by_setting):
    selected, plot_data = [], []
    for (d, p, sigma), methods in sorted(by_setting.items()):
        winner = min(CANDIDATES, key=lambda method: methods[method]["mean_us_per_shot"])
        best = methods[winner]
        selected.append({"case": best["case"], "d": d, "p": p, "sigma": sigma,
            "selected_configuration": winner, "physical_cores": CORES[winner],
            "mean_us_per_shot": best["mean_us_per_shot"], "host": best["host"],
            "source_result": best["source_result"], "source_sha256": best["source_sha256"]})
        for key, label, *_ in SERIES:
            method = winner if key == "best" else key
            row = methods[method]
            plot_data.append({"case": row["case"], "d": d, "p": p, "sigma": sigma,
                "decoder": label, "source_method": method,
                **{field: row[field] for field in (
                    "mean_us_per_shot", "repeat_1_mean_us", "repeat_2_mean_us",
                    "repeat_3_mean_us", "shots", "repeats", "host",
                    "source_result", "source_sha256")}})
    assert len(selected) == 21 and len(plot_data) == 147
    return selected, plot_data


def _write_csv(path, records):
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(records)


def render(output: Path):
    """Validate the bundled data, select configurations, and export the figure."""
    selected, plot_data = _select(_load_data())
    values = {(row["d"], row["p"], row["decoder"]): row["mean_us_per_shot"]
              for row in plot_data}
    configure_style(STYLE)
    fig, axes = plt.subplots(1, 3, figsize=(11.5, 5.8), dpi=170, sharex=True, sharey=True)
    fig.subplots_adjust(left=0.085, right=0.985, bottom=0.14, top=0.78, wspace=0.105)
    handles = {}
    for ax, p in zip(axes, PROBABILITIES):
        ax.set_yscale("log")
        ax.set_ylim(0.5, 1e5)
        ax.set_xlim(7.5, 35.5)
        ax.set_xticks(DISTANCES)
        ax.yaxis.set_major_locator(FixedLocator([1, 10, 100, 1000, 10000, 100000]))
        ax.yaxis.set_major_formatter(LogFormatterMathtext())
        ax.yaxis.set_minor_locator(FixedLocator([]))
        ax.yaxis.set_minor_formatter(NullFormatter())
        ax.tick_params(axis="both", which="both", top=True, right=True)
        ax.tick_params(axis="x", pad=4)
        ax.tick_params(axis="y", pad=4)
        ax.grid(axis="y", which="major")
        for key, label, color, marker, linestyle in SERIES:
            data = [values[d, p, label] for d in DISTANCES]
            assert all(0.5 < value < 1e5 for value in data)
            line, = ax.plot(DISTANCES, data, label=label, color=color,
                linestyle=linestyle, linewidth=1.4, marker=marker, markersize=4.5,
                markerfacecolor=color if key == "best" else "white",
                markeredgewidth=0.8, zorder=4 if key == "best" else 3)
            assert list(line.get_xdata()) == DISTANCES and list(line.get_ydata()) == data
            handles[label] = line
        ax.set_title(f"$p = {p * 100:g}\\%$", pad=8)

    # Column-major legend filling yields four MWPM methods above three UF methods.
    order = ["Beyond blossom", "UnionFind", "Upstream baseline", "Parity-UF",
             "Parity blossom", "LDPC-UF", "Fusion blossom"]
    legend = fig.legend([handles[name] for name in order], order,
        loc="upper center", bbox_to_anchor=(0.525, 0.995), ncol=4, frameon=False,
        fontsize=11.5, handlelength=2.4, handletextpad=0.6, columnspacing=1.7,
        borderaxespad=0, labelspacing=0.8)
    fig.supxlabel("Code distance, $d$", x=0.535, y=0.035, fontsize=13)
    fig.supylabel("Mean native decoding time (μs/shot)", x=0.012, y=0.458, fontsize=12.5)

    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    width, height = fig.canvas.get_width_height()
    texts = list(fig.texts) + list(legend.get_texts())
    for ax in axes:
        texts += list(ax.get_xticklabels()) + list(ax.get_yticklabels()) + [ax.title]
    visible = [text for text in texts if text.get_visible() and text.get_text()]
    boxes = [text.get_window_extent(renderer) for text in visible]
    assert all(0 <= box.x0 <= box.x1 <= width and 0 <= box.y0 <= box.y1 <= height
               for box in boxes), "Figure text extends outside the canvas"
    assert all(not a.overlaps(b) for a, b in itertools.combinations(boxes, 2)), "Figure text overlaps"

    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    _write_csv(output / "selected-configurations.csv", selected)
    _write_csv(output / "plot-data.csv", plot_data)
    save_figure(fig, output, "decoder_comparison")
