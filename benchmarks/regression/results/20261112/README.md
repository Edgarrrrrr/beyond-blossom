# Migration validation

The Linux x86-64 migration passes the recorded correctness and packaging checks. The measured timing changes remain workload-dependent. Open the [offline report](report/index.html) for the result tables, interactive mechanisms, fixed source excerpts, and code diff.

| Identity | Revision |
| --- | --- |
| Tested implementation | `edd29b6780713dc78f6a99d67c4c4d192e9c52b5` |
| Portable compiler baseline and rebuilt package | `d2506bde8ee9a2eba7ad97523fbc948a97142676` |
| Structured collector | `0ad4fde55adaae41e337a6ee89974703c07bb8cd` |
| Migration source | `241160314087e5ae698c6e76c3c055a11c3ad422` |
| Historical source tag | `source-2411603` at `9651df809d8c2c034717bd3b036c35fda1c90fcb` |
| Fixed official reference | `e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd` |

The historical tag preserves the selected original source and AE helpers; its new repository commit differs from the original upstream commit. The portable compiler fix changes CMake and documentation, with `src/` and `python/` unchanged from the measured implementation. Later documentation and evidence commits do not change the tested runtime.

## Performance

Measurements ran on the Xeon Gold 6230R host `zeus`, with GCC 12.2, Release optimization, profiling disabled, and no LTO. Compilation finished before the serial measurement schedule. Structured parallel methods used physical cores on one socket. See the [hardware record](final-hardware.json) and [frozen protocol](generic-final-protocol.json).

The structured comparison contains 21 settings, six candidate methods, and three repeats: 819 measured invocations including the migration source and fixed official reference. Each setting retains 10,000 input records, reused by all methods and repeats. The [126-row summary](structured/summary.csv), [raw records](structured/raw.csv), and [independent audit](structured-audit/README.md) preserve every setting.

The primary speedup is `official time / candidate time`; its measured median ratios range from 1.0309 to 10.0966. The additional migration ratio is `migration-source time / candidate time`, ranging from 0.9669 to 2.3615. Parallel methods use 2, 4, or 8 cores while the official reference uses one core, as specified in the report.

Of the 126 migration comparisons, 34 have higher candidate median latency. The largest increase is 3.4265% for `component-parallel`, distance 17, error rate 0.003: 20.3364 to 21.0332 microseconds per shot. Three comparisons have non-overlapping observed repeat ranges. No median increase exceeds 5%; this diagnostic threshold does not establish performance equivalence. All increases remain in the [regression audit](structured-audit/README.md).

The arbitrary-graph comparison contains ten workloads, 5,500 saved input records, seven process repeats per variant, and 3,801 raw batch measurements. Candidate predecode improves the median time in nine workloads; the hubs workload increases by 1.0966%. The [generic summary](generic-final-summary.csv) retains source, candidate-off, and candidate-on absolute times and repeat ranges. These synthetic and saved workloads are a migration regression suite; the report does not attribute an isolated speedup to every individual implementation change.

Every measured decoder invocation passed its exact-weight checks. The structured comparison also had zero observable differences. Timing records are batch means, so their repeat ranges do not describe individual-shot tail percentiles.

The separate [boundary-outlining follow-up](boundary-outline/README.md) retains 120 calls across five arms and three selected cases, using 30,000 distinct saved records and eight process repeats. All exact checks passed. The experimental variant was not adopted: the results did not establish that the changed code layout caused the original regressions. The original 819-call matrix remains unchanged, and the two measurement windows are not pooled. These selected cases do not replace coverage of all 126 comparisons.

## Validation

| Check | Recorded result |
| --- | --- |
| Release C++ | 356 tests passed |
| ASan and UBSan C++ | 356 tests passed; leak checking disabled because sandbox ptrace is unsupported |
| Installed wheel Python | 250 tests passed |
| Source package and SDK | Wheel built from extracted sdist; CLI and external CMake consumer passed |
| Portable compiler baseline | 44 linked core objects and the complete library byte-identical with local GCC 12.2; 356 native and two Neighborhood tests passed |
| Rebuilt portable package | Fresh `d2506bde` sdist and wheel; 250 Python tests passed with zero skips; APIs, CLIs and installed CMake consumer passed |
| Final hosted GCC 15 CI | Run and job completed successfully; Python 248 passed, 2 skipped; CTest 355 passed, 1 skipped out of 356 scheduled, 0 failed |
| AE controls | 66 tests and 44 subtests passed |
| Native AE smoke | Seven methods, 1,025 inputs each, zero weight or observable differences |
| External AE smoke | Five methods, 64 inputs each; all 320 parity and correction-chain checks passed |

