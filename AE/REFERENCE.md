# Acquisition reference

The artifact contains immutable historical measurements, offline figure generators,
and native collection tools for Beyond Blossom and five external decoders. Commands
run from the repository root. Analysis needs Python 3.10+; the complete acquisition
workflow needs Python 3.11+, CMake 3.21+, a C++20 compiler, OpenMP and Rust nightly.
Builds and measurements use separate directories outside the source checkout.

## Environment and archived results

```sh
AE_WORK=/absolute/path/ae-work
mkdir -p "$AE_WORK"
python3 -m venv "$AE_WORK/venv"
. "$AE_WORK/venv/bin/activate"
python -m pip install '.[stim]' -r AE/requirements.txt
python AE/replot.py --output "$AE_WORK/archived-figures"
python -m unittest discover -s AE -p 'test_*.py'
```

`replot.py` verifies the bundled evidence and writes PDF/SVG/PNG figures, numerical
CSVs, `performance-macros.tex`, `predecoder_numbers.tex` and a completion receipt.
It runs no decoder and needs no original million-shot input files or cluster account.
Fonts use Matplotlib's redistributable DejaVu family; old proprietary-font pixels
are not part of the numerical reproducibility contract. Existing output directories
are rejected.

| Paper result | Archived evidence | Individual offline command |
| --- | --- | --- |
| Xeon incremental ablation, 21 settings | `data/latest-matrix/`, all LTO off | `python AE/plot.py latest-matrix --output "$AE_WORK/ablation-figure"` |
| Xeon timings and speedup macros | Same 882-call matrix | `python AE/migrate_paper_numbers.py --check` |
| Predecoder table, seven distances, p=0.003 | `data/tailreset/`, separate timings and counts | `python AE/generate_predecoder_macros.py --check` |
| L2/L3 miss percentages | `data/pmu/`, raw event counts | `python AE/pmu_data.py verify` |
| M4 per-round throughput, 21 settings | `m4_pro/data/`, original 126 configuration rows | `python AE/m4_pro/plot_m4_per_round.py --output-dir "$AE_WORK/m4-figure"` |
| Historical external comparison | `data/decoders.csv`, 1000 shots per repeat | `python AE/plot.py decoders --output "$AE_WORK/external-figure"` |
| Earlier grid/cache/distribution study | `data/grid/` | `python AE/plot.py grid --output "$AE_WORK/grid-figures"` |
| Earlier mixed-LTO ablation | `data/ablation.json` | `python AE/plot.py ablation --output "$AE_WORK/older-ablation"` |

Archived figures and macros always use their recorded source data. Fresh collection
writes new evidence and never replaces these files. See [measurement provenance](data/SOURCES.md)
for the sample-count, timing-scope and table-label discrepancies that limit paper claims.

## Fixed dependencies and reference

The independent reference is upstream release 2.3.1 at
`e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd`. It is built separately from the
current unstructured backend. The historical tag `source-2411603` contains the
original acquisition adapter; `historical_sources.json` checks every recovered file.
Fetch that tag explicitly after a shallow clone.

```sh
git fetch origin tag source-2411603
git clone https://github.com/oscarhiggott/PyMatching.git "$AE_WORK/upstream"
git clone https://github.com/quantumlib/Stim.git "$AE_WORK/stim"
git -C "$AE_WORK/stim" checkout --detach 1320ad7eac7de34d2e9c70daa44fbc6d84174450
git clone https://github.com/google/googletest.git "$AE_WORK/googletest"
git -C "$AE_WORK/googletest" checkout --detach 58d77fa8070e8cec2dc1ed015d66b454c8d78850
cmake -S "$AE_WORK/stim" -B "$AE_WORK/stim-build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
cmake --build "$AE_WORK/stim-build" --target libstim -j 2
python AE/build_reference.py \
  --upstream-repository "$AE_WORK/upstream" \
  --history-checkout . --history-ref source-2411603 \
  --stim-source "$AE_WORK/stim" --stim-archive "$AE_WORK/stim-build/out/libstim.a" \
  --googletest-source "$AE_WORK/googletest" --jobs 2 --output "$AE_WORK/reference"
```

