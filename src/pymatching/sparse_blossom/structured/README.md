# Structured backend

The structured decoder owns a separate C++ type family in `pm::structured`.
Its nodes, regions, event payloads, queues, alternating trees, flooders, matcher,
path recovery, and drawing code live in this directory. Changes to these types
do not change the layout or implementation of the original `pm` decoder.

`driver/backend.h` provides complete specializations of
`Backend<BackendKind::Generic>` and `Backend<BackendKind::Structured>`.
The primary template has no implementation. Each specialization binds directly
to one concrete decoder and its decode functions. Python selects the
specialization at the API boundary; `decode_batch` keeps that selection outside
the shot loop. The profiling runner selects a specialization before its setting
and shot loops.

## Independent components

The structured specialization uses `ComponentDecoder`. Construction partitions
the graph by ordinary-edge connectivity; boundary half-edges do not connect
components. One or two nonempty components are supported. Explicit group labels
do not merge disconnected components. Each component owns a complete `Mwpm`, including its event queue,
dual clock, indexed region pool, and alternating-tree arena. The normal
`ComponentDecoder` entry runs components sequentially. `SharedComponentDecoder`
can assign them to separate groups in a resident OpenMP team. Each component
retains its own matcher and queue.

Input detector IDs are global. The outer decoder partitions them into local
component IDs, XORs the observable predictions, adds the weights, and maps
recovered paths and matched detector pairs back to global IDs. All components
use one normalization constant computed from the complete input graph before
partitioning. Negative edges and self-loops retain their original contributions,
including on shots with no detection events. Observable sets above 64 use each
component's own search graph.

Component IDs describe graph connectivity. They do not infer physical X/Z
stabilizer labels. For the supported surface-code models the two components
correspond to those stabilizer classes, but that association depends on the
circuit's coordinate convention.

Correlated decoding is currently unsupported by the structured API and is
explicitly rejected when requested. A DEM loaded with correlation information
can still use ordinary single-pass decoding. Generic correlated decoding remains
available after disabling the structured backend.

## Storage and ownership

- Matching nodes hold a 32-bit owner region ID, event state, and grid location
  fields. They have no adjacency vectors; their size is 72 bytes on the
  measured GCC 14/x86-64 ABI. Dense collision state holds the top region ID
  and wrapped radius; the node has no duplicate top-region pointer.
- `RegionPool` stores actual objects in `regions[id]`; ID zero means no region.
  The same ID indexes `RegionRadiusStore`. Persistent region links store IDs;
  temporary pointers are resolved directly from the pool. Placement construction
  and explicit destruction preserve object lifetimes without moving live regions.
  Queued shrink events carry an allocation generation, so recycling an ID cannot
  make an old event refer to the new object. Generation overflow throws before
  reusing an identity.
- A region shell keeps its first node inline. Larger shells allocate storage
  on growth and release it when the region is destroyed.
- Graph and node accessors always access structured storage. They have no
  adjacency-list fallback or nullable backend selector.
- `MatchingGraphConstructionNode` owns temporary adjacency vectors separately
  from runtime `DetectorNode` records. Grid construction reads these records
  and releases them only after all components are installed successfully. A
  rejected grid configuration leaves the construction records intact.
- The matcher and search graph own separate grid storage. Component allocations
  and pool allocations preserve node, region, and edge-weight addresses when
  the decoder moves.

The original `UserGraph` remains unchanged. `driver/backend_user_graph.h`
contains a separate `BackendUserGraph` API adapter with independently invalidated
generic and structured caches. This also preserves the code generated for the
original DEM-to-decoder factory.

## Specialization boundary

There is no generic-versus-structured test in either decoder core. Checks for
algorithm state, missing edges, boundaries, and input validity remain part of
the algorithm.

Grid dimensions specialize `StaticGridBox` kernels. Construction also chooses
one of three complete `CircuitStencil` specializations from the component's
actual ordinary-edge directions, independently of component order and labels.
Scalar matching and search kernels expand exactly twelve directions. In the
shared direction numbering, the rotated X stencil is `{0..9, 12, 13}`, the
rotated Z stencil is `{0..7, 10, 11, 14, 15}`, and the unrotated stencil is
`{0..11}`. Their union has sixteen directions; this does not imply that a
single circuit component needs sixteen scan candidates. Boundary half-edges
must use a slot within the selected stencil. Unsupported direction combinations
are rejected during construction, before any input edge can be discarded.

