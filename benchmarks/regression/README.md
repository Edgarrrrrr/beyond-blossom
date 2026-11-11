# Generic decoder regression

This matrix compares the exact migration source, candidate predecode disabled, and candidate predecode enabled. The source revision is `241160314087e5ae698c6e76c3c055a11c3ad422` from `Edgarrrrrr/PyMatching-experiments`, branch `test/m4-code-ae-merged-20261112`.

The workload set includes saved surface-code shots, sparse irregular graphs, degree-64 graphs, dense syndromes, and a ring with no adjacent syndrome seeds. All variants read identical saved inputs. Graph weights use the even integer lattice produced by `UserGraph`.

`generate_holdouts.py` creates an independent corpus with a different random seed and weight distribution: active high-degree hubs, a concentrated community syndrome, and a dense active component surrounded by 8064 disconnected inactive nodes. It uses the same runner interface as `generate_corpora.py`.

## Build

Extract the source from the public `source-2411603` tag and run from the Beyond Blossom repository root. The two builds must use the same compiler, architecture flags, Stim revision, OpenMP settings, and optimization level. LTO and profiling are disabled.

```bash
git fetch --tags
mkdir -p build/source-snapshot
git archive source-2411603 | tar -x -C build/source-snapshot
SOURCE_SNAPSHOT="$PWD/build/source-snapshot"
cmake -S "$SOURCE_SNAPSHOT" -B build/source -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DPYMATCHING_BUILD_PROFILE_TOOLS=OFF \
  -DPYMATCHING_BUILD_TESTS=OFF
cmake --build build/source --target libpymatching -j4
cmake -S . -B build/candidate -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DBB_BUILD_PROFILE_TOOLS=OFF \
  -DBB_BUILD_TESTS=ON -DBB_BUILD_PYTHON=OFF
cmake --build build/candidate --target libbeyond_blossom beyond_blossom_tests -j4
python3 benchmarks/regression/build_benchmarks.py build/source build/candidate \
  benchmarks/regression/generic_benchmark.cc build/regression-bin
```

`build_benchmarks.py` reads each library's compiler and Makefile flags, rejects LTO flags, and records commands and source, flag, library, and binary hashes in `build.json`. Archive both CMake caches, compile commands, source identities, and benchmark binaries with the run. Explicit FetchContent source directories can supply the pinned dependencies offline.

## Inputs and execution

```bash
python3 benchmarks/regression/generate_corpora.py build/regression-inputs
build/candidate/beyond_blossom_tests --gtest_filter='UnstructuredPredecode.*:radix_heap_queue.*'
python3 benchmarks/regression/run_generic.py \
  --baseline-bin build/regression-bin/baseline --candidate-bin build/regression-bin/candidate \
  --corpora build/regression-inputs \
  --surface-dem data/surface_code_rotated_memory_x_13_0.01.dem \
  --surface-shots data/surface_code_rotated_memory_x_13_0.01_1000_shots.b8 \
  --candidate-identity '<actual commit or frozen snapshot hash>' \
  --cpu 0 --repeats 7 --output results/generic-regression/run-001
```

Choose an available CPU from the process affinity set. Finish compilation and other CPU-intensive work before timing. A smoke run uses `--repeats 1` and a fresh output directory. Full runs retain seven balanced process launches per variant. For short batches, add `--batches-per-process 20` to retain twenty native timing passes per process. Both forms of repetition reuse the same input records and do not increase the number of independent shots.

Each process warms the entire corpus before timing. The timed interval contains decoding and result checksums; graph construction, file reads, and initial allocation are excluded. Variant order reverses on alternate rounds. Matching-weight checksums must agree across all three variants. Independent exhaustive correctness tests also verify individual optimal weights and syndrome parity.

The first source invocation saves every shot's exact weight in a signed 64-bit little-endian vector. Every subsequent invocation compares each weight to that vector outside the timed interval and fails at the first mismatch. The ordered weight hash remains an additional run-consistency check. The native options are `--write-weights path` and `--expected-weights path`.

The candidate also records `complete_shots` and `skipped_shots` during warmup. The predecoder skips its full adjacency scan when the normalized seeds have mean degree at least 32, no direct boundary, and fewer than one eighth of sampled neighbors are active. It samples up to 32 neighbors in at most `min(8, ceil(seeds / 4))` neighborhoods, bounding both total work and the cost of dispatch on short shots. It then seeds the full solver directly. The sample uses current local neighborhoods, so unrelated or disconnected graph vertices do not affect dispatch. This heuristic changes performance only; every selected path still solves the full matching problem.

