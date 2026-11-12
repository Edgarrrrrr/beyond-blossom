# Acquisition reference

Use [AE/README.md](README.md) for installation and the standard workflows:
`AE/run.py` collects locally, `AE/slurm.py` distributes complete settings, and
`AE/replot.py` redraws saved results. Commands run from the repository root, with
`AE_WORK` set to a work directory outside the checkout.

## Saved results and paper figures

Replay a completed local or Slurm workflow without running a decoder:

```sh
python AE/replot.py --run "$AE_WORK/paper" --output "$AE_WORK/redraw"
```

Alternatively, replay a sealed canonical export, including the compact measurements
bundled with the repository:

```sh
python AE/replot.py --input AE/data/reproduction/canonical.json \
  --output "$AE_WORK/included-results"
```

`--input` discovers the adjacent manifest or export receipt; `--manifest` selects
one explicitly. Replay verifies the canonical data and seal, uses the current
shared paper renderers, and records their hashes. It copies available tables and
macros unchanged. It performs no sampling, decoding or raw-run revalidation.
Output directories must be new.

Fresh collection and canonical replay use the same figure implementations:

| Paper figure | Output basename | Required measurements |
| --- | --- | --- |
| Incremental ablation | `incremental_ablation_1107_v1` | Official reference and five Beyond Blossom configurations |
| Decoder comparison | `decoder_comparison_shows_speedup_v3` | Native and external methods on the same saved input streams |
| Apple M4 per-round time | `m4_pro_per_round` | M4 batch measurements |

Each figure is exported as PDF, SVG and PNG. The predecoder experiment is a
**table**, represented by `predecoder.csv` and `predecoder_numbers.tex`; the paper
renderer does not add decoding-time or residual-syndrome bar charts. The comparison
figure has six series. Parity UF remains in the collected external data but is not
an additional manuscript curve.

Rendering uses Matplotlib **3.11.0**, pinned in [requirements.txt](requirements.txt).
The [font bundle](fonts/README.md) contains all 30 Microsoft Core Fonts files with
their EULA and checksums. The ablation uses Times New Roman, the comparison uses
Arial, and the M4 figure uses DejaVu Sans. The renderers register the bundled fonts
instead of relying on the host's font installation.

Original manuscript figures, historical archives and newly collected measurements
are distinct data sources. The exact input table for the original manuscript's
external-comparison figure has not been identified; the older 1,000-shot comparison
CSV is a different dataset. Matching the paper layout does not establish an exact
redraw of those original measurements. Running `replot.py` without `--run` or `--input` requests all three original
paper figures and fails explicitly while the original comparison source is
missing. `--paper-figures ablation m4` selects the two verified original figures.
Legacy grid and 1,000-shot plots remain separate lower-level utilities. See
[data provenance](data/SOURCES.md) for the archived scopes.

## Measurement scope

The standard paper matrix contains seven distances, d=9,13,17,21,25,29,33, and
three error rates, p=0.0005,0.001,0.003. Circuits are rotated memory-X with d rounds,
the complete X/Z detector stream, and all four circuit-level noise probabilities
set to p. The official reference and each Beyond configuration use one million
saved inputs per setting and three timing passes. Repeating the inputs does not
increase the independent sample count.

The six paper stages are `reference`, `structured`, `predecoded`,
`component-parallel`, `pipelined` and `parallel-predecode`, using 1/1/1/2/4/8 cores.
The lower-level collector also supports `unstructured` as a separate regression
measurement. The reference is the independently built official PyMatching revision
`e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd`; the current unstructured backend does
not replace it.

Native batch timing includes the normal decode lifecycle, result storage and final
pipeline drain. Model construction, input loading, warmup, correctness checks and
output are outside the timing window. Weight differences fail validation;
equal-optimum observable differences require actual candidate correction witnesses.
The input format does not contain physical observable truth, so these checks do not
measure logical failure rates.