The output seals the original source, adapted acquisition wrapper, dependencies,
static library and executable in `reference.json`. Platform adaptation changes only
acquisition code. `--history-archive /path/to/original-source.tar.gz` is an alternative
to the historical tag. Recovery for source inspection is also available:

```sh
python AE/recover_history.py --checkout . --ref source-2411603 \
  --output "$AE_WORK/historical-tools"
```

## Candidate build and full native matrix

Commit the complete source before a paper build. `build_native.py` seals the source
commit and file hashes before and after compilation, compiler/configuration,
Stim inputs, commands and binary hash. A paper run requires this receipt and the
fixed reference receipt. `--profile smoke` permits a development tree; omitting
receipts then produces an integration check with explicitly weaker provenance.

```sh
python AE/build_native.py --output "$AE_WORK/native" --jobs 2 \
  --stim-source "$AE_WORK/stim" --stim-archive "$AE_WORK/stim-build/out/libstim.a"
python AE/reproduce.py run --profile paper --backend avx512vl \
  --binary "$AE_WORK/native/bin/bb_benchmark" --build-record "$AE_WORK/native/build.json" \
  --reference-binary "$AE_WORK/reference/build/bin/experiment_benchmark" \
  --reference-record "$AE_WORK/reference/reference.json" \
  --output "$AE_WORK/native-batch"
python AE/reproduce.py analyze "$AE_WORK/native-batch"
python AE/reproduce.py run --profile paper --backend avx512vl --measurement latency \
  --binary "$AE_WORK/native/bin/bb_benchmark" --build-record "$AE_WORK/native/build.json" \
  --reference-binary "$AE_WORK/reference/build/bin/experiment_benchmark" \
  --reference-record "$AE_WORK/reference/reference.json" \
  --input-run "$AE_WORK/native-batch" --output "$AE_WORK/native-latency"
```

The default paper matrix uses d=9,13,17,21,25,29,33, p=0.0005,0.001,0.003,
one million saved independent shots per setting and three timing passes. Circuits
are rotated memory-X with d rounds, the complete X/Z detector stream, and all four
circuit-level noise probabilities equal to p. Seven methods are collected:
`reference`, `unstructured`, `structured`, `predecoded`, `component-parallel`,
`pipelined` and `parallel-predecode`. The six paper stages use 1/1/1/2/4/8 cores;
`unstructured` is an additional current-code regression measurement.

`--input-run` verifies and copies a completed run's saved circuit, DEM and sparse
inputs. Add `--link-inputs` to reuse the verified files with hard links where
supported; cross-device reuse falls back to copying and records the actual method.
Neither mode changes the source files. It preserves original seed and stream IDs, including when selecting only
some distances or error rates, and performs no sampling. Reused inputs must retain
the original shot count. This also supports implementation migration comparisons.

Timing includes the complete native decode, result staging, reset and pipeline
drain. Construction, saved-input loading, warmup, validation and output are excluded.
Weight differences fail the run; observable ties require the native witness check.
`plan.json`, `calls/`, `complete.json` and `summary.csv` retain identities, commands,
placement, correctness and raw timing. Both pooled means and median-of-three batch
means are reported. Repeated passes do not increase the independent sample count.

Linux automatically selects distinct physical cores on one socket; `--cpus` overrides
the selection. All seven methods need eight cores. A smaller smoke command is:

```sh
python AE/reproduce.py run --profile smoke --backend scalar \
  --binary "$AE_WORK/native/bin/bb_benchmark" \
  --methods unstructured,structured,predecoded --output "$AE_WORK/native-smoke"
```

Smoke defaults to 1025 inputs at d=9, p=0.003, one pass and at least two seconds of
warmup per call. Without an independent reference, its expected results come from
the candidate's generic path. The recorded Xeon platform was Gold 6230R, GCC 12.2,
C++20, `-O3`, AVX-512VL/AVX2 and LTO off; use exclusive cores for new timing claims.

## External native comparison

[External decoder instructions](external/README.md) document source identities,
correctness overlays, offline Cargo builds and each original timing window. The
five native methods are Parity Blossom, Fusion Blossom, Parity UF, UnionFind C++
and LDPC UF. Their correction witnesses are checked against an independent native
oracle linked to the fixed official reference.

