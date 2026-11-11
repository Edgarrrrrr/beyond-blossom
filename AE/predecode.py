"""Grouped bars for predecoder residual syndromes and full decoding time."""
import csv
import json
import math
from pathlib import Path

from common import DATA, configure_style, save_figure
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

DISTANCES = [9, 13, 17, 21, 25, 29, 33]
SERIES = [
    ("skip", "No predecoder", "#325A7E", "initial_mean"),
    ("coordinate", "Coordinate-ordered predecoder", "#BF7A49", "residual_coordinate"),
    ("current", "Degree-ordered predecoder", "#7E64A0", "residual_current"),
]
STYLE = {
    "font.size": 10.5,
    "text.color": "#262B33", "axes.labelcolor": "#262B33",
    "axes.labelsize": 11.5, "axes.linewidth": 0.8,
    "axes.edgecolor": "#9DA2AA", "axes.spines.top": False,
    "axes.spines.right": False, "axes.axisbelow": True,
    "xtick.color": "#636A73", "ytick.color": "#636A73",
    "xtick.labelsize": 10.5, "ytick.labelsize": 10.5,
    "xtick.major.size": 3.5, "ytick.major.size": 3.5,
    "xtick.major.width": 0.7, "ytick.major.width": 0.7,
    "grid.color": "#E3E7EB", "grid.linewidth": 0.65,
    "legend.frameon": False, "legend.fontsize": 10.5,
    "mathtext.fontset": "dejavusans",
}


def _arrange_labels(fig, ax, labels):
    """Lift colliding values while keeping each centered above its own bar."""
    gap_points = 2.0
    gap_pixels = gap_points * fig.dpi / 72
    shifts = 0
    for _ in range(100):
        fig.canvas.draw()
        renderer = fig.canvas.get_renderer()
        collision = next(((a, b) for i, a in enumerate(labels) for b in labels[i + 1:]
                          if a.get_window_extent(renderer).padded(gap_pixels / 2).overlaps(
                              b.get_window_extent(renderer).padded(gap_pixels / 2))), None)
        if collision is None:
            break
        higher, lower = sorted(collision, key=lambda label: label.xy[1], reverse=True)
        above = lower.get_window_extent(renderer).y1 + gap_pixels + 0.5
        delta = above - higher.get_window_extent(renderer).y0
        dx, dy = higher.get_position()
        higher.set_position((dx, dy + max(delta, 1) * 72 / fig.dpi))
        shifts += 1
    else:
        raise AssertionError("Value label layout did not converge")

    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    width, height = fig.canvas.get_width_height()
    texts = list(ax.get_xticklabels()) + list(ax.get_yticklabels())
    texts += [ax.xaxis.label, ax.yaxis.label] + list(ax.texts) + list(fig.texts)
    texts += [text for legend in fig.legends for text in legend.get_texts()]
    texts += [legend.get_title() for legend in fig.legends]
    for text in texts:
        if text.get_visible() and text.get_text():
            box = text.get_window_extent(renderer)
            assert 0 <= box.x0 <= box.x1 <= width and 0 <= box.y0 <= box.y1 <= height, text.get_text()
    assert len(labels) == 21
    assert all(label.get_position()[0] == 0 for label in labels)
    assert all(not a.get_window_extent(renderer).padded(gap_pixels / 2).overlaps(
        b.get_window_extent(renderer).padded(gap_pixels / 2))
        for i, a in enumerate(labels) for b in labels[i + 1:])
    return {"horizontal_value_labels": len(labels), "minimum_label_gap_points": gap_points,
            "collision_adjustments": shifts,
            "label_vertical_offsets_points": [label.get_position()[1] for label in labels]}


def _draw(kind, source, output):
    fig = plt.figure(figsize=(8.8, 4.8), dpi=150)
    ax = fig.add_axes([0.095, 0.14, 0.88, 0.69])
    handles, labels = [], []
    for index, (key, label, color, residual) in enumerate(SERIES):
        metric = residual if kind == "remaining_syndromes" else "speed_" + key
        values = [float(source[metric][f"d{d:02d}"]) for d in DISTANCES]
        assert all(value > 0 for value in values)
        positions = [i + (index - 1) * 0.32 for i in range(len(DISTANCES))]
        ax.bar(positions, values, width=0.25, color=color, linewidth=0)
        for position, value in zip(positions, values):
            labels.append(ax.annotate(f"{value:,.2f}", xy=(position, value),
                xytext=(0, 4), textcoords="offset points", ha="center", va="bottom",
                fontsize=9, rotation=0, color="#262B33", clip_on=False,
                bbox={"facecolor": "white", "edgecolor": "none", "pad": 0.4}))
        handles.append(Patch(facecolor=color, label=label))
    legend = fig.legend(handles=handles, loc="upper center", bbox_to_anchor=(0.535, 0.985),
        borderaxespad=0, handlelength=1.6, handletextpad=0.6, labelspacing=0.55,
        columnspacing=1.8, ncol=3, title="TailReset", title_fontsize=11.5)
    legend.get_title().set_fontweight("semibold")
    ax.set_xlabel("Code distance, $d$", labelpad=9)
    ax.set_xticks(range(len(DISTANCES)), DISTANCES)
    ax.set_xlim(-0.60, len(DISTANCES) - 0.40)
    ax.grid(axis="y", which="major")
    ax.text(0.025, 0.965, "$p = 0.3\\%$, rounds $= d$", transform=ax.transAxes,
        ha="left", va="top", fontsize=10, color="#636A73")
    if kind == "remaining_syndromes":
        ax.set_ylim(0, 2200)
        ax.set_yticks([0, 500, 1000, 1500, 2000])
        ax.set_ylabel("Mean remaining syndromes per shot", labelpad=10)
    else:
        maximum = max(float(source["speed_" + key][f"d{d:02d}"]) for key, _, _, _ in SERIES for d in DISTANCES)
        ceiling = max(420, 100 * math.ceil(maximum * 1.12 / 100))
        ax.set_ylim(0, ceiling)
        ax.set_yticks(range(0, int(ceiling) + 1, 100))
        ax.set_ylabel("Mean decoding time (μs/shot)", labelpad=10)
    checks = _arrange_labels(fig, ax, labels)
    save_figure(fig, output, kind)
    return checks


def render(output: Path):
    """Read the bundled exact CSV and export both figures to ``output``."""
    configure_style(STYLE)
    with (DATA / "predecode.csv").open(newline="", encoding="utf-8") as stream:
        records = list(csv.DictReader(stream))
    source = {row["metric"]: row for row in records}
    assert len(source) == len(records) == 10
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    checks = {kind: _draw(kind, source, output)
              for kind in ("remaining_syndromes", "decoding_time")}
    rows = [{"distance": d, "physical_error_rate": "0.003", "rounds": d,
             "independent_shots": 1000000, "strategy": key, "label": label,
             "remaining_syndromes_per_shot": source[residual][f"d{d:02d}"],
             "mean_decoding_time_us_per_shot": source["speed_" + key][f"d{d:02d}"]}
            for key, label, _, residual in SERIES for d in DISTANCES]
    with (output / "plot-data.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    (output / "render-validation.json").write_text(json.dumps(checks, indent=2) + "\n")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent / "output" / "predecode")
    render(parser.parse_args().output)