Physical weight and observable rows use the build-time
`PYMATCHING_STRUCTURED_STORAGE_DIRECTIONS` setting (12 by default, or 16). With twelve slots,
`CircuitStorage<Stencil>` packs each stencil's ascending geometric directions
into slots 0 through 11. Complete opposite-direction pairs preserve `slot ^ 1`
for reverse edges. Mapping is constant in each scan specialization; the shared
sixteen-direction geometry and generic backend are unchanged.

The twelve-slot weight row is 48 bytes with 16-byte alignment; the sixteen-slot
row is 64 bytes with 64-byte alignment. Matching observable rows shrink from
128 to 96 bytes. Twelve slots therefore save 48 bytes per dense matching cell
relative to sixteen slots. Rank rows have been removed. These payload sizes
exclude runtime nodes and are not the working set or process RSS. Search observable rows contain vector
objects instead of matching's 64-bit observable masks and must be counted separately.

Explicit SIMD choices are `scalar`, `avx2`, `avx512vl`, and `avx512`. AVX2 and
AVX-512VL both process four 64-bit lanes at a time; AVX2 emulates signed 64-bit
shifts and minimum with other integer instructions. AVX-512VL uses native
256-bit signed operations and mask registers. Both use three batches for twelve
slots or four for sixteen. AVX-512 uses two batches of eight lanes in either
layout, with a masked four-weight tail load in the twelve-slot layout. No tail
load reads beyond the last weight row. All three SIMD scans specialize source
growth state and inline their candidate batches.
The `auto` choice binds the scalar kernel. SIMD remains opt-in: on the measured
Xeon Gold 6230R, the scalar kernel was faster than all three SIMD variants in
the screening cases. ISA availability alone does not establish a speed benefit.

Dimension, stencil, and SIMD selection occur during construction. The `matching_next`,
`search_next`, and `selected_offset` function pointers remain in each component;
the decoder core gains no backend-selection branch.
The registry supports odd distances 3 through 19, plus 21 and 25, with rounds
equal to the distance or three times the distance.

Matching and search choose the first direction with the minimum event time.
Scalar uses a strict comparison; SIMD chooses the lowest winning storage slot.
Ascending slot mappings preserve the same geometric direction in either layout.
Grid construction records boundary masks alongside edge-presence masks.
Predecode, neighbor lookup, and shortest-path search share these masks.
The grid stores no adjacency ranks; correlated decoding remains unsupported.

The scalar matching entry computes the collision-array base, weight row,
present mask, and source intercept once. Source growth state then selects one
of two complete `GridSourceCollision` specializations. Growing sources accept
non-shrinking targets; frozen or empty sources accept growing targets. Shrinking
sources return no event. These tests use the valid `Varying` slopes -1, 0, and
+1. Candidate bodies no longer repeat the source-state arithmetic. GCC and
Clang builds apply `always_inline`; the measured GCC 14.3 code contains no
outlined candidate calls. Grid dimensions and stencil selection remain construction-time
choices; there is no backend or stencil branch in the candidate bodies.

Equal-time event order can select a different minimum-weight correction and
observable mask from generic. Correctness checks therefore distinguish the
minimum objective value from the choice among degenerate optima. Exact small
graph oracles enumerate all optimal observable masks; surface-code comparisons
also record prediction differences and paired logical outcomes.

## Validation

The static-grid C++ tests cover graph construction, collision states, equal-time selection,
masked directions, scalar/SIMD parity, path recovery, active-decoder moves,
reweighting, diagram coordinates, independent component ownership, global ID
mapping, and common weight normalization. Compile-time assertions keep generic
node and region sizes at their original 64-bit ABI sizes, and verify that the
structured runtime node has no construction adjacency members. Python tests cover
batch and single-shot APIs, correlation rejection, negative weights, large
observable sets, configuration failure, mutation, and repeated backend switching.

`experiments/sparse-blossom-benchmarks/scripts/verify_generic_binary_identity.py`
compares every original sparse-blossom object except the profiling runner after
removing debug sections. It requires exact object bytes, including code,
constants, symbols, and relocations. A combined executable containing both
backends has a different complete ELF hash. The shared disabled-profiling fix
also changes generic code generation; compare baselines with the same profiling
header and flags when checking identity after that fix.

The earlier `separate_backend_benchmark.py` and
`jobs/13_separate_backends.slurm` describe the backend-separation experiment with
a shared structured queue. Its timing and algorithm-counter parity results do
not describe independent component execution. New measurements must freeze the
current source, verify optimal weights and record prediction differences, and include global-to-local
dispatch and result merging in the reported end-to-end decode time. Independent
queue order can change algorithm counters and which degenerate optimum is chosen.

