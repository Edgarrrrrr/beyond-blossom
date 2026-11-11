# Generic decoder regression

This matrix compares the exact migration source, candidate predecode disabled, and candidate predecode enabled. The source revision is `241160314087e5ae698c6e76c3c055a11c3ad422` from `Edgarrrrrr/PyMatching-experiments`, branch `test/m4-code-ae-merged-20261112`.

The workload set includes saved surface-code shots, sparse irregular graphs, degree-64 graphs, dense syndromes, and a ring with no adjacent syndrome seeds. All variants read identical saved inputs. Graph weights use the even integer lattice produced by `UserGraph`.

`generate_holdouts.py` creates an independent corpus with a different random seed and weight distribution: active high-degree hubs, a concentrated community syndrome, and a dense active component surrounded by 8064 disconnected inactive nodes. It uses the same runner interface as `generate_corpora.py`.

## Build

Set `SOURCE_SNAPSHOT` to the extracted source revision and run from the Beyond Blossom repository root. The two builds must use the same compiler, architecture flags, Stim revision, OpenMP settings, and optimization level. LTO and profiling are disabled.

```bash
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

`build_benchmarks.py` reads each library's Makefile flags and rejects LTO flags. Archive both CMake caches, compile commands, source identities, and benchmark binaries with the run. Explicit FetchContent source directories can supply the pinned dependencies offline.

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

Choose an available CPU from the process affinity set. Finish compilation and other CPU-intensive work before timing. A smoke run uses `--repeats 1` and a fresh output directory. Full runs retain seven repetitions; repetition reuses the same input records and does not increase the number of independent shots.

Each process warms the entire corpus before timing. The timed interval contains decoding and result checksums; graph construction, file reads, and initial allocation are excluded. Variant order reverses on alternate rounds. Matching-weight checksums must agree across all three variants. Independent exhaustive correctness tests also verify individual optimal weights and syndrome parity.

The first source invocation saves every shot's exact weight in a signed 64-bit little-endian vector. Every subsequent invocation compares each weight to that vector outside the timed interval and fails at the first mismatch. The ordered weight hash remains an additional run-consistency check. The native options are `--write-weights path` and `--expected-weights path`.

Each run writes immutable raw timing rows, input and binary hashes, commands, environment metadata, stderr logs, and a derived summary. Timing records contain batch means in ns/shot; they do not establish individual-shot percentiles. Recompute the summary with:

```bash
python3 benchmarks/regression/run_generic.py --output results/generic-regression/run-001 --summarize-only
```

The summary reports median batch latency, observed repetition ranges, and speedup as `source / candidate`. The on/off comparison isolates the predecoder; the source/off comparison includes shared fixes and the radix-queue bitmap. Regressions remain visible for each workload. These runs measure the listed inputs and machine; they do not imply universal performance equivalence.