Linux results use pooled batch means, calculated from integer elapsed time and shot
counts. M4 results use the median of three complete-batch means. Speedup is official
reference latency divided by candidate latency. The best configuration is selected
from the five measured Beyond configurations per setting; this is a descriptive
selection, not a measured adaptive dispatcher.

## External sample counts

The default follows the manuscript comparison's caption and marker fills.
Beyond Blossom, PyMatching and UnionFind use one million shots at every setting.
The following external points use a 10,000-shot prefix; all other plotted points
use one million shots. Each timing run has three passes.

| Method | p = 0.0005: reduced distances | p = 0.001: reduced distances | p = 0.003: reduced distances |
| --- | --- | --- | --- |
| Parity Blossom | 33 | 25, 29, 33 | 17, 21, 25, 29, 33 |
| Fusion Blossom | 25, 29, 33 | 17, 21, 25, 29, 33 | 13, 17, 21, 25, 29, 33 |
| LDPC-UF | 33 | 29, 33 | 21, 25, 29, 33 |

Supplementary Parity-UF, which has no curve in the manuscript figure, uses 10,000
shots. `--external-shots 10000` or `1000000` selects an explicit uniform override;
`--external-shot-counts counts.json` overrides selected methods or cases.
Saved results retain actual counts and are classified separately when they differ
from the manuscript. Recovering the count assignment does not recover the missing
original comparison timings.

External interfaces have different timing boundaries and integer quantization:
Parity/Fusion time solve and subgraph extraction without clear; UnionFind and LDPC
UF also exclude preparation or cleanup steps. See the
[external reference](external/README.md) for exact scopes and the independent
integer-weight oracle. Missing measurements are never filled from the old CSVs.

## Individual stages

The automatic runners build and schedule these stages. Direct entry points are
available for hardware-specific work or a selected experiment:

| Entry point | Purpose |
| --- | --- |
| `build_reference.py` | Build and seal the fixed official reference |
| `build_native.py` | Build and seal a candidate configuration |
| `reproduce.py run` | Collect batch, latency, PMU or untimed statistics |
| `reproduce.py external` | Collect external decoders on saved native inputs |
| `export_results.py` | Validate completed runs and export figures, tables and macros |

The [vendor bundle](../vendor/README.md) supplies fixed decoder sources, reference
adapters and locked crates. After installing the standard tools and Python packages,
`--offline` builds need no network access. Paper builds require a clean source
checkout and record compiler flags, source files, dependencies and executable hashes.

`reproduce.py run --input-run COMPLETED_RUN` reuses verified circuit, DEM and input
files without sampling. `--link-inputs` requests hard-link reuse where supported;
otherwise the collector copies the files and records the fallback. Reused inputs
retain their original seed, stream identity and shot count. `--measurement latency`
selects a separate per-shot measurement; its values are not substituted for batch
means.

PMU collection uses `--measurement pmu` and separate L2/L3 builds selected by
`--cache-level 2` or `3` on both builders. It is Linux-only and measures the calling
user-space thread, so only serial methods are allowed. L2 uses Cascade Lake demand
read/request events; L3 uses LLC-loads and LLC-load-misses. The reported percentage
is `100 * misses / requests`. Unsupported or multiplexed groups fail. The automatic
workflow collects d=33, p=0.003 when CPU events and permissions permit it, and records
unavailable results explicitly.

For the serial predecoder table, the export roles map to physical build modes:

| Export role | `build_native.py --predecode-mode` | Policy |
| --- | --- | --- |
| `0` | `0` | Degree-ordered predecoding |
| `1` | `1` | No predecoding |
| `2` | `3` | Simultaneous equal-share simple baseline |

Mode 3 caps active-active contributions at half the edge weight, keeps the full
bound for inactive neighbors and boundaries, and rounds down to the even-radius
lattice. All caps are published before coordinate-order tight matching. Mode 2
remains the distinct historical coordinate-order, full-cap implementation.