`ComponentDecoder::prepare_shot` owns one compact shot seed vector. For inputs
without negative-edge syndrome corrections it counts component sizes and fills
their final contiguous spans directly, preserving input order within each span.
A 16-byte detector address supplies component, local detector, cell, and the
last input epoch; duplicate detection does not read mutable detector nodes or
write their active-cell arrays. Epoch wrap clears the address stamps once.
Negative-edge corrections use append, parity normalization, and a stable
component permutation. Predecode and final extraction consume the same seed
records. Each grid owns active-cell indices, dual radii, boundary masks, and
kernel bindings. The cell-to-seed index is built after the final seed ordering;
callers preparing spatial buckets defer that step.

`OneHopPredecoder::run` consumes prepared seeds. Its range interface keeps
candidate masks, degrees, and order in disjoint slices of component scratch,
and accumulates results and diagnostics in caller-owned partial results.
A source range can grow or match a neighboring seed outside the range, so
parallel callers must separate the complete read/write footprints before
running ranges concurrently. `merge` and `finish` run after those ranges finish.
Direct `Mwpm` calls retain an owned seed vector for the same prepared interface.

## Resident shared execution

`SharedComponentDecoder::run_with_team` surrounds a continuous batch. Calls to
`decode` or `decode_to_matches` run inside its callback; one OpenMP team serves
the entire batch. Explicit CPU IDs are checked against each worker's actual
affinity and restored on exit. Atomic commands dispatch work and join results.
An active decoder cannot be moved or entered recursively.

`PYMATCHING_SHARED_COMPONENT_PARALLELISM` binds component scheduling at compile
time. Its default value `2` retains separate X/Z groups. Value `1` reuses one
group sequentially for both components. With two total threads and two spatial
partitions, CP1 runs both interior lanes for X, joins them, completes that
component, then reuses the same captain and helper for Z. Serial import, MWPM,
extraction, and the conflict bucket stay on the captain. The normal
`ComponentDecoder` entry remains independent of this shared execution setting.

CP1 worker snapshots use `SharedThreadSnapshot::SHARED_COMPONENT`, because each
physical lane serves both components. The captain writes the current component
before releasing helper commands; helper acquisition reads that published ID.
Completion joins every active helper before the captain changes the ID. The
same resident team serves consecutive shots, fallback routes, and recovery.
The setting adds no runtime component-policy selector to the CP2 hot path.

CP1 worker phase endpoints reused by both components are repeated envelopes.
Duration sums remain worker service durations; an envelope also contains gaps
and contributes only diagnostic detail. Component phase intervals remain
continuous and ordered X then Z for interval-union analysis.

CPU IDs are explicit. The default configuration uses one spatial partition per
component, a component threshold of 64 seeds, a whole-shot threshold of 32 seeds,
a two-cell guard, and serial state import. Spatial partitioning and parallel
import remain explicit options for measured configurations. This C++ batch API
does not change the normal Python structured entry point.

Spatial predecode uses fixed time slabs, independently of the number of threads.
Construction converts slab boundaries and guard bands into flat-cell thresholds.
Each component captain permutes its span into contiguous interior buckets and
one conflict bucket, then rebuilds its cell index. Workers process disjoint
interiors, join, and process the conflict bucket serially. A guard of at least
two cells separates one-hop writes from two-hop reads, including unmasked SIMD
loads. Low-syndrome components enter serial predecode before bucket preparation;
small whole shots take the serial route before X/Z dispatch.

State import reserves contiguous region IDs before workers construct disjoint
seed ranges. Matched endpoints use node addresses and region IDs, so a worker
does not read its mate's unconstructed object. All workers join before the
captain creates tree roots and schedules events. Exception paths join both
groups before reclaiming live objects and unused reservations. Parallel import
is an explicit configuration whose benefit must be measured with total CPU time.

After extraction, the component captain clears its active-cell and radius
arrays before publishing the result. Seeds, results, and diagnostics remain
available until the next reset; the main thread skips this completed cleanup.
Predecoder moves transfer cell ownership and invalidate the old view.
Cleanup, dispatch, waiting, and result merging belong in decode timing. Component
and worker diagnostic windows can overlap and cannot be added as wall-clock shares.

### Experimental double-buffer reset overlap

