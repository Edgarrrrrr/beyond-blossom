# Reproduce the paper

AE provides two workflows: regenerate the paper's archived figures, or build and run the decoders to collect new results. Run the commands below from the repository root.

## Setup

Use Python 3.11+, CMake 3.21+, a C++20 compiler, OpenMP, and Git. Fresh external-decoder measurements also need Rust nightly. On Debian/Ubuntu, install `g++`, `cmake`, `python3-dev`, `python3-venv`, and `libgomp1`.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install '.[stim]' -r AE/requirements.txt
```

For fresh collection, install the pinned Rust toolchain once:

```sh
rustup toolchain install nightly-2026-03-10 --profile minimal
```

Use a work directory outside the checkout. Builds, inputs, logs, and results stay there. For the default full matrix, plan for 32 GB RAM and roughly 100 GB of free space with hard-link reuse; the saved inputs alone occupy about 61 GB. Filesystems that require input copies need more space.

```sh
AE_WORK="$PWD/../beyond-blossom-ae"
```

Decoder sources, reference adapters, and locked Rust crates are included in [vendor/](../vendor/README.md). After installing the tools and Python packages, `--offline` builds without network access or private directories. The automatic local and Slurm workflows target Linux and need eight physical cores on one socket. [M4 acquisition](REFERENCE.md#m4-acquisition) provides the macOS commands using QoS without fixed physical placement.

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

The runner discovers CPU placement and SIMD support, verifies bundled dependencies, builds all configurations, and executes the measurements serially. It compares against the separately built official upstream reference. Builds finish before timing starts.

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

Completed stages are reused; an incomplete stage restarts in a new attempt directory. Failed attempts remain available. `plan` prints the configuration. For separate build and measurement phases, use `build --work BUILD_DIR`, then `run --work RUN_DIR --builds-manifest BUILD_DIR/builds.json` with a fresh run directory. `collect --work RUN_DIR` resumes an existing run without building. `--source` selects a fixed checkout, `--jobs` controls compilation, and `--cpus` overrides placement. See `python AE/run.py --help` for all options.

## Optional Slurm execution

Place the checkout, Python environment, and work directory on storage shared by the compute nodes. Compute nodes need Git and the native runtime libraries; compilers are needed only on the build host. Choose the partition and CPU allocation for the cluster; the allocation must contain at least eight physical cores on one socket. For example, on a cluster with a `cpu` partition:

```sh
python AE/slurm.py run --work "$AE_WORK/slurm-paper" \
  --partition cpu --shards 6 --cpus-per-task 16 --offline
```

`--nodes node01,node02` selects named nodes instead of `--shards`; no cluster names are built into the runner. `--mem` and `--time` set resource limits. Builds finish on the submission host, then each worker runs complete settings serially. All methods and repeats of a setting stay on the same node. An `afterok` job exports the combined results automatically.

Add `--profile smoke` and use a new work directory for a one-setting check. `prepare` writes a plan without building or submitting; `submit --work ...` executes it. `resume --work ...` retries unfinished work. `submissions.json` records job IDs, and `logs/` contains scheduler output. [The acquisition reference](REFERENCE.md#slurm-configuration) includes the configuration used on the authors' cluster.

## Read the results

The [2026-11-12 reproduction](results/20261112/README.md) includes the full 21-setting run, small measurement records, independent audits, figures, and a [repository-only redraw command](results/20261112/REPRODUCE.md).

The final export contains the ablation and decoder-comparison figures, predecoder table, per-method CSVs, selected configurations, and paper-compatible LaTeX macros. `canonical.json` records the inputs, hardware, and SIMD backend; completion receipts identify the actual builds and runs. Missing hardware-specific values are listed, not filled from historical data.

Apple M4 performance requires an M4 machine. The Linux workflow does not claim new M4 measurements. [Detailed acquisition commands](REFERENCE.md) cover M4, individual stages, saved-input reuse, timing scopes, and separate exports. [External decoder details](external/README.md) document their native timing scopes and correctness checks.

Historical predecoder data used a four-thread pipeline and a coordinate-order baseline. Fresh local collection follows the manuscript's **serial** configuration and includes an explicit half-edge, equal-share simple baseline. The selected policy and execution scope are recorded; new timings remain separate from the archived paper values.