```sh
rustup toolchain install nightly-2026-03-10 --profile minimal
python AE/external/build.py --output "$AE_WORK/external-build" \
  --reference-build "$AE_WORK/reference" \
  --stim-source "$AE_WORK/stim" --stim-archive "$AE_WORK/stim-build/out/libstim.a" \
  --toolchain nightly-2026-03-10 --jobs 2
python AE/reproduce.py external --profile paper \
  --ae-run "$AE_WORK/native-batch" --build "$AE_WORK/external-build" \
  --output "$AE_WORK/external-million"
BB_EXTERNAL_ORACLE="$AE_WORK/external-build/oracle" \
  python -m unittest discover -s AE/external -p 'test_*.py' -v
```

The wrapper consumes all 21 sealed native input cases. Paper mode defaults to one
million distinct inputs and three repeats for every method. `--limit 10000` selects
the same 10,000-shot prefix for every external method. `--shot-counts counts.json`
can instead provide explicit method defaults and case-specific overrides:

```json
{"parity": 1000000, "d33-p0.003": {"fusion": 10000}}
```

These options define new collections. The manuscript does not identify the original
method/setting assignment of its million-shot and 10,000-shot markers, and that
assignment cannot be inferred from the archived 1000-shot CSV. Every fresh row records
its actual distinct count, full/prefix status, three repeats and native timing scope.
Default warmup reuses a measured prefix and records that policy. `--warmup-root`
accepts a separate saved AE `inputs/` directory with matching cases and DEMs.

The external scopes differ from the complete-decode scope above: Parity/Fusion time
`solve + subgraph` and exclude `clear`; UnionFind excludes syndrome preparation,
edge projection and `clear`; LDPC UF also excludes cluster initialization and cleanup.
The sparse inputs lack physical observable truth, so this path reports matching
optimality and correction validity, not logical failure rates.

## Cache counters and predecoder policies

PMU collection is separate from timing and Linux-only. It requires perf permissions
and the matching CPU events. L2 uses Cascade Lake `L2_RQSTS.ALL_DEMAND_DATA_RD` and
`L2_RQSTS.DEMAND_DATA_RD_MISS`; L3 uses `LLC-loads` and `LLC-load-misses`. The metric is
`100 * misses / requests`. Counters cover the calling user-space thread; unsupported
or multiplexed groups fail. Build both reference and candidate at the same level:

```sh
for level in 2 3; do
  python AE/build_reference.py --cache-level "$level" --jobs 2 \
    --upstream-repository "$AE_WORK/upstream" --history-checkout . --history-ref source-2411603 \
    --stim-source "$AE_WORK/stim" --stim-archive "$AE_WORK/stim-build/out/libstim.a" \
    --googletest-source "$AE_WORK/googletest" --output "$AE_WORK/reference-l$level"
  python AE/build_native.py --cache-level "$level" --jobs 2 --output "$AE_WORK/native-l$level" \
    --stim-source "$AE_WORK/stim" --stim-archive "$AE_WORK/stim-build/out/libstim.a"
  python AE/reproduce.py run --profile paper --measurement pmu --repeats 1 \
    --binary "$AE_WORK/native-l$level/bin/bb_benchmark" --build-record "$AE_WORK/native-l$level/build.json" \
    --reference-binary "$AE_WORK/reference-l$level/build/bin/experiment_benchmark" \
    --reference-record "$AE_WORK/reference-l$level/reference.json" \
    --methods reference,structured --distances 33 --error-rates .003 --backend avx512vl \
    --input-run "$AE_WORK/native-batch" --output "$AE_WORK/pmu-l$level"
done
```

Fresh collection follows the manuscript's serial predecoder table. Build modes 0/1/3
select degree ordering, no predecoding and simultaneous equal-share growth. Mode 2
retains the historical coordinate-order, full-cap policy. The export roles 0/1/2
map to build modes 0/1/3 for the new table. The loop
below measures all three on the same saved p=0.003 inputs, then separately collects
untimed residual counters. Counter-enabled binaries reject timing modes. The archived
table used a four-thread pipeline; that historical scope remains explicitly labeled.

