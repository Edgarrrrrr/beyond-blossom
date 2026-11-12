# External native decoders

This directory builds and runs the five external methods from the decoder comparison. The recovered C++ and Rust runner files are unchanged. `sources.lock.json` identifies every bundled source file and verifies all 60 source entries present in the original build receipts.

| Method | Native implementation | Fixed source |
| --- | --- | --- |
| `parity` | Fusion Blossom `SolverSerial` | 0.2.13, frozen source hashes |
| `fusion` | Fusion Blossom `SolverParallel` | Same frozen source; two time partitions |
| `parity-uf` | `SolverSerial`, `max_tree_size=0` | Same frozen source |
| `unionfind` | UnionFind C++ with open boundaries | `c861819d00a550cceec7dffc280cea60b97fb5d2` plus preserved overlays |
| `ldpc-uf` | LDPC prepared peeling decoder | 2.4.1, `d3429964cd4ffe1abfc041c6ec8b8425cb174f40` plus preserved overlays |

The frozen Fusion Blossom snapshot has no Git metadata; its file hashes are its identity. The historical receipts used floating `cargo +nightly` and did not record compiler versions. New builds record the complete compiler versions, commands, lock file, source commit, library hashes and executable hashes. They do not claim to reproduce an unknown historical compiler.

## Build

Use Python 3.11 or newer, the [AE Python environment](../README.md), a C++20 compiler, and Rust nightly. The Linux smoke build uses nightly-2026-03-10 (`rustc 1.96.0-nightly`, commit `2d76d9bc7`). Cargo always uses the bundled `Cargo.lock` with `--locked`.

First build the fixed official reference with [`AE/build_reference.py`](../build_reference.py). Its output must contain `reference.json`, `upstream/` and `build/pymatching/libpymatching.a`. The independent oracle uses this official library, never the candidate library.

```sh
rustup toolchain install nightly-2026-03-10 --profile minimal
python AE/external/build.py \
  --output /path/to/external-build \
  --reference-build /path/to/official-reference \
  --offline \
  --toolchain nightly-2026-03-10 --jobs 2
```

Output directories must be new and outside the source repository. `build.json` is written only after all selected binaries build successfully. A supplemented reference receipt can be supplied with `--reference-record`; its provenance remains in the build record. To satisfy Clang's narrowing checks, the builder writes a generated UF source with an explicit checked 32-bit graph-size conversion before decoder construction. The original source remains unchanged; the generated file hash is recorded, and the timed function is unchanged.

The repository includes the exact crate archives required by `Cargo.lock`, including packages for other target platforms. `--offline` uses these verified sources without a populated user Cargo cache or network access. The builder obtains fixed Stim paths from the reference receipt. `--crate-cache`, `--stim-source`, and `--stim-archive` remain optional overrides. Install the standard compiler and pinned Rust toolchain before building offline. See the [dependency manifest](../../vendor/README.md).

## Prepare identical inputs

Use a saved AE sparse dataset and its corresponding `model.dem`. Conversion does not sample or decode. It preserves shot order and original shot IDs, merges graph edges with the common non-correlated DEM rule, and retains the original external integer quantization:

```text
q(e) = 2 * max(1, round(weight(e) * 10000 / max_edge_weight))
```

Every boundary edge gets a private virtual vertex. Vertices are sorted by time, and the middle time slice is the interface between the two Fusion partitions. Inputs that cannot satisfy this partition contract are rejected.

```sh
python AE/external/input_adapter.py \
  --dataset /path/to/saved/dataset.bin \
  --dem /path/to/saved/model.dem \
  --output /path/to/external-input \
  --warmup-dataset /path/to/independent-warmup/dataset.bin \
  --repeats 3 --warmup-seconds 2
```

The separate warmup dataset must have the same companion `model.dem`. Without it, `--warmup-prefix 256` reuses the first measured inputs and records that policy. `--limit 10000` selects a prefix while still checking the complete source dataset. Repeating 10,000 inputs three times is recorded as 10,000 independent inputs and three timing repeats.

`input-provenance.json` seals the sparse dataset, DEM, converter implementation and native JSON. `shot-reference.jsonl` preserves the original sparse file's oracle results for traceability. Those results use another integer scale and are not used as the external graph's optimum. The sparse format does not contain physical observable truth, so this path does not report logical failure rates.

