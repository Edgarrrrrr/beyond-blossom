# Artifact evaluation

The AE collects the paper's experiments and generates their figures automatically.
The collection, saved-result replay, and historical-paper paths use the same
plotting implementations. No source edits or custom postprocessing scripts are
required. Run commands from the repository root and use a new output directory
for each run.

## Setup

Use Python 3.11+ and pip 22.3+. Create an isolated environment:

```sh
python3 -m venv --without-pip .venv
python3 -m pip --python .venv install pip -r AE/requirements.txt
. .venv/bin/activate
AE_WORK="$PWD/../beyond-blossom-ae"
```

The plotting environment pins Matplotlib 3.11.0. The paper's Times New Roman and
Arial fonts are bundled with their license; no host font installation is needed.

Fresh experiments also require Git, CMake 3.21+, a C++20 compiler, OpenMP,
Python development headers, and Rust nightly. Install the package and toolchain:

```sh
python -m pip install '.[stim]'
rustup toolchain install nightly-2026-03-10 --profile minimal
```

On Debian/Ubuntu, the native prerequisites are `g++`, `cmake`, `git`,
`python3-dev`, and `libgomp1`. The Linux workflow uses eight physical cores on one
socket. For the full matrix, plan about 400 GB of working storage with hard-link
input reuse and use a large-memory host; the authors' compute nodes have 128 GiB
RAM. The smoke profile is much smaller. All decoder sources and locked Rust crates are included
in [vendor/](../vendor/README.md); `--offline` disables build-time downloads.

## Collect and generate figures

First run the complete workflow on a small input set:

```sh
python AE/run.py run --profile smoke --work "$AE_WORK/smoke" --offline
```

Then collect the full paper matrix from a clean Git checkout:

```sh
python AE/run.py run --work "$AE_WORK/paper" --offline
```

The runner builds the configurations, generates shared inputs, runs the decoders,
and exports the figures and tables. Beyond Blossom and the fixed official
PyMatching reference each use **1,000,000 shots per setting**, with three timing
passes. External methods use the paper's per-setting **1,000,000 / 10,000-shot**
assignment and three passes; reduced runs use the same input stream's prefix.
The [sample-count table](REFERENCE.md#external-sample-counts) lists the assignments.
There are seven distances and three error rates, for 21 settings. Serial
predecoder timing and residual counts are collected separately at p=0.003;
L2/L3 counters are collected when the CPU and permissions support them.
Use `--jobs N` to set build parallelism (default: 2); this does not change the
measured decoder thread counts.

The final message and `complete.json` identify the export directory. It contains:

| Paper result | Output |
| --- | --- |
| Incremental ablation | `figures/incremental_ablation_1107_v1.{pdf,svg,png}` |
| Decoder comparison, six series | `figures/decoder_comparison_shows_speedup_v3.{pdf,svg,png}` |
| Predecoder table | `predecoder.csv`, `predecoder_numbers.tex` |
| Paper numbers and input provenance | `performance-macros.tex`, `canonical.json`, CSV tables |

The figures retain the paper's layout, fonts, colors, labels, axes and marker
encoding. Re-execution supplies new measured timings; it does not substitute
historical numbers. Sample counts, hardware and correctness records stay in the
result files, without extra annotations being added to the paper artwork.
Apple M4 measurements require an M4 machine; the [M4 procedure](REFERENCE.md#m4-acquisition)
generates `m4_pro_per_round.{pdf,svg,png}` with the same paper renderer.

Resume an interrupted run with `python AE/run.py resume --work "$AE_WORK/paper"`.
Completed stages are reused, and failed attempts remain available.

## Reuse completed measurements

Completed 21-setting Xeon measurements are bundled for a quick rendering check.
Their native runs use one million shots; their external runs uniformly use
10,000 shots, which differs from the paper's mixed-count policy at 54 points:

```sh
python AE/replot.py --input AE/data/reproduction/canonical.json \
  --output "$AE_WORK/included-results"
```

Replay an existing completed local or Slurm run through the same entry point:

```sh
python AE/replot.py --run "$AE_WORK/paper" --output "$AE_WORK/redraw"
```

These commands verify the saved data and generate the same paper layouts used by
fresh collection. They run no decoder and require no original large input files.
The included measurements and their sample counts are described in
[data/reproduction/](data/reproduction/README.md).

## Optional Slurm execution

Keep the checkout, Python environment and work directory on shared storage.
Compute nodes need Git and the native runtime libraries. Choose the partition
and allocation for the cluster; each worker needs eight physical cores on one
socket. For example:

```sh
python AE/slurm.py run --work "$AE_WORK/slurm-paper" \
  --partition cpu --shards 6 --cpus-per-task 16 --offline
```

Builds finish before submission. Each setting stays on one node, and an `afterok`
job exports the combined figures automatically. `--nodes node01,node02` selects
named nodes instead of `--shards`; node names and partition names have no
site-specific defaults. `--mem` and `--time` set allocation limits.
Use `--profile smoke` with a new work directory for a small cluster check.
`python AE/slurm.py resume --work "$AE_WORK/slurm-paper"` retries unfinished work.
The [reference](REFERENCE.md#slurm-configuration) includes the authors' cluster example.

## Historical paper figures

The original ablation and M4 data are available and can be redrawn directly:

```sh
python AE/replot.py --paper-figures ablation m4 --output "$AE_WORK/original-figures"
```

The original comparison figure's input table has not yet been recovered. The older
`data/decoders.csv` is a different 1,000-shot experiment and is not substituted.
The default request for all three original figures therefore fails explicitly
until that source is included. Fresh collection and replay of the completed
reproduction above are available. [Provenance](data/SOURCES.md) and the
[detailed reference](REFERENCE.md) distinguish the datasets and timing scopes.
