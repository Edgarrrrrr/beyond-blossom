#!/usr/bin/env python3
"""Render the paper's experiment figures from the bundled measurements."""
import argparse
import importlib
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent
GROUPS = ("predecode", "decoders", "grid", "ablation", "latest-matrix")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("figures", nargs="*",
                        metavar="FIGURE", help="all (default), predecode, decoders, grid, ablation, latest-matrix")
    parser.add_argument("--output", type=Path, default=ROOT / "output",
                        help="output directory (default: AE/output)")
    args = parser.parse_args()
    unknown = set(args.figures) - {"all", *GROUPS}
    if unknown:
        parser.error("unknown figure group: " + ", ".join(sorted(unknown)))
    output = args.output.resolve()
    groups = tuple(GROUPS if not args.figures or "all" in args.figures else dict.fromkeys(args.figures))
    existing = [str(output / name) for name in groups if (output / name).exists()]
    if existing:
        parser.error("figure output already exists; choose a new --output directory: " + ", ".join(existing))
    output.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("MPLCONFIGDIR", str(output / ".matplotlib"))
    for name in groups:
        folder = output / name
        importlib.import_module(name.replace('-', '_')).render(folder)
        print(f"{name}: {folder}")


if __name__ == "__main__":
    main()