```sh
for variant in 0:0 1:1 2:3; do
  policy=${variant%:*}
  mode=${variant#*:}
  python AE/build_native.py --predecode-mode "$mode" --jobs 2 \
    --stim-source "$AE_WORK/stim" --stim-archive "$AE_WORK/stim-build/out/libstim.a" \
    --output "$AE_WORK/policy-$policy"
  python AE/reproduce.py run --profile paper --methods predecoded --error-rates .003 \
    --binary "$AE_WORK/policy-$policy/bin/bb_benchmark" --build-record "$AE_WORK/policy-$policy/build.json" \
    --reference-binary "$AE_WORK/reference/build/bin/experiment_benchmark" \
    --reference-record "$AE_WORK/reference/reference.json" --backend avx512vl \
    --input-run "$AE_WORK/native-batch" --output "$AE_WORK/policy-$policy-timing"
  python AE/build_native.py --predecode-mode "$mode" --statistics --jobs 2 \
    --stim-source "$AE_WORK/stim" --stim-archive "$AE_WORK/stim-build/out/libstim.a" \
    --output "$AE_WORK/policy-$policy-counts-build"
  python AE/reproduce.py run --profile paper --measurement statistics --repeats 1 \
    --methods predecoded --error-rates .003 --backend avx512vl \
    --binary "$AE_WORK/policy-$policy-counts-build/bin/bb_benchmark" \
    --build-record "$AE_WORK/policy-$policy-counts-build/build.json" \
    --reference-binary "$AE_WORK/reference/build/bin/experiment_benchmark" \
    --reference-record "$AE_WORK/reference/reference.json" \
    --input-run "$AE_WORK/native-batch" --output "$AE_WORK/policy-$policy-counts"
done
```

The simple baseline caps each active-active edge contribution at half its weight,
retains the full bound for inactive neighbors and boundaries, and rounds down to
the decoder's even-radius lattice. It publishes all caps before coordinate-order
tight-edge matching. It performs no degree ordering or additional partner growth.

`statistics.csv` reports integer totals, mean residuals and matched percentages.
The analyzer requires normalized = residual + 2*pairs + boundaries. No statistics
call contributes a timing value.

## M4 acquisition

Repeat dependency and reference builds locally on Apple M4 Pro using Apple Clang
and an installed OpenMP package. Supply `--cxx-flags="-O3 -mcpu=apple-m4"` and
`--cmake-arg=-DCMAKE_PREFIX_PATH=/path/to/libomp` to the reference builder when needed.
Use fresh native M4 build and run directories:

```sh
python AE/build_reference.py --output "$AE_WORK/reference-m4" --jobs 2 \
  --upstream-repository "$AE_WORK/upstream" --history-checkout . --history-ref source-2411603 \
  --stim-source "$AE_WORK/stim" --stim-archive "$AE_WORK/stim-build/out/libstim.a" \
  --googletest-source "$AE_WORK/googletest" \
  --cxx-flags="-O3 -mcpu=apple-m4" --cmake-arg=-DCMAKE_PREFIX_PATH=/path/to/libomp
python AE/build_native.py --output "$AE_WORK/native-m4" --jobs 2 \
  --stim-source "$AE_WORK/stim" --stim-archive "$AE_WORK/stim-build/out/libstim.a" \
  --cxx-flags="-O3 -mcpu=apple-m4" --cmake-arg=-DCMAKE_PREFIX_PATH=/path/to/libomp
python AE/reproduce.py run --profile paper --backend scalar \
  --binary "$AE_WORK/native-m4/bin/bb_benchmark" --build-record "$AE_WORK/native-m4/build.json" \
  --reference-binary "$AE_WORK/reference-m4/build/bin/experiment_benchmark" \
  --reference-record "$AE_WORK/reference-m4/reference.json" --output "$AE_WORK/m4-batch"
```

The predecoder's Arm kernel is selected by the platform build. Acquisition requests
USER_INITIATED QoS and records physical affinity as unverified. The M4 aggregation
is the median of three complete-batch means; divide a setting's block time by d to
obtain time per measurement round. Historical CSV recalculation is supported;
Linux smoke tests do not establish a successful current M4 build or new M4 timings.

## Fresh figures, tables and macros