Receipts are in [validation](validation/). The two exact external methods matched the independently computed optimum; heuristic UnionFind variants retain their observed weight gaps. The native and external smoke timings were collected for workflow validation and are not used as performance evidence.

The [portable-baseline evidence](portable-baseline/README.md) preserves the first CI failure, the local full-test result, and the real Neighborhood translation-unit compile controls. A global x86-64-v3 setting reproduces the target mismatch when the explicit baseline is absent; adding the baseline compiles successfully and produces the same object as the ordinary local build. This establishes the local GCC 12.2 result. Its earlier pending hosted-CI snapshot remains unchanged.

The new [final CI observation](ci-final/README.md) records run `43094516453` and job `130779786311` as `completed/success` for `d2506bde`. All 12 reported steps succeeded. The full log identifies GCC 15.2.0 and shows the compiler's default target query as `x86-64-v3` with `popcnt` enabled. Python collected 250 cases: **248 passed and 2 skipped**. CTest scheduled 356 cases: **355 passed, 1 skipped, and 0 failed**. Its footer says “100% tests passed out of 356,” but test 20, `DecoderStrategy.ParallelSessionEmptyOddExceptionalStreamsAndCapabilityBoundaries`, is explicitly listed as not run. The log does not give that test's skip reason or the two Python skip reasons, so none is inferred. This hosted result is separate from the local 356 executed C++ passes and the local package audit's 250 Python passes with zero skips. The [independent audit](ci-final/audit.json) accounts for every CTest record and preserves log line references.

The [new package audit](package-portable/README.md) validates all 256 selected source files against the committed `d2506bde` snapshot, builds the wheel from the extracted sdist, and tests the installation outside the source tree. Its [summary](package-portable/summary.json) identifies the new source package and wheel hashes. The original package audit remains preserved. Validation covers the recorded Linux x86-64 environment; neither byte identity on GCC 12.2 nor the selected follow-up measurements establish universal compiler or performance equivalence.

The current M4 build, full million-shot paper cohorts, and new hardware PMU collection have not been executed. Their reproduction commands are in [AE](../../../../AE/README.md). Archived paper results retain their original provenance and documented limitations.

## Archived inputs and binaries

[archive-index.json](archive-index.json) identifies the durable workspace archives, hashes, and relocation checks. Large binaries, inputs, and dependency snapshots are outside Git; small raw tables and receipts are committed here. Original receipts retain their original absolute paths. The archive manifests map those paths to preserved files without rewriting the evidence.

The index preserves the six original archives and adds the final [boundary archive receipt](boundary-outline/archive.json), the [portable-baseline manifest](portable-baseline/manifest.json), the [rebuilt-package manifest](package-portable/manifest.json), and the new [final-CI manifest](ci-final/manifest.json). The last three are evidence directories rather than compressed archives; the workspace package directory also contains the new sdist and wheel. The earlier [supplemental verification](supplemental-index-verification.json) retains its checks of 2,495 boundary archive members, 113 workspace directory files, and 97 repository evidence files. The final CI evidence adds eight identically preserved files in each location, under a new manifest; earlier entries, pending observations, and manifests remain unchanged. The final boundary archive SHA256 is `9447d2785b38d7706a9c9ddff87220057965ef56755fc9b13afd3f38398fa5d4`.

The performance archive was extracted into a different directory, and all three summaries were recomputed successfully with byte-identical outputs. From the repository root:

```sh
mkdir -p /path/to/replay
tar -xzf /path/to/validation/20261112/performance.tar.gz -C /path/to/replay
python3 benchmarks/regression/run_structured.py \
  --output /path/to/replay/beyond-build/structured-final-run --summarize-only
python3 benchmarks/regression/run_generic.py \
  --output /path/to/replay/beyond-build/generic-final-fast --summarize-only
python3 benchmarks/regression/run_generic.py \
  --output /path/to/replay/beyond-build/generic-final-dense --summarize-only
```

Fresh collection and smoke commands are in the [regression guide](../../README.md) and [report reproduction guide](report/REPRODUCE.md). Fresh runs use new output directories and retain the original records.
