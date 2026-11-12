# Redraw the archived results

Redraw the included figures from the repository root:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r AE/requirements.txt
python AE/results/20261112/replot.py --output /path/to/new-figures
```

The script verifies the canonical and renderer hashes, then writes PDF/SVG/PNG
figures and `replot.json`. It uses only repository files; original absolute paths
remain provenance text. It does not rerun decoding or require the measurement
machines. `--input PATH --manifest PATH` selects another sealed export; smoke
exports retain their smoke labels.

The manifest uses `{"schema":1,"exporter_sha256":"…","files":{"canonical.json":"…"}}`.
The renderer hash identifies `AE/export_results.py`; extra archived file entries
are allowed. The original export receipt is provenance, not a redraw dependency.

`figures/` preserves the automatic export. `presentation/` contains the same
values redrawn with vertical bar labels to prevent overlap; the report uses
these figures. `original_exporter_sha256` identifies the acquisition-time
renderer, and `exporter_sha256` identifies the current presentation renderer.
No measurement changed in this layout revision.

The source and collector used commit `75aedd433d1b129e5a973635320f199b5f26cbae`.
The [AE guide](../../README.md) provides smoke and full recollection commands for
local Linux and Slurm. The archived run used six Xeon Gold 6230R nodes, 1,000,000
native inputs per setting, and a 10,000-input prefix for each external method.