Each run writes immutable raw timing rows, input and binary hashes, commands, environment metadata, stderr logs, and a derived summary. Timing records contain batch means in ns/shot; they do not establish individual-shot percentiles. Recompute the summary with:

```bash
python3 benchmarks/regression/run_generic.py --output results/generic-regression/run-001 --summarize-only
```

Raw records identify both `matrix_repeat` and `native_repeat`. The summary first takes the median of native batches within each process, then reports the median and observed range of those process medians, with speedup as `source / candidate`. The on/off comparison isolates the predecoder; the source/off comparison includes shared fixes and the radix-queue bitmap. Regressions remain visible for each workload. These runs measure the listed inputs and machine; they do not imply universal performance equivalence.

Collector controls use synthetic rows and perform no native timing:

```bash
python3 -m unittest discover -s benchmarks/regression -p test_run_generic.py
```

## Structured adapter comparison

The existing native adapters can link directly to the same release libraries. This preserves their saved-input and timing interfaces without rebuilding either library.

```bash
python3 benchmarks/regression/build_structured.py --source-snapshot "$SOURCE_SNAPSHOT" \
  --source-build build/source --candidate-build build/candidate \
  --output build/structured-regression-bin
```

The helper reads each library's actual compiler flags, rejects LTO and profiling builds, and enables shared and pipeline adapters with OpenMP adapter selection disabled. The TailReset witness adapter is enabled; syndrome statistics remain disabled. `build.json` records full commands and adapter, header, library, and binary hashes. Both executables retain the native `--dataset`, `--backend`, `--execution`, and timing-mode options used by the AE workflow.

Run the preservation matrix after all compilation and other timing jobs have finished:

```bash
python3 benchmarks/regression/run_structured.py \
  --build build/structured-regression-bin \
  --reference-binary /path/to/fixed-official-reference/build/bin/experiment_benchmark \
  --reference-record /path/to/fixed-official-reference/reference.json \
  --cpus 0,2,4,6,8,10,12,14 \
  --output /path/to/structured-regression/run-001
```

The preparation executable must match an [`AE/build_reference.py`](../../AE/build_reference.py) receipt for official revision `e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd`. It prepares each case once and saves the exact detector records, integer optimum, DEM and circuit. Both migration variants read that same saved file and check weights outside timing. Equal-weight observable differences require actual candidate correction witnesses.

Defaults cover distances 9, 13, 17, 21, 25, 29 and 33 at error rates 0.0005, 0.001 and 0.003, with 10,000 distinct shots and three timing repeats. The six methods are `unstructured`, `structured`, `predecoded`, `component-parallel`, `pipelined` and `parallel-predecode`; their core counts are 1, 1, 1, 2, 4 and 8. Structured methods default to AVX-512VL. The unstructured method uses each binary's native generic entry and default implementation. Select a supported alternative with `--backend scalar`, `avx2` or `avx512`.

All native processes run serially. Source and candidate calls are adjacent; their order reverses each repeat, while method order rotates. Each process performs a two-second native warmup in its resident team. The full default matrix has 756 measured invocations, so warmup alone takes at least 25.2 minutes. For a small control run, pass `--distances 9 --error-rates 0.003 --shots 32 --repeats 1`; the warmup contract remains two seconds.

This runner labels development snapshots explicitly and does not require a clean checkout by default. Optional `--source-identity` and `--candidate-identity` JSON receipts identify frozen source snapshots; their contents and hashes are preserved. Without a candidate receipt, the checkout identity is recorded as an observation at collection time, while the builder receipt and executable hash identify the compiled snapshot. `--require-clean` additionally rejects a dirty controller checkout.

Every run retains executable copies, the builder receipt, available CMake caches and compile commands, input hashes, native stdout logs, native timing and correctness artifacts, and a raw CSV. A changed binary, corpus or controller, failed correctness check, incomplete warmup or missing measurement prevents a successful summary. No aggregate replaces the per-case results: `summary.csv` contains source and candidate median batch means, observed minimum and maximum, `source/candidate` speedup, candidate percentage change, and paired-repeat speedup ranges. Speedup below one indicates a measured slowdown; these ranges are not confidence intervals or individual-shot percentiles.

Verify and recompute the summaries from sealed artifacts with:

```bash
python3 benchmarks/regression/run_structured.py \
  --output /path/to/structured-regression/run-001 --summarize-only
```

The collector control-flow tests use mocked native output and perform no timing:

```bash
python -m pytest benchmarks/regression/test_run_structured.py -q
```
