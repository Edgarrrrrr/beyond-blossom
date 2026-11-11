# Generic hubs dynamic diagnosis: engineering decision

The diagnosis confirms a small, repeatable predecode prefix cost. It does not
identify a correctness defect or a sufficiently supported, necessary code fix.
The recommended engineering endpoint is to retain the original measured positive
and negative results and finish the evidence review, without another speculative
implementation or measurement round. This recommendation is not a declaration of
statistical equivalence or acceptance of a performance regression on the user's
behalf.

## Integrity and scope

The frozen plan completed all 72 processes and 1,440 batches. Independent replay
rechecked every saved result and counter contract and reproduced `summary.json`,
`process-medians.json` and `raw.csv` byte for byte. There are 288,000 measured shot
records but only 200 distinct saved inputs: 192,000 candidate records match both
weight and observable; 96,000 source/official records match weight, with zero
observed observable differences. All 1,920 PMU snapshots and 720,000 raw clock
calibration values passed validation. No rows or processes were discarded.

## Observations

All times below are per shot. Each process contributes its median over twenty
batch means; ranges cover the six process medians, not independent inputs.

| Condition | Median | Process range |
|---|---:|---:|
| Candidate normal, predecode off, PMU off | 75.818 us | 75.012–76.127 us |
| Candidate normal, predecode on, PMU off | 76.379 us | 73.985–78.382 us |
| Source241 normal, PMU off | 85.943 us | 84.996–86.648 us |
| Official normal, PMU off | 85.641 us | 85.031–88.830 us |

Candidate normal on/off without PMU changes the ratio of medians by +0.7400%.
Five of six paired process differences are positive; their median is +778 ns
(+1.0265%), with a range from −1,694 to +2,256 ns. With PMU, every paired process
has exactly +2,823 retired instructions per shot (+0.64345%); cycles also increase
in all six pairs, by +417 to +3,001 cycles (paired median +1,111 cycles). This is
evidence of extra executed work, not merely a noisy latency sign.

The clocked split paths localize that work:

| Phase | Enabled median | Disabled median / paired difference |
|---|---:|---:|
| prepare | 127.36 ns | absent when disabled; enabled range 123.64–133.03 ns |
| gate | 249.07 ns | absent when disabled; enabled range 240.94–256.82 ns |
| root creation | 3,184.54 ns | 3,241.09 ns; paired difference median −32.97 ns |
| prepare + gate + roots | 3,569.20 ns | 3,241.09 ns; paired difference +258.97…409.49 ns, all six positive |
| solve + extract | 73,068.29 ns | 72,939.27 ns; paired differences −877.36…1,094.94 ns, three positive and three negative |

The difference of prefix medians is +328.11 ns; the median of paired prefix
differences is +347.39 ns. These are different summaries and are not exchanged.
The observations do not establish a fixed slowdown in solve/extract itself.

## Control disturbances and original-performance boundary

PMU-on versus PMU-off normal latency changes by +0.414%, +0.878%, +0.644% and
+0.939% in ratios of medians for candidate-off, candidate-on, source and official,
respectively; individual paired differences have both signs. Copied split versus
normal changes by +0.858% for predecode-off and −0.587% for predecode-on. Adding
stage clocks to split changes by −0.638% and +0.179%, respectively. Therefore
phase timing is not an unbiased decomposition of the original driver, and clock
calibration values must not be mechanically subtracted. PMU instructions cover
the whole batch and cannot be assigned to individual phases.

The original formal hubs result remains source 85.889 us, off 85.788 us, on
86.831 us: on/source +1.09665%, with overlapping process ranges. Its seven paired
on/source changes have median +0.18667% and five positive signs. Neither overlap
nor mixed signs establishes equivalence. The new diagnostic adapter's candidate
is roughly 11% faster than the reference in its own window; this does not erase
the original result or prove a corresponding product speedup.

The adapter retains the same libraries and input but changes several surrounding
conditions: the original harness directly calls decode and accumulates two
checksums, warming one corpus pass; the diagnostic dispatches through `invoke`,
stores per-shot result/stage records, checks empty queues and skipped fallback,
and warms full corpus cycles for two seconds. It also loads and retains an edge
dataset before graph construction, allocates other record buffers, and links a
different translation unit containing split paths. Heap placement, call/code
layout and warmup conditions therefore differ. Their individual contributions
to the absolute change have not been isolated; no one of them is asserted as its
cause. New-window numbers cannot replace the frozen original measurement.

## Concrete code review and decision

`one_hop_predecoder.cc:22` validates and normalizes events, clears the prior sparse
map, builds new seed/map entries and handles negative-edge XOR. Its
`seeds.push_back({node})` at lines 40 and 54 initializes the fields declared in
`one_hop_predecoder.h:18`. The skipped gate and
`Mwpm::import_predecode_state` at `mwpm.cc:547` use only each seed's node pointer.
Thus lazy initialization of the unused matching fields is a concrete candidate
for future investigation, but the current data does not isolate those stores
from the rest of prepare. Even eliminating the entire measured prepare interval
corresponds to only about 0.17% of this diagnostic's whole-shot latency; that is
a scale comparison, not a predicted saving or a hardware-independent upper bound.

A safe change would need an explicit initialization/lifetime invariant across
negative-syndrome seed copies, exceptions, accepted predecode, and skipped import.
Simply removing defaults is insufficient. A separate pointer-only preparation
representation also adds work or storage to accepted-predecode workloads. The
existing dynamic evidence does not justify this added design and validation cost
as a release-blocking fix.

The sampled gate at `one_hop_predecoder.cc:119` performs bounded degree/boundary
checks and active-neighbor membership reads needed by its current dispatch rule.
The previous loop call/division removal is already present. This run provides no
evidence that another redundant operation dominates its 249 ns interval, and no
threshold change is proposed. Sparse degree-bound shortcuts were independently
rejected earlier because they cover none of the actual hubs shots.

Consequently, there is evidence for necessary extra dispatch/preparation work on
fallback shots, but no demonstrated removable bottleneck that is necessary to
repair before delivery. There is also no proof that the entire migration is
performance-equivalent. Keep the original +1.09665% hubs median and all ranges,
the complete multi-workload results, and this diagnostic limitation visible.
Do not continue optimizing merely to force every sample or window below zero.

Reproduction: `analysis/analyze.py` regenerates numeric comparisons from the
frozen results and original summary. `independent-replay.json` records byte-exact
replay. `analysis.json` records source/result hashes and all paired differences.
No runtime source, sealed input, collector binary, or formal benchmark was changed.