`DoubleBufferComponentDecoder` in `shared_component_decoder.h` owns two complete
`SharedComponentDecoder` objects. Construct both from the same graph, grid,
quantization and algorithm settings. Each buffer owns its mutable regions,
queues, detector state, seed storage and scratch. Normal decoder entry points
remain independently selectable.

```cpp
DoubleBufferComponentDecoder pipeline(std::move(even), std::move(odd),
    DoubleBufferComponentDecoder::Schedule::overlap);
pipeline.run_with_team([&] {
    for (const auto& input : shots) {
        const auto result = pipeline.decode(input);
        consume(result);
    }
    pipeline.drain();
});
```

One OpenMP team contains two disjoint groups of G threads, using explicit,
nonoverlapping CPU lists. Each group reuses its existing X/Z and spatial worker
schedule. G=1, 2 and 4 therefore require 2, 4 and 8 actual threads. Thread zero
is both the callback thread and buffer zero's captain. Its even decode overlaps
buffer one's pending reset. For an odd input it publishes buffer one's decode,
resets buffer zero, and acquires the completed result. A long reset on thread
zero delays odd result delivery; that delay belongs in measured latency.

`decode` borrows the input only for the duration of the call and returns an
owned `MatchingResult`. Calls are synchronous and results stay in submission
order. The result is copied before the remote captain receives `Reset`.
Acquire/release publication orders result reads, cleanup and buffer reuse.
Callback exit drains both buffers, including empty, one-shot and odd batches.
An exception joins in-flight work and retires state before propagating; later
batches may reuse the decoder. Parity continues across batches and explicit
`drain` calls. Moves transfer inactive state; active moves and nested batches
are rejected. Callback-only mutation keeps reset ownership private.

Three schedules are bound during construction:

| Schedule | Actual threads | Work ordering |
| --- | ---: | --- |
| `overlap` | 2G | Decode and the other buffer's reset may overlap. |
| `serial_control` | 2G | The same two groups and buffers execute reset and decode sequentially. |
| `single_thread` | 1 | Two G=1 buffers use the same CPU and execute all work sequentially. |

The single-thread schedule preserves the selected spatial partition algorithm
and fallback thresholds. Its two logical buffer snapshots identify the same
actual thread. Read `team_size()` for the total, and deduplicate thread IDs
when collecting CPU usage. `serial_decoder(buffer)` exposes const setup metadata
while the team is inactive.

Measure each submission immediately before `decode` and result visibility
after it returns. The interval includes ready waits, publication, preparation,
MWPM, extraction, result transfer and any controller reset that blocks delivery.
Batch timing includes final `drain`; declare whether OpenMP team startup and
exit are also included. Account for CPU time on every actual thread.

Timer-enabled builds expose caller-owned `shot_profile()` snapshots. A reset's
`completed_reset_profile(buffer)` becomes visible when that buffer is acquired
again or drained; attach it to its original `shot` ID and collect the final
two records after draining. Overlapping stage intervals remain separate.
Clock-free builds compile out these snapshots, clocks and diagnostic shot IDs.

### Shared strategy construction

The normal strategy facade uses this single structured source tree. Its
`ComponentDecoder` constructor binds predecode on or off; `Mwpm` selects complete
preparation and timeline template specializations before any shot. Both bindings
coexist in one library. GridSIMD uses the current structured core with predecode
off, including its active-tree completion rule. Predecode uses the serial fixed
seed path from the 1103 Predecode/Component source.

`SharedDecoderConfiguration` freezes shared preparation choices at construction.
The explicit-configuration constructor does not read environment variables.
The older constructor retains `from_environment()` for experimental callers.
Component and TailReset retain the P1 fixed serial backing and component-owned
retirement; TailReset adds the existing two-buffer overlap. Spatial retains the
indexed P2 input, captained-serial import, captain reset ownership, and two-buffer
overlap. The latest borrowed-seed pool and import implementation are shared.

`SeedOrdering::reconstruct` is the original PC/Tail negative-normalization
reordering, which rebuilds records from their detector IDs.
`SeedOrdering::cycle_permutation` is the original Spatial reordering, which swaps
complete seed records. These existing implementations are bound through
`ComponentDecoder::configure_seed_ordering`; strategy construction selects the
corresponding source behavior before decoding.

Structured import's borrowed root reservation is provided by `StructuredArena`,
derived from the generic arena. Its extra methods are confined to structured
code; the generic arena's representation and allocation algorithm are shared
without changing the official generic decoder path.