For the complete matrix, use a completed `AE/reproduce.py` run. The wrapper checks its plan, completion record, every input hash and every saved dataset's dimensions before launching the external methods:

```sh
python AE/external/collect.py \
  --ae-run /path/to/completed-ae-run \
  --build /path/to/external-build \
  --output /path/to/external-matrix \
  --profile paper --cpus 0,2,4,6
```

The paper profile uses the manuscript's [per-setting sample counts](../REFERENCE.md#external-sample-counts) and three timing repeats. Add `--limit 10000` or `--limit 1000000` for an explicit uniform override. To choose counts per method or case, supply `--shot-counts counts.json`, for example `{"fusion":10000,"d33-p0.003":{"parity":10000}}`. Unspecified entries retain the manuscript count or explicit `--limit`. Counts must be 10,000 or 1,000,000, and the saved case must contain at least that many inputs. The original caption and marker fills establish the default count assignment; they do not supply the missing original timing values.

The smoke profile defaults to at most 64 distinct inputs and one repeat. `--repeats` changes smoke repeats; `--warmup-root /path/to/separate-ae-inputs` selects a separate saved warmup case for every matrix entry. Matrix summaries preserve the source count, selected count, prefix/full status and method-specific timing scope.

## Collect and verify

Commit the complete source tree, build from that clean commit, and keep compilation separate from measurement. Select physical cores on one socket for Linux. The Fusion process uses four available cores; its preserved configuration has two primal workers and one dual worker, with parallel dual execution disabled. Serial methods use one physical core.

```sh
python AE/external/run.py \
  --input /path/to/external-input \
  --build /path/to/external-build \
  --output /path/to/external-run \
  --profile paper --cpus 0,2,4,6
```

Omit `--cpus` to select available physical cores automatically. On macOS, omit it: the launcher sets `USER_INITIATED` QoS on the main thread and records that physical placement is not pinned. macOS collection requires its own build and validation; Linux results are not evidence of macOS performance. `--profile smoke` permits a dirty development tree and labels the result accordingly. `--methods parity fusion` restricts the selected methods without changing inputs or timing windows.

Before timing, the independent native oracle builds the exact integer graph from `q(e)`, computes optimal corrections using official PyMatching revision `e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd`, and exports edge witnesses. The controller independently recomputes their syndrome, weights and observable masks. Every measured native shot checks its syndrome. The first repeat exports correction witnesses for a second, independent check. `parity` and `fusion` must match the oracle's integer optimum; the three heuristic methods report their nonoptimal count and weight gap. Equal-weight observable differences are permitted.

| Method | Timed operations | Operations outside the timer |
| --- | --- | --- |
| Parity, Fusion, Parity UF | `solve` and `subgraph` | Input preparation, validation, `clear`, output |
| UnionFind C++ | `Decoder.decode`, including returned correction copy | Syndrome preparation, edge projection, `clear`, validation, output |
| LDPC UF | `PreparedUfDecoder.decode_prepared` | Initial cluster creation, output reset, correction extraction, cleanup, validation, output |

These are the preserved historical scopes. They are not end-to-end latency measurements. `summary.json` records each scope alongside per-repeat means, the arithmetic mean over all timed shots, inverted-CDF p50/p99, maximum latency, validation counts and weight gaps. Raw per-shot CSVs, correction witnesses, oracle output, executable copies and command records remain available. A failed run retains partial output and writes `failure.json`; it never writes a successful summary.

## Correctness overlays and licenses

The UnionFind overlay supports open boundaries, roots peeling at an unconstrained boundary, and replaces an endpoint hash that caused excessive robin-map probe growth. The LDPC overlay keeps the unconstrained boundary as the peeling root; `PreparedUfDecoder` separates preparation and cleanup from the original growth, merge and peeling operations. The exact upstream diffs are in [`patches/`](patches/). The boundary lattice and preparation wrapper remain separately readable in `sources/`.

Each bundled upstream retains its own license. UnionFind is LGPL-2.1; Fusion Blossom and LDPC are MIT. The complete attribution table and overlay notices are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md). The robin-map and nlohmann JSON headers retain their embedded copyright and license text.

Run the input, output-validation and exhaustive native-oracle checks with:

```sh
BB_EXTERNAL_ORACLE=/path/to/external-build/oracle \
  python -m unittest discover -s AE/external -p 'test_*.py' -v
```