The fresh exporter reads completed runs, verifies their sealed artifacts, and writes
the current article's figures and numeric macros. Missing settings are never filled
from archives. Xeon runs must share a source commit, saved inputs and host; M4 runs
must identify Apple Silicon and the same source commit. Existing output is rejected.

```sh
python AE/export_results.py --profile paper \
  --native-run "$AE_WORK/native-batch" --external-run "$AE_WORK/external-million" \
  --l2-run "$AE_WORK/pmu-l2" --l3-run "$AE_WORK/pmu-l3" \
  --policy-timing "0=$AE_WORK/policy-0-timing" "1=$AE_WORK/policy-1-timing" "2=$AE_WORK/policy-2-timing" \
  --policy-counts "0=$AE_WORK/policy-0-counts" "1=$AE_WORK/policy-1-counts" "2=$AE_WORK/policy-2-counts" \
  --m4-run "$AE_WORK/m4-batch" --output "$AE_WORK/fresh-paper"
```

Outputs include `canonical.json`, per-method CSVs, selected configurations,
`performance-macros.tex`, `predecoder_numbers.tex`, a calculation audit and a sealed
completion receipt. Numeric macros use raw integer timing/count operands and final
decimal rounding. Speedup is the fixed official reference time divided by the
candidate time; values above one mean the candidate is faster. The
selection pool is explicitly `structured`, `predecoded`, `component-parallel`,
`pipelined`, and `parallel-predecode`; the additional `unstructured` regression
column is not substituted for the reference or a structured variant.

| Group | Fresh outputs | Required runs |
| --- | --- | --- |
| `ablation` | `incremental_ablation_1107_v1` PDF/SVG/PNG | Native batch matrix |
| `comparison` | `decoder_comparison_shows_speedup_v3` PDF/SVG/PNG | Native and external matrices on the same saved inputs |
| `predecoder` | `decoding_time`, `remaining_syndromes`, `predecoder.csv`, 58 table macros | All three serial policy timings and all three untimed statistics runs; historical pipeline runs must use a consistent recorded scope |
| `m4` | `m4_pro_per_round` PDF/SVG/PNG | M4 batch matrix, median of three passes |
| `xeon-macros` | Available Xeon performance and predecoder macros, omitted-key audit | Native and supplied local PMU/policy runs; no M4 requirement |
| `macros` | Complete performance and 58 predecoder macros, audit | Native, both PMU levels, all policy timing/count runs and M4 |

Groups can be exported separately before cross-platform collection finishes:

```sh
python AE/export_results.py --native-run "$AE_WORK/native-batch" \
  --groups ablation --output "$AE_WORK/fresh-ablation"
python AE/export_results.py --native-run "$AE_WORK/native-batch" \
  --external-run "$AE_WORK/external-million" --groups comparison \
  --output "$AE_WORK/fresh-comparison"
python AE/export_results.py --native-run "$AE_WORK/native-batch" \
  --policy-timing "0=$AE_WORK/policy-0-timing" "1=$AE_WORK/policy-1-timing" "2=$AE_WORK/policy-2-timing" \
  --policy-counts "0=$AE_WORK/policy-0-counts" "1=$AE_WORK/policy-1-counts" "2=$AE_WORK/policy-2-counts" \
  --groups predecoder --output "$AE_WORK/fresh-predecoder"
python AE/export_results.py --native-run "$AE_WORK/native-batch" \
  --m4-run "$AE_WORK/m4-batch" --groups m4 --output "$AE_WORK/fresh-m4"
```

`--no-render` exports verified tables and macros without drawing. `--profile smoke`
permits small development runs and marks their figures accordingly; it does not
establish paper performance. External marker fill records the actual shot count,
and the comparison caption preserves the differing native timing scopes. Replacing
article figures remains a deliberate operation; the exporter never edits the paper.

## Evidence boundaries

Historical identifiers and checksums remain unchanged. A historical receipt describes
its original run, not validation of the current implementation. New timings require
new completed runs on the stated hardware. The commands above collect and plot fresh
native, PMU, predecoder, M4 and external results. Archived plotting remains bound to
its original datasets. The three older grid diagnostic figures and the mixed-LTO
ablation are supplemental archived experiments, not figures referenced by the current
article; `replot.py` reproduces them without fabricating new measurements for their
different PMU protocols.
