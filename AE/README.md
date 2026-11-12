# Reproduce the paper

AE provides two workflows: regenerate the paper's archived figures, or build and run the decoders to collect new results. Run the commands below from the repository root.

## Setup

Use Python 3.11+, CMake 3.21+, a C++20 compiler, OpenMP, and Git. Fresh external-decoder measurements also need Rust nightly. On Debian/Ubuntu, the native packages are `g++`, `cmake`, `python3-dev`, and `libgomp1`.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install '.[stim]' -r AE/requirements.txt
```

For fresh collection, install the pinned Rust toolchain once:

```sh
rustup toolchain install nightly-2026-03-10 --profile minimal
```

Use a work directory outside the checkout. Builds, inputs, logs, and results stay there.

```sh
AE_WORK="$PWD/../beyond-blossom-ae"
```

## Regenerate archived figures

```sh
python AE/replot.py --output "$AE_WORK/archived"
```

This verifies bundled measurements and regenerates PDF/SVG/PNG figures, CSV tables, and LaTeX macros. It runs no decoder. It also reproduces the archived M4 figure on Linux. [Data provenance](data/SOURCES.md) describes the original measurements.

## Run the experiments

Start with a small end-to-end check:

```sh
python AE/run.py run --profile smoke --work "$AE_WORK/smoke"
```

Then run the full local matrix from a clean Git checkout:

```sh
python AE/run.py run --work "$AE_WORK/paper" --external-shots 10000
```

The runner discovers CPU placement and SIMD support, obtains pinned dependencies, builds all configurations, and executes the measurements serially. It compares against the separately built official upstream reference. Builds finish before timing starts.

| Experiment | Fresh local collection |
| --- | --- |
| Incremental ablation | 21 settings; 1,000,000 saved shots per setting; three passes; reference plus five Beyond Blossom configurations |
| External comparison | The same saved inputs; 10,000-shot prefix for each slower external method; three passes |
| Predecoder table | None, simple, and degree-ordered policies; serial execution; seven distances at p=0.003; separate timing and statistics |
| Cache misses | L2/L3 counters at d=33, p=0.003, when supported by the local CPU and permissions |

Beyond Blossom and the official reference use **1,000,000 shots in every setting**. External figure markers use the actual recorded sample count. `--external-shots 1000000` requests a million shots for the external methods too; this can take days. On the paper's Xeon class of machine, the native matrix takes hours and the 10,000-shot external matrix adds several hours. Smoke results validate the workflow and are labeled separately from paper measurements.

Progress is printed as each stage starts. Per-stage output is in `logs/`; `state.json` records completion and failures. To continue an interrupted workflow:

```sh
python AE/run.py resume --work "$AE_WORK/paper"
```

Completed stages are reused. Failed attempts remain available for inspection. `plan` prints the configuration without building or measuring; `build` and `collect` expose the two phases separately. `--source` selects a fixed source checkout, `--jobs` controls build parallelism, and `--cpus` overrides CPU placement. See `python AE/run.py --help` for offline dependency and existing-build options.

## Read the results

The final export contains the ablation and decoder-comparison figures, predecoder table, per-method CSVs, selected configurations, and available Xeon LaTeX macros. `canonical.json` records the inputs behind the figures; completion receipts identify the actual builds and runs. Missing hardware-specific values are listed, not filled from historical data.

Apple M4 performance requires an M4 machine. The Linux workflow does not claim new M4 measurements. [Detailed acquisition commands](REFERENCE.md) cover M4, individual stages, saved-input reuse, timing scopes, and separate exports. [External decoder details](external/README.md) document their native timing scopes and correctness checks.

Historical predecoder data used a four-thread pipeline and a coordinate-order baseline. Fresh local collection follows the manuscript's **serial** configuration and includes an explicit half-edge, equal-share simple baseline. The selected policy and execution scope are recorded; new timings remain separate from the archived paper values.
