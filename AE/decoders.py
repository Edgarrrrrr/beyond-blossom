"""Paper comparison rendering and the separate legacy comparison-data renderer.

``render_paper`` consumes supplied measurements and draws the six paper series.
The legacy ``render`` uses the bundled 1,000-shot data and seven series.
"""
import csv
from decimal import Decimal
import hashlib
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


PAPER_CANDIDATES = ('structured', 'predecoded', 'component-parallel', 'pipelined', 'parallel-predecode')
PAPER_SERIES = (
    ('best', 'Beyond Blossom', '#4D7EA8', 'o'),
    ('reference', 'PyMatching', '#737B86', 's'),
    ('parity', 'Parity Blossom', '#C8786D', '^'),
    ('fusion', 'Fusion Blossom', '#A080AE', 'D'),
    ('unionfind', 'UnionFind', '#5B9889', 'v'),
    ('ldpc-uf', 'LDPC-UF', '#A78840', 'P'),
)


def render_paper(output, native, external, *, profile='paper', paper_points=None):
    """Draw the manuscript comparison layout using explicitly supplied measurements.

    Native/external keys are (distance, Decimal(error_rate), method). Optional
    paper_points uses the same keys for the six displayed series, including
    ``best``; each row must provide its measured mean_us and shots. It never falls
    back to the older 1000-shot comparison data or adds Parity UF to this figure.
    """
    from matplotlib.lines import Line2D
    from plot_fonts import register_paper_fonts

    if profile not in ('paper', 'smoke'):
        raise ValueError('Expected paper or smoke profile')
    if paper_points is None:
        settings = sorted({(int(d), Decimal(str(p))) for d, p, _ in native})
        points = {}
        for d, p in settings:
            winner = min(PAPER_CANDIDATES, key=lambda method: native[d, p, method]['mean_us'])
            points[d, p, 'best'] = native[d, p, winner]
            points[d, p, 'reference'] = native[d, p, 'reference']
            for method, *_ in PAPER_SERIES[2:]:
                points[d, p, method] = external[d, p, method]
    else:
        points = {}
        for (d, p, method), row in paper_points.items():
            if type(d) is not int:
                raise ValueError('Comparison distances must be integers')
            key = (d, Decimal(str(p)), method)
            if key in points:
                raise ValueError('Duplicate comparison point')
            points[key] = row
        if profile == 'paper':
            expected = {(d, Decimal(str(p)), method) for d in DISTANCES
                        for p in PROBABILITIES for method, *_ in PAPER_SERIES}
            if set(points) != expected:
                raise ValueError('Paper comparison requires exactly 21 settings and six series')
        settings = sorted({(d, p) for d, p, _ in points})
    if not settings:
        raise ValueError('No comparison measurements')
    distances = sorted({d for d, _ in settings})
    rates = sorted({p for _, p in settings})
    if any(not p.is_finite() or not 0 < p < 1 for p in rates):
        raise ValueError('Invalid comparison error rate')
    if set(points) != {(d, p, method) for d, p in settings for method, *_ in PAPER_SERIES}:
        raise ValueError('Comparison requires exactly six series per setting')
    if set(settings) != {(d, p) for d in distances for p in rates}:
        raise ValueError('Comparison settings must form a rectangular matrix')
    if min(distances) < 8 or max(distances) > 34:
        raise ValueError('Comparison distances are outside the manuscript axis range')
    for d, p in settings:
        for method, *_ in PAPER_SERIES:
            row = points[d, p, method]
            if (not math.isfinite(row['mean_us']) or not .3 <= row['mean_us'] <= 1e5 or
                    type(row['shots']) is not int or row['shots'] <= 0):
                raise ValueError('Invalid comparison measurement or value outside the manuscript axis range')

    register_paper_fonts()
    plt.rcdefaults()
    plt.rcParams.update({
        'font.family': 'Arial', 'font.size': 18,
        'mathtext.fontset': 'custom', 'mathtext.rm': 'Arial',
        'mathtext.it': 'Arial:italic', 'mathtext.bf': 'Arial:bold',
        'text.color': '#243240', 'axes.labelcolor': '#243240',
        'axes.edgecolor': '#B9C2C8', 'axes.linewidth': .75,
        'axes.labelsize': 26, 'axes.titlesize': 24, 'axes.titleweight': 'bold',
        'xtick.labelsize': 18, 'ytick.labelsize': 18,
        'xtick.color': '#46535E', 'ytick.color': '#46535E',
        'xtick.direction': 'out', 'ytick.direction': 'out',
        'xtick.major.size': 3.5, 'ytick.major.size': 3.5,
        'xtick.major.width': .7, 'ytick.major.width': .7,
        'axes.axisbelow': True, 'grid.color': '#E5E9ED', 'grid.linewidth': .65,
        'pdf.fonttype': 42, 'ps.fonttype': 42, 'svg.fonttype': 'none',
        'figure.facecolor': 'white', 'savefig.facecolor': 'white',
    })
    fig, axes = plt.subplots(1, len(rates), figsize=(14.4, 6.8), dpi=170,
                             sharex=True, sharey=True, squeeze=False)
    fig.subplots_adjust(left=.09, right=.985, bottom=.145, top=.70, wspace=.14)
    legend_handles = {}
    for ax, p in zip(axes[0], rates):
        ax.set_yscale('log')
        ax.set_ylim(.3, 1e5)
        ax.set_xlim(8, 34)
        ax.set_xticks(DISTANCES)
        ax.yaxis.set_major_locator(FixedLocator([1, 10, 100, 1000, 10000, 100000]))
        ax.yaxis.set_major_formatter(LogFormatterMathtext())
        ax.yaxis.set_minor_locator(FixedLocator([]))
        ax.yaxis.set_minor_formatter(NullFormatter())
        ax.spines['top'].set_visible(False)
        ax.spines['right'].set_visible(False)
        ax.tick_params(axis='both', pad=6)
        ax.grid(axis='y', which='major')
        for method, label, color, marker in PAPER_SERIES:
            rows = [points[d, p, method] for d in distances]
            values = [row['mean_us'] for row in rows]
            width = 2.6 if method == 'best' else 2
            zorder = 4 if method == 'best' else 3
            ax.plot(distances, values, color=color, linestyle='-', linewidth=width, zorder=zorder)
            for d, value, row in zip(distances, values, rows):
                ax.plot(d, value, linestyle='none', marker=marker, color=color,
                        markersize=7.5, markeredgewidth=1.35,
                        markerfacecolor=color if row['shots'] == 1000000 else 'white', zorder=zorder, clip_on=False)
            # The paper legend identifies each series; per-point fill records sample count.
            legend_handles[method] = Line2D([], [], color=color, linestyle='-', linewidth=2.6 if method == 'best' else 2.1,
                    marker=marker, markersize=7.5, markeredgewidth=1.35, markerfacecolor=color, label=label)
        ax.set_title(f'p = {float(p) * 100:g}%', pad=18)
        ax.set_xlabel('Code distance (d)', labelpad=9)
    axes[0, 0].set_ylabel('Decoding time (μs)', fontsize=22, fontweight='bold', labelpad=12)
    order = ('best', 'fusion', 'reference', 'unionfind', 'parity', 'ldpc-uf')
    fig.legend([legend_handles[method] for method in order],
               [legend_handles[method].get_label() for method in order],
               loc='upper center', bbox_to_anchor=(.53, .985), ncol=3,
               frameon=False, fontsize=26, handlelength=1.8, handletextpad=.55,
               columnspacing=1.65, borderaxespad=0, labelspacing=.55)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    result = []
    try:
        for extension in ('pdf', 'svg', 'png'):
            path = output / f'decoder_comparison_shows_speedup_v3.{extension}'
            if path.exists():
                raise ValueError('Comparison output already exists: ' + str(path))
            fig.savefig(path, dpi=600 if extension == 'png' else 170)
            result.append({'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    finally:
        plt.close(fig)
    return result


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
