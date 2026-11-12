# Measurement provenance

The CSV/JSON files in this directory are immutable historical measurements.
Names, source revisions, original paths and measurement hashes identify the
experiments as collected. The public implementation uses the Beyond Blossom
package and APIs; historical identifiers are retained so the evidence remains
verifiable.

- `latest-matrix/SOURCE.json`: all-LTO-OFF Xeon matrix, 21 settings, seven native
  entries, three batch and three latency calls per entry; 882 calls.
- `tailreset/SOURCE.json`: three predecoding strategies and a fixed upstream
  reference, seven distances at p=0.003; 133 calls. Timing and residual statistics
  use separate binaries. The measured execution is the four-thread pipeline.
- `pmu/SOURCE.json`: independent L2 and L3 counters, 21 settings and two methods
  per level. The rate is misses divided by the specified request event.
- `ablation.json`: older mixed-LTO experiment. It is not the all-LTO-OFF matrix.
- `decoders.csv`: older external-decoder comparison, 1000 independent inputs
  per setting and three passes. It is not a million-input comparison.
- `grid/`: earlier latency distributions and cache diagnostics; the original
  metrics and event definitions remain in the source records.
- `../m4_pro/data/`: original M4 configuration rows and median-of-three timings.

`SHA256SUMS` records the original bundled data checksums. Dataset-specific
manifests connect individual receipts by relative paths and hashes. Original
absolute paths are provenance labels, not dependencies of offline analysis.
Historical source scripts and narrative snapshots omitted from the distribution
can be restored using `../historical_sources.json` and `../recover_history.py`.
Original manifest entries referring to those snapshots remain unchanged.

## Manuscript reconciliation

The archived external CSV contains 1000 shots per repeat. The comparison caption
and plotted marker fills declare a mixed one-million / ten-thousand-shot policy;
the [per-setting table](../REFERENCE.md#external-sample-counts) records that assignment.
The archived CSV cannot establish the larger experiment or its missing timings.
Fresh `reproduce.py external --profile paper` collection follows the declared
mixed counts and three timing repeats. `--limit` or `--shot-counts` explicitly
overrides that policy. Fresh outputs remain new measurements, not replacements
for missing original data.

The recovered native external timers also exclude operations included by the
manuscript's blanket timing description: Parity/Fusion exclude `clear`, UnionFind
excludes syndrome preparation and `clear`, and LDPC UF excludes cluster creation
and cleanup. Exact scopes accompany every fresh result in `external/README.md` and
the collection receipts. Comparing retained historical native scopes requires that
qualification; claiming identical end-to-end latency would require different adapters
and new measurements.

The archived predecoder table and macros derive from four-thread pipelined execution,
whereas the evaluation prose and caption call the experiment serial. Retaining the
archived values requires describing the four-thread pipeline. A serial experiment
requires fresh timing and residual-count evidence with an explicitly serial protocol.
The current workflow collects serial timing and residual counts separately, using
physical modes 0/1/3 for degree-ordered, no predecoding, and simple equal-rate policies.

The M4 source data supports the archived median-of-three calculations, including
18/21 configurations below one microsecond per round versus 11/21 for the fixed
reference. A current-build M4 hardware run remains necessary before assigning those
results to the renamed implementation.
