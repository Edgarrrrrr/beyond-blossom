"""Render measured speedups with zero outer padding and a bottom colorbar.

Read frozen r02 data/fonts; preserve all displayed values and winner outlines.
The shared color scale spans 0–12×, covering the measured maximum.
"""
import argparse
import csv
import hashlib
import json
import os
import struct
from pathlib import Path

from plot_fonts import register_bundled_fonts

import matplotlib
matplotlib.use("Agg")
from matplotlib import font_manager
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap, Normalize
from matplotlib.patches import FancyBboxPatch
import numpy as np

NAMES = ["PyMatching", "+ SIMD-Enabled Structured Grid",
         "+ Degree-ordered predecoder", "+ Component", "+ TailReset", "+ Spatial"]
TICKS = ["PyMatching", "+ SIMD-Enabled\nStructured Grid",
         "+ Degree-ordered\npredecoder", "+ Component", "+ TailReset", "+ Spatial"]


def reference(path):
    path = Path(path).resolve()
    return dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def render(output: Path, *, data_dir=None, data=None, stem='incremental_ablation'):
    source, output = Path(__file__).resolve().parent, Path(output).resolve()
    inputs = Path(data_dir).resolve() if data_dir is not None else source
    output.mkdir(parents=True, exist_ok=True)
    fonts = register_bundled_fonts(source / 'fonts')
    supplied = data is not None
    if data is None:
        data = json.loads((inputs / "data/ablation.json").read_text())
    distances, methods = data["distances"], data["methods"]
    assert distances and len(methods) == 6 and data['panels']
    rows, panels = len(distances), len(data['panels'])
    plt.rcdefaults()
    plt.rcParams.update({
        "pdf.fonttype": 42, "ps.fonttype": 42, "svg.fonttype": "none",
        "figure.facecolor": "white", "savefig.facecolor": "white",
        "font.size": 12.5, "text.color": "black",
        "axes.labelcolor": "black", "axes.titlesize": 16, "axes.labelsize": 13,
        "xtick.color": "black", "ytick.color": "black",
        "xtick.labelsize": 12.5, "ytick.labelsize": 13, "mathtext.fontset": "dejavusans",
    })
    plt.rcParams.update({'font.family': 'serif', 'font.serif': ['Times New Roman'], 'mathtext.fontset': 'dejavuserif'})
    font_files = {}
    for weight in ('normal', 'bold'):
        path = Path(font_manager.findfont(font_manager.FontProperties(
            family=plt.rcParams['font.serif'], weight=weight)))
        font_files[weight] = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                  family=font_manager.FontProperties(fname=str(path)).get_name())
    assert all(font['family'] == 'Times New Roman' for font in font_files.values())
    cmap = LinearSegmentedColormap.from_list(
        "Blues_black_text", matplotlib.colormaps["Blues"](np.linspace(0, .58, 256)))
    all_speedups = np.asarray([panel["speedups"] for panel in data["panels"]])
    assert np.all(np.isfinite(all_speedups)) and np.all(all_speedups > 0)
    speedup_max = float(np.max(all_speedups))
    colorbar_max = max(12, int(np.ceil(speedup_max / 2)) * 2)
    norm = Normalize(vmin=0, vmax=colorbar_max)
    assert np.all((norm(all_speedups) >= 0) & (norm(all_speedups) <= 1))
    cell_gap_pt, corner_radius_pt = 3.4, 2.2
    outline_color, outline_width_pt = "#A63D24", 1.6
    rgb = cmap(np.linspace(0, 1, 256))[:, :3]
    linear = np.where(rgb <= .04045, rgb/12.92, ((rgb+.055)/1.055)**2.4)
    minimum_contrast = float(np.min((linear @ np.array([.2126, .7152, .0722])+.05)/.05))
    assert minimum_contrast >= 4.5
    fig, axes = plt.subplots(1, panels, figsize=(18.5 * panels / 3, 6.6), dpi=170, squeeze=False)
    axes = axes[0]
    # Keep the paper's physical left margin when smoke has fewer panels.
    # Scaling the canvas alone clips the fixed-size y-axis label.
    left_margin = .055 if panels >= 3 else .055 * 3 / panels
    fig.subplots_adjust(left=left_margin, right=.995, bottom=.37, top=.84, wspace=.08)
    plot_rows, cell_labels, cell_geometry = [], [], []
    for ax, panel in zip(axes, data["panels"]):
        speed, times = np.asarray(panel["speedups"]), np.asarray(panel["time_us"])
        assert speed.shape == times.shape == (rows, 6)
        ax.set_xlim(-.5, 5.5)
        ax.set_ylim(rows - .5, -.5)
        # Use physical units so gaps and corner radii look equal in both directions.
        cell_width_pt = ax.get_position().width * fig.get_figwidth() * 72 / 6
        cell_height_pt = ax.get_position().height * fig.get_figheight() * 72 / rows
        inset_x, inset_y = cell_gap_pt / (2 * cell_width_pt), cell_gap_pt / (2 * cell_height_pt)
        half_width, half_height = .5 - inset_x, .5 - inset_y
        cell_geometry.append(dict(width_fraction=2 * half_width, height_fraction=2 * half_height))
        ax.set_xticks(range(6), TICKS, rotation=48, ha="right", rotation_mode="anchor")
        ax.set_yticks(range(rows), [str(d) for d in distances])
        ax.tick_params(axis="both", length=0, pad=7)
        for spine in ax.spines.values():
            spine.set_visible(False)
        ax.set_title(f'p = {panel["p"] * 100:g}%', pad=48, weight='bold')
        transform = ax.get_xaxis_transform()
        ax.plot([.5, .5, 5.5, 5.5], [1.025, 1.045, 1.045, 1.025],
                transform=transform, clip_on=False, color="#636A73", lw=.75)
        ax.text(3, 1.07, "Beyond blossom", transform=transform,
                ha="center", va="bottom", fontsize=12.5)
        for i, distance in enumerate(distances):
            winner = int(np.argmin(times[i]))
            assert winner == int(np.argmax(speed[i]))
            for j, method in enumerate(methods):
                ax.add_patch(FancyBboxPatch((j - half_width, i - half_height),
                    2 * half_width, 2 * half_height,
                    boxstyle=f"round,pad=0,rounding_size={corner_radius_pt / cell_width_pt}",
                    mutation_aspect=cell_width_pt / cell_height_pt,
                    facecolor=cmap(norm(speed[i, j])),
                    edgecolor=outline_color if j == winner else "none",
                    linewidth=outline_width_pt if j == winner else 0,
                    zorder=1))
                # Source ratios and times were rounded independently in the supplied image.
                lower = (times[i, 0] - .0005) / (times[i, j] + .0005)
                upper = (times[i, 0] + .0005) / (times[i, j] - .0005)
                assert lower <= speed[i, j] + .005 and upper >= speed[i, j] - .005
                plot_rows.append(dict(p=panel["p"], d=distance, source_method=method,
                    display_name=NAMES[j], speedup=f"{speed[i, j]:.2f}",
                    batch_us_per_shot=f"{times[i, j]:.3f}", fastest=j == winner))
                speed_label = ax.text(j, i-.18, f"{speed[i, j]:.2f}×",
                    ha="center", va="center", fontsize=12,
                    weight='bold' if j == winner else 'normal', color='black')
                time_label = ax.text(j, i+.22, f"{times[i, j]:.3f} μs",
                    ha="center", va="center", fontsize=10.5, color='black')
                cell_labels.extend([(ax, label, i, j, half_width, half_height)
                                    for label in (speed_label, time_label)])
    axes[0].set_ylabel("Code distance (d)", labelpad=7)
    # Place the bar below the lowest rotated method label, with a 10 pt gap.
    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    labels_bottom_px = min(label.get_window_extent(renderer).y0
                           for ax in axes for label in ax.get_xticklabels())
    bar_height_inches, bar_gap_pt = .12, 10
    bar_top = (labels_bottom_px - bar_gap_pt * fig.dpi / 72) / fig.bbox.height
    bar_height = bar_height_inches / fig.get_figheight()
    cax = fig.add_axes([.275, bar_top - bar_height, .50, bar_height])
    ticks = list(range(0, colorbar_max + 1, 2))
    bar = fig.colorbar(matplotlib.cm.ScalarMappable(norm=norm, cmap=cmap),
                       cax=cax, ticks=ticks, orientation="horizontal")
    bar.set_ticklabels([f"{tick:g}×" for tick in ticks])
    bar.set_label("Speedup over PyMatching", labelpad=5, fontsize=13)
    bar.ax.tick_params(labelsize=12.5, length=3)
    bar.outline.set_linewidth(.6)

    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    width, height = fig.canvas.get_width_height()
    assert not fig.texts, 'The figure-level caption must stay removed'
    assert cax.get_window_extent(renderer).y1 < labels_bottom_px
    # Include stroke width when checking the gap between neighboring frames.
    minimum_frame_gap_pt = cell_gap_pt - outline_width_pt
    assert minimum_frame_gap_pt > 0
    for ax in fig.axes:
        texts = list(ax.texts) + list(ax.get_xticklabels()) + list(ax.get_yticklabels())
        for label in texts + [ax.title, ax.xaxis.label, ax.yaxis.label]:
            if not label.get_visible() or not label.get_text():
                continue
            box = label.get_window_extent(renderer)
            assert 0 <= box.x0 < box.x1 <= width and 0 <= box.y0 < box.y1 <= height, (
                label.get_text(), box.bounds, (width, height))
    for ax, label, i, j, half_width, half_height in cell_labels:
        box = label.get_window_extent(renderer)
        left, bottom = ax.transData.transform((j - half_width, i + half_height))
        right, top = ax.transData.transform((j + half_width, i - half_height))
        # Keep text clear of the highlighted cell's inner stroke edge.
        stroke_inset_px = outline_width_pt * fig.dpi / 144
        left, right = left + stroke_inset_px, right - stroke_inset_px
        bottom, top = bottom + stroke_inset_px, top - stroke_inset_px
        assert left <= box.x0 and box.x1 <= right and bottom <= box.y0 and box.y1 <= top, (
            label.get_text(), box.width, right-left)
    assert len(plot_rows) == rows * panels * 6 and sum(row["fastest"] for row in plot_rows) == rows * panels
    for extension in ('pdf', 'svg', 'png'):
        options = dict(dpi=600) if extension == 'png' else {}
        fig.savefig(output / f'{stem}.{extension}',
                    bbox_inches='tight', pad_inches=0, **options)
    fig.savefig(output / f'{stem}_preview.png', dpi=170,
                bbox_inches='tight', pad_inches=0)
    plt.close(fig)
    (output / "rendering.json").write_text(json.dumps(dict(
        title="Beyond blossom", text_color="black", blue_ramp_max=.58, speedup_scale=[0, colorbar_max],
        measured_speedup_max=speedup_max, clipped_cells=0,
        colorbar=dict(orientation="horizontal", location="bottom", ticks=ticks,
                      maximum_tick_label=f"{colorbar_max}×", method_label_gap_pt=bar_gap_pt),
        layout_revision="r05-bottom-colorbar-12", figure_caption_visible=False,
        export_bbox="tight", export_padding_inches=0,
        minimum_black_text_contrast=minimum_contrast, figure_inches=[18.5 * panels / 3, 6.6],
        font_family=font_files['normal']['family'], font_files=font_files,
        cells=dict(count=rows * panels * 6, gap_pt=cell_gap_pt, corner_radius_pt=corner_radius_pt,
                   geometry=cell_geometry, shape="rounded rectangle"),
        winner_outline=dict(color=outline_color, linewidth_pt=outline_width_pt,
                            follows_cell_boundary=True, white_stroke=False,
                            minimum_adjacent_gap_pt=minimum_frame_gap_pt),
        winner_speedups_bold=rows * panels, probability_title_weight='bold',
        font_pt=dict(method=12.5, speedup=12, time=10.5, distance_tick=13, axis_label=13,
                     probability_title=16, group_title=12.5, colorbar_tick=12.5),
        text_bounds="passed", numeric_cell_bounds="passed"), indent=2) + "\n")
    for name, rows in [("plot-data.csv", plot_rows),
                       ("fastest-cells.csv", [r for r in plot_rows if r["fastest"]])]:
        with (output / name).open("w", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    previous = inputs / 'reference'
    for name in ('plot-data.csv', 'fastest-cells.csv'):
        def numeric_rows(path):
            with path.open(newline='') as stream:
                return [{k:v for k,v in row.items() if k != 'display_name'} for row in csv.DictReader(stream)]
        if not supplied:
            assert numeric_rows(output / name) == numeric_rows(previous / name), 'Displayed numerical values changed'
    dimensions = {name: struct.unpack('>II', (output / name).read_bytes()[16:24])
                  for name in (f'{stem}.png', f'{stem}_preview.png')}
    receipt = dict(complete=True, host=os.uname().nodename, job_id=os.environ.get('SLURM_JOB_ID'),
                   revision='r05-bottom-colorbar-12', figure_caption_visible=False,
                   padding_inches=0, colorbar_orientation='horizontal', colorbar_location='bottom',
                   colorbar_max=colorbar_max, speedup_max=speedup_max, clipped_cells=0,
                   dimensions_px=dimensions, fonts=fonts,
                   source_data=reference(inputs / 'data/ablation.json') if not supplied else {'scope': 'Caller-provided validated measurements'},
                   renderer=reference(__file__), provenance=reference(inputs / 'SOURCE.json') if not supplied else None,
                   font_helper=reference(Path(__file__).with_name('plot_fonts.py')),
                   historical_numeric_comparison='not applicable' if supplied else 'passed',
                   artifacts={p.name: reference(p) for p in output.iterdir() if p.is_file()})
    (output / 'complete.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(dict(complete=True, output=str(output), speedup_max=speedup_max,
                         dimensions_px=dimensions)))


def render_paper(output, native, *, profile='paper'):
    """Use the paper layout with validated newly measured values."""
    methods = ['reference', 'structured', 'predecoded', 'component-parallel', 'pipelined', 'parallel-predecode']
    distances = sorted({d for d, p, m in native})
    rates = sorted({p for d, p, m in native})
    panels = []
    for p in rates:
        times = [[native[d, p, m]['mean_us'] for m in methods] for d in distances]
        panels.append(dict(p=float(p), time_us=times, speedups=[[r[0] / t for t in r] for r in times]))
    render(output, data=dict(distances=distances, methods=methods, panels=panels), stem='incremental_ablation_1107_v1')
    return [reference(Path(output) / f'incremental_ablation_1107_v1.{ext}') for ext in ('pdf', 'svg', 'png')]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent / 'output/ablation',
                        help='New output directory (default: AE/output/ablation)')
    args = parser.parse_args()
    render(args.output)
