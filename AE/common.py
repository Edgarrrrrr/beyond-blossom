"""Shared paths, typography, and paper export formats."""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data"


def configure_style(settings=None):
    """Reset style between figure groups; use a portable sans-serif fallback."""
    plt.rcdefaults()
    plt.rcParams.update(settings or {})
    plt.rcParams.update({
        "font.family": "sans-serif",
        "font.sans-serif": ["Arial", "Liberation Sans", "DejaVu Sans"],
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
        "svg.fonttype": "none",
        "figure.facecolor": "white",
        "savefig.facecolor": "white",
    })


def save_figure(fig, output, name):
    """Export vector PDF/SVG, a 600 dpi PNG, and a screen preview."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    for extension in ("pdf", "svg", "png"):
        options = {"dpi": 600} if extension == "png" else {}
        fig.savefig(output / f"{name}.{extension}", **options)
    fig.savefig(output / f"{name}_preview.png", dpi=170)
    plt.close(fig)