Timing uses ordinary binaries. Separate `--statistics` builds and
`--measurement statistics --methods predecoded` runs collect untimed counts;
statistics binaries reject timing modes. The analyzer checks
`normalized = residual + 2*pairs + boundaries`. Historical pipeline-table values
remain separate from the fresh serial table.

## M4 acquisition

Apple M4 acquisition requires an M4 machine, Apple Clang and an installed OpenMP
package. The Linux workflow and archived M4 redraw do not establish a new M4 build
or new M4 timings. For a local M4 collection, set `OMP_PREFIX` to the installed
OpenMP prefix and use separate build directories:

```sh
python AE/build_reference.py --offline --output "$AE_WORK/reference-m4" --jobs 2 \
  --cxx-flags="-O3 -mcpu=apple-m4" --cmake-arg="-DCMAKE_PREFIX_PATH=$OMP_PREFIX"
python AE/build_native.py --offline --output "$AE_WORK/native-m4" --jobs 2 \
  --cxx-flags="-O3 -mcpu=apple-m4" --cmake-arg="-DCMAKE_PREFIX_PATH=$OMP_PREFIX"
python AE/reproduce.py run --profile paper --backend scalar \
  --binary "$AE_WORK/native-m4/bin/bb_benchmark" --build-record "$AE_WORK/native-m4/build.json" \
  --reference-binary "$AE_WORK/reference-m4/build/bin/experiment_benchmark" \
  --reference-record "$AE_WORK/reference-m4/reference.json" --output "$AE_WORK/m4-batch"
```

The Arm predecoder kernel is selected during the platform build. Acquisition requests
USER_INITIATED QoS and records physical P/E-core affinity as unverified. Per-round
time is the selected batch time divided by the setting's d measurement rounds.
Use `export_results.py --m4-run` to include a completed M4 run; it must use the same
source commit as the supplied native matrix.

## Slurm configuration

All nodes need access to the same checkout, Python environment and work directory.
Builds finish on the submission host before measurement starts. Each setting and
all its methods, repeats and counters remain on one node. The exporter checks paired
hosts and input identities while allowing different settings on different hosts.

The recorded six-node campaign used the following cluster-specific allocation:

```sh
python AE/slurm.py run --work "$AE_WORK/slurm-paper" --offline \
  --nodes ares1,ares2,ares3,ares4,ares5,ares6 --partition debug \
  --cpus-per-task 52 --mem 0 --cpus 2,4,6,8,10,12,14,16
```

These node and partition names are examples, not defaults. Each node was a Xeon
Gold 6230R with 52 cores; methods used the specified eight-core pool on one socket.
Choose allocation and memory limits for the target cluster. Nodes are exclusive by
default; `--shared-node` opts out where required by site policy.

`slurm.py resume --work ...` retries unfinished stages without replacing completed
measurements. `slurm.py export --work ...` exports an already completed campaign.
The final `shards.json` binds disjoint case bundles; duplicate or missing settings
are rejected.

## Export groups

Local and Slurm workflows export automatically. `export_results.py` supports direct
exports from `--native-run` plus matching stage runs, or from a `--shards` manifest.
Select `--groups` explicitly when only part of the hardware matrix is available:

| Group | Inputs and outputs |
| --- | --- |
| `ablation` | Native batch matrix; incremental-ablation figure |
| `comparison` | Native and external matrices; six-series comparison figure |
| `predecoder` | Three policy timing and three statistics runs; table and 58 macros |
| `m4` | Completed M4 batch matrix; per-round figure |
| `xeon-macros` | Available local timing, PMU and policy results; unavailable keys listed in the audit |
| `macros` | Complete native, L2/L3, policy and M4 results; full performance and predecoder macros |

The export includes `canonical.json`, per-method CSVs, selected configurations,
applicable LaTeX macros and a completion seal. Numeric macros use integer timing and
count operands with final decimal rounding. `--no-render` exports the data without
figures; later `replot.py --input` uses the saved canonical values. It does not add
missing M4, PMU or policy measurements.
