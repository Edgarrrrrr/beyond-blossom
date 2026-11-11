# Pipeline dynamic diagnosis: draft, not executed

Date: 2026-11-12. Owner: `/root/primal_architecture`; independent review: `/root/paper_ae_audit`.

This protocol plans a diagnostic acquisition for the remaining d17, p=0.001 pipeline slowdown. Formal acquisition has not started. Root separately authorized isolated adapter builds and 32-shot capability/correctness smoke. All three r4 adapters built and all six source/current/official off/on smokes passed exact output, thread coverage and counter validity. The prior r3 accounting failure is retained and explained below. Root's initial probe is preserved at `pmu_probe.default.json`; it tested one user-only cycles event.

The immediate proposal is a separate, small adapter patch linked to the existing sealed libraries. It reads counters for every resident worker around the existing post-warmup batch window. The second, conditional step uses the existing `pipe-stages` profiler. This is diagnosis, not a new algorithm, an accepted optimization, or a replacement for the original 819-call run or the 120-call outlining follow-up.

## 1. Question and evidence boundaries

The original pipeline cell had a candidate/source median time increase of 1.4384%; the separate eight-repeat follow-up had 0.8876%. The rejected boundary noinline variant did not establish a reliable causal remedy. Source and candidate have the same pipeline schedule, selected AVX-512VL kernel, four worker CPUs, saved input, warmup and completion contract. Existing process CPU time includes spin CPU.

We seek evidence distinguishing these explanations:

1. More executed work or worse execution efficiency in the candidate's decode/reset path.
2. Greater synchronization or buffer-readiness delay, including busy-waiting.
3. A different on-CPU/frequency/scheduling exposure despite equal nominal affinity.

Window counters can discriminate some of these, but **cycles, instructions and task CPU time do not by themselves separate computation from busy-spin**. A spin loop consumes CPU time and retires instructions. Low IPC is not proof of waiting. Stage durations identify where wall time is spent in instrumented executions; they still do not convert automatically into per-thread useful-work CPU time.

Official upstream remains the main task-level performance baseline. Source241 is the migration control. Official upstream is serial and uses a different algorithmic organization, so its one-thread counter values are context, not a four-thread pipeline wait-time denominator.

## 2. Fixed artifacts

| Role | Executable | SHA-256 |
| --- | --- | --- |
| Source migration control | `/tmp/beyond-build/structured-final-bin/source` | `72aad5b155b40f10eac1e3cb411ee3ebf7a8aa8f999e3e987a30310c4647650e` |
| Current implementation | `/tmp/beyond-build/structured-final-bin/candidate` | `47f106f88c6c1ced83d98f2f96bbffa388b68ab4254c4acd04d57c71882a0520` |
| Official upstream | `/tmp/beyond-build/structured-final-run/bin/reference` | `b2b42e4fd401fe3db39a405bd4d119edfc9ee03b2b9d029a558afa8d52b4e7e4` |

Source original commit: `241160314087e5ae698c6e76c3c055a11c3ad422`. Candidate runtime commit: `edd29b6780713dc78f6a99d67c4c4d192e9c52b5`. Official upstream: `e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd`. The d250 portable CMake change produced identical 44 objects and static library on this host; this does not assert identity on GCC15 or a different machine. Receipt: `/tmp/beyond-portable-baseline-validation/validation-receipt.json`.

Library inputs for adapter-only builds:

- Source: `/tmp/beyond-build/baseline/libpymatching.a`, SHA `cb26c0ae13d82aa6929aeaa967979f94a450f4b7b7d6962fb8fb08950246fbe9`.
- Candidate: `/tmp/beyond-build/candidate/libbeyond_blossom.a`, SHA `b3f84f05588766cfe38e0ad9e47e65b84d6a2350ca176b192b9e80b1fb586a7a`.
- Official: use the library and source identities in `/tmp/beyond-ae-sealed-reference/reference.supplement.json`, checking them against the sealed reference executable build receipt.
- Source/candidate Stim library SHA: `c068eebb611b66d2094d392eb222c05e57ecbcd99df4102b44f4a157aebabbc6`.
- Preserve actual compiler commands and headers from `/tmp/beyond-build/structured-final-bin/build.json`; use the original official adapter command for the reference. No LTO; no new ISA flags; internal profiling remains off in this first step.

Input: `/tmp/beyond-build/structured-final-run/inputs/d17-p0.001/dataset.bin`, SHA `c793ee4b01d9747283137ef96aa5b4d26b6c6ea2c5e13500d70f4a757c564eef`. Preserve the adjacent `model.dem` and `circuit.stim` and all their existing hashes. These are the same 10,000 saved distinct records, not fresh samples. Root seed 20261112; stream ID 10. No input regeneration, selection, reordering or repeated-shot expansion in the initial protocol.

Before implementation, recheck these identities from their receipts and freeze a local manifest. A mismatched artifact stops preparation; do not silently substitute current HEAD.

## 3. Smallest proposed patch

Make the changes only in an isolated, identified acquisition snapshot, never in the root runtime checkout. Freeze the adapter diff and source file hashes before building. Source, candidate and official receive the same collector implementation, with only the already-established namespace/include adaptation.

1. Add `AE/native/window_pmu.h` (and the equivalent original adapter path). A small RAII collector owns per-TID descriptors and preallocated snapshot storage. It provides construction/attachment, `snapshot_begin()`, `snapshot_end()`, validation and persistence. It makes no decoder calls and does no output inside the window.
2. In `benchmark.cc`, include the helper before `serial_window`/`decode_window`. After `capture_serial` or `capture_team`, create the collector inside the existing resident-team callback, before warmup. Give it the actual TIDs and CPU/component/lane/buffer mapping. A scoped pointer on the calling thread makes it available to `measure`; restore that pointer on every exit. No worker command, barrier, dispatcher or scheduling change.
3. In `measurement.h`, add two hooks only to the ordinary batch path: a start snapshot immediately before the existing `getrusage(cpu_before)`, and an end snapshot immediately after the existing `getrusage(cpu_after)`. Capture the existing outer-clock endpoints in the diagnostic record. Leave the decode loop, block clocks, result stores and final drain unchanged. PMU mode is selected once per process with a new adapter-only environment variable, proposed `BB_DIAGNOSTIC_WINDOW_PMU=off|on`; source and official use the same spelling. `off` opens no descriptors and takes no PMU snapshots.
4. Add an out-of-tree build/controller script that reuses the sealed compile/link commands and libraries, writes hashes/commands, freezes run order, and validates all native outputs. No files under `src/` change. Three diagnostic adapters are built, one per library identity; each supports on/off collection.

This is a new adapter binary, not the old sealed executable. A static library hash does not imply that all header-instantiated adapter code is identical. Record normalized instruction comparisons for the critical pipeline team, decode, drain, reset and primal symbols; record all differences rather than demand an invented whole-executable identity. Sealed binaries and the same diagnostic binary in `off` mode are retained as controls.

### Why not whole-process `perf stat` or inherited groups

A process-lifetime counter includes construction, cold correctness checks, team startup, at least two seconds of warmup and output. The actual measured pipeline stream is only roughly 48 ms. Such totals cannot be called decode counters.

For the planned collector, `pid=TID, cpu=-1` attaches to a named live thread. Inheritance applies to newly created children, not already existing workers, and has a documented incompatibility with `PERF_FORMAT_GROUP`; use `inherit=0`. Open a separate group for each live thread. A parent counter with inheritance is not a substitute for per-worker attribution. [Linux perf_event_open manual](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)

An `LD_PRELOAD` interposer around the two `getrusage` calls could retain the exact sealed executable, but it would require identifying call sites, exposing worker identity and validating interposition effects. The explicit two-hook adapter is the smaller auditable implementation now that adapter-only changes are permitted. Uprobes or a new sampling profiler are not needed in the first step.

## 4. Actual window and lifecycle

Current code references, to be bound to frozen source hashes:

- `AE/native/measurement.h::measure`: preallocates result/block storage, warms the decoder, invokes the before-measurement hook, then runs the normal timed block loop.
- `AE/native/pipeline_measurement.h::align_pipeline_warmup`: aligns buffer zero and drains before the measured stream.
- `AE/native/pipeline_measurement.h::measure_pipeline`: maps `pipe-batch` onto normal batch measurement and includes final `decoder.drain()` in the last block.
- `AE/native/benchmark.cc::decode_window`: enters one resident team before invoking the measurement callback.
- `src/beyond_blossom/structured/driver/shared_component_decoder.cc::State::team_paired`: establishes the complete OpenMP team, initializes workers, and passes two startup barriers before the callback. Workers persist until the callback and final teardown finish.

Planned order:

```text
construct graph/decoder; cold correctness work; create persistent team
  capture actual thread mapping; enumerate all process tasks; attach/open counters
  enable groups and task clocks; allocate/pre-touch all collector storage
  >= 2 s normal warmup and correctness checks
  align buffer zero and drain; finish warmup bookkeeping/frequency snapshot
  verify task membership and affinity outside the counter-delta window
  [start snapshots: group + task-clock for each TID, read brackets recorded]
  existing getrusage(cpu_before)
  existing outer_begin
    existing 1024-shot blocks, decode + result store
    final drain inside last block
  existing outer_end
  existing getrusage(cpu_after)
  [end snapshots: same per-TID order, read brackets recorded]
  disable/close counters; verify task identities; persist after callback
team teardown and output
```

Counters can run during warmup because only the end-minus-start snapshots are analyzed. No allocation, descriptor opening, enumeration, disk access or formatting belongs between the two snapshot sets. Every snapshot and delta is retained.

`capture_team` maps all four participants, including the caller, to CPUs 2,4,6,8, with one lane per X/Z component per buffer. Enumerate `/proc/self/task` in addition to this mapping; attach to all existing TIDs, label any additional process threads, and fail preparation if their role/lifecycle cannot be explained. Record `/proc/.../stat` start-time identity to guard against TID reuse. Compare membership at attachment, after warmup and after the stream. Require all expected resident workers throughout.

Equal endpoint task sets alone cannot exclude a short-lived thread inside the window. The no-late-creation claim also depends on the audited fixed resident-team code and lack of thread creation in the callback. If this cannot be established for an extra runtime thread, the run is incomplete until separate lifecycle evidence is available; do not silently rely on endpoint enumeration or inheritance.

### Counter-window skew is measured, not hidden

Reading each group's descriptors sequentially creates slightly different per-thread envelopes. A group read is atomic only for that group, not globally across threads. The first layer does not insert a new worker barrier or pause the workers: idle workers may spin while the caller reads snapshots.

Bracket each `read()` with `CLOCK_MONOTONIC` timestamps; record snapshot order, group/task-clock intervals and the native outer-clock timestamps. Each per-event delta encloses the native loop plus some boundary work. Report that envelope separately from `elapsed_total_ns` and `outer_wall_ns`. The process CPU snapshots exclude the collector reads; PMU/task-clock deltas include their boundary exposure.

For each record, compute the conservative extra-envelope duration `max(end_read_end) - min(start_read_begin) - native_outer_wall_ns`. Initial quality gate: it must be <= 0.25% of native outer wall. This is a predeclared quality threshold for a roughly 1% question, not a correction factor or accuracy guarantee. Do not subtract estimated spin, syscall cycles or timer cost. If the gate fails, retain the run and revise the acquisition separately, rather than silently increasing repeats or scaling it away.

## 5. Counter contract

Per TID, open a three-event hardware group:

- `PERF_COUNT_HW_CPU_CYCLES` (leader), `PERF_COUNT_HW_INSTRUCTIONS`, `PERF_COUNT_HW_REF_CPU_CYCLES`.
- `PERF_TYPE_HARDWARE`, `cpu=-1`, `pid=TID`, `inherit=0`, `exclude_kernel=1`, `exclude_hv=1`, sampling disabled. Leader `disabled=1`, `pinned=1`; members join that leader and start with it.
- `read_format = GROUP | ID | TOTAL_TIME_ENABLED | TOTAL_TIME_RUNNING`; obtain event IDs using `PERF_EVENT_IOC_ID`. Decode returned values by ID, not by positional assumption. Check complete read length, event count, unique IDs, positive times, monotonic snapshots and no counter wrap.

Open a separate `PERF_TYPE_SOFTWARE/PERF_COUNT_SW_TASK_CLOCK` descriptor per TID with its own enabled/running/ID fields. Do not mix software and hardware PMUs into one presumed schedulable group. Request total scheduled task CPU time (user and kernel); record the exact exclusion bits and whether the kernel accepted the event. Hardware counts are user-only, so IPC and cycles/task-CPU have different scope. The task clock is a per-task running clock; reference cycles have a frequency-independent definition. [Linux perf event definitions](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)

The initial probe verified only one self-thread cycles event. The completed r4 smoke additionally verified cross-thread attachment, the three-event group, reference-cycle support and software task clock on the actual source/current/official adapters. Failure in any subsequent record is still an explicit capability/contract result. No sudo, permissive sysctl change, guessed raw event encoding or fallback to process-lifetime counting is part of this protocol.

Require delta `time_running == time_enabled > 0` for the pinned hardware group. Preserve raw values and both times even on failure. If scheduling cannot satisfy the group, stop; do not silently report multiplex-scaled counts. A revised separate-pass event set needs its own frozen plan. Avoid unrelated concurrent profilers. Disable the existing import PMU environment variables in both namespaces to avoid competing counters.

Task clock is the primary per-thread CPU quantity. Native `getrusage(RUSAGE_SELF)` user+system delta remains a diagnostic record, not an exact short-window reference for the other CPU cores. Do not use `/proc` tick-resolution utime/stime as a substitute. For each TID, require a nonnegative monotonic task-clock delta no larger than `end_task_read_end - begin_task_read_begin`, the largest possible actual interval between its two reads. Keep per-thread values and bounds; do not use the common native wall as every thread's exact counter interval.

The initial r3 draft froze a stronger aggregate check: with S the summed task clock, R the native getrusage delta and E the summed extra read envelopes, it required `R - 4000 ns <= S <= R + E + 4000 ns`. Its first source/on 32-shot smoke failed: S=649492 ns, R=147000 ns, E=73958 ns. All four PMU groups were unmultiplexed, all raw reads/IDs were valid, and all 32 observable/weight outputs matched. Acquisition stopped, preserving `smoke-r3` and `PROTOCOL.r3-pre-smoke.md`.

Root then identified the invalid premise: Linux v6.1's `thread_group_cputime` updates the current task's pending runtime, while other running tasks' accumulated runtime need not include the latest unscheduled interval. `RUSAGE_SELF` reaches that accounting through the adjusted group CPU-time path. Four microseconds only bounded timeval representation, not remote-task update delay. This kernel mechanism motivates removing the false reference check, rather than enlarging a tolerance to make the observation pass. Source: [Linux v6.1 CPU accounting](https://raw.githubusercontent.com/torvalds/linux/v6.1/kernel/sched/cputime.c). Host kernel/config identity is recorded in `kernel-accounting.json` (6.1.0-41-amd64, HZ250, runtime nohz_full null).

Revision r4 retains the old failed comparison as historical diagnostic data, uses the per-TID physical interval bound above, and keeps the full-coverage, strict hardware enabled/running and 0.25% formal snapshot-envelope gates unchanged. Root independently tested four threads on CPUs 2/4/6/8, each at 1 ms and 50 ms: all eight task-clock deltas fell inside that worker's own `CLOCK_THREAD_CPUTIME_ID` read envelopes without an added tolerance or subtraction. Source/result: `task_clock_probe.cc` and `task_clock_probe.json`. This supports using task clock; it does not prove the whole prior RUSAGE discrepancy has one cause. Root authorized r4 builds and six 32-shot capability smokes after that result. Short smoke is exempt from the 0.25% formal envelope limit and still reports the ratio. Formal acquisition remains unauthorized.

Persist per process, TID and buffer/component role: raw start/end values; event IDs/type/config/attrs; enabled/running deltas; cycles, instructions, reference cycles, task CPU ns; each read bracket; native wall/block times; correctness counts; affinity and thread identities; input/library/adapter/binary hashes.

Derived values: instructions/record, user cycles/record, reference cycles/record, task-CPU ns/record, per-thread IPC, cycles/reference-cycles ratio, and CPU utilization using each thread's measured envelope. Sum counts before forming an aggregate ratio. Do not average per-thread IPC or label an aggregate cycles sum as elapsed time. Frequency ratios are comparisons of event counts, not a calibrated GHz value. Thread role splits and raw counts remain available.

## 6. Minimal initial matrix and actual invocation

One fixed cell only: d17, p=0.001, 10,000 saved records. No new parameter search. All calls serialized on the original host, same affinity and environment, with no concurrent compile, render, native test or other benchmark.

Three implementations: source, current, official. Three acquisition conditions per implementation:

1. Original sealed executable, pure mode.
2. New diagnostic executable, collector `off`.
3. The same diagnostic executable, collector `on`.

Six fresh process repeats = **54 calls**, each with its own >=2 s warmup. This provides contemporaneous pure source/current evidence, controls for rebuilding the adapter, and an on/off control for the actual collector. It is not six new corpora. Use all six implementation permutations once; rotate the three acquisition-condition blocks across repeats so every condition appears in each position twice. Freeze the exact 54 command arrays and order before execution. Keep every observation; no trimming or early favorable-result termination.

The separately authorized 32-shot correctness/capability smoke is complete at `smoke-r4/smoke.json`: six calls, all exact observable/weight comparisons passed, thread counts 4/4/1, unmultiplexed hardware groups and valid task-clock bounds. These calls are not performance evidence. The short-window snapshot overhead ratios were approximately 28.8%/27.8%/1.72% for source/current/official, so none establishes the 0.25% quality gate for the formal 10,000-record window. That gate remains mandatory per formal record; no 10,000-record run has been started to test it.

Existing pipeline CLI, unchanged except binary and a fresh output path:

```bash
/tmp/beyond-build/structured-final-bin/source \
  --distance 17 --error-rate 0.001 --shots 10000 \
  --dataset /tmp/beyond-build/structured-final-run/inputs/d17-p0.001/dataset.bin \
  --root-seed 20261112 --stream-id 10 --mode pipe-batch --cpus 2,4,6,8 \
  --warmup-seconds 2 --verify exact --block-size 1024 --repeats 1 \
  --execution pipeline --predecode 1 --partitions 1 \
  --component-threshold 64 --shot-threshold 32 --conflict-width 3 \
  --backend avx512vl --output-dir <fresh-call-directory>
```

Candidate uses exactly the same arguments. Diagnostic invocation uses `env BB_DIAGNOSTIC_WINDOW_PMU=on <diagnostic-binary>` with the same argument array; `off` changes only that environment value. Diagnostic binaries are `build-r4/{source,candidate,official}.benchmark`; identities and commands are in `build-r4/build.json`. Preserve the default overlap schedule. `--repeats 1` is copied from the original contract; it is not assumed to repeat the batch internally. Existing `--replay-kind` targets one selected shot/context and is not an appropriate whole-stream duration extension.

Official invocation:

```bash
/tmp/beyond-build/structured-final-run/bin/reference \
  --distance 17 --error-rate 0.001 --shots 10000 \
  --dataset /tmp/beyond-build/structured-final-run/inputs/d17-p0.001/dataset.bin \
  --root-seed 20261112 --stream-id 10 --mode batch --cpus 2 \
  --warmup-seconds 2 --verify exact --block-size 1024 --repeats 1 \
  --backend generic --execution serial --predecode 0 \
  --output-dir <fresh-call-directory>
```

Check every shot against both the saved integer weight and observable. The fixed input produced zero differences in both previous windows, and the collector does not change the algorithm. Freeze `--verify exact` plus independent output comparison; any observable or weight disagreement stops the run. Do not relax this cell's policy through a post-hoc degeneracy witness. Preserve normal cleanup and the last-block drain, kernel selection, measurement scope, execution thread records, warmup duration and all stdout/stderr.

## 7. Analysis and stopping rules

First evaluate validity and disturbance, before interpreting hardware counts:

1. Reject attribution for any missing/changed worker, wrong affinity/kernel, changed input, correctness failure, unsupported event, incomplete read, multiplexing, excessive snapshot envelope, nonmonotonic counter or task-clock delta exceeding that TID's physical read envelope. Keep the failed raw output and stop further scheduled calls on a contract failure. The asynchronously updated RUSAGE comparison is diagnostic only and cannot reject an r4 record.
2. Report sealed versus collector-off and collector-off versus collector-on absolute times and paired ratios for source/current/official. If collector or adapter effects are comparable to or exceed the observed migration difference, do not attribute that difference using the perturbed timing. Counter evidence can remain exploratory with that limitation; do not relabel it as pure timing.
3. Report contemporaneous source/current pure time, source/current counter ratios, all six paired observations and ranges. Keep old 3-repeat and 8-repeat windows separate. If the slowdown does not recur, conclude this acquisition cannot establish the cause of the prior window. Do not dismiss the original as noise or claim statistical equivalence.
4. More instructions alongside higher CPU time suggests added executed work, but may be extra spin. Higher cycles with similar instructions may reflect stalls, frequency or wait-loop behavior. Lower per-thread CPU participation suggests off-CPU time, but cannot distinguish descheduling from blocking without more evidence. Treat these as hypotheses, not causal proof.
5. If waiting versus computation remains ambiguous, proceed to the already-existing stage profiler under a separately approved, frozen second stage. Do not construct an optimization from an ambiguous aggregate. If the first layer identifies a clear measurement defect, repair the collector and create a new run before using any numbers.

No sequential stopping for significance, outlier removal or replacement of slow repetitions. The proposed six-repeat matrix ends after its fixed 54 calls. A later larger or longer-window experiment is a new plan; repeating the same records would increase executions, not independent input count.

## 8. Conditional existing stage profiler

Use `AE/native/pipeline_measurement.h::profile_pipeline`, which already emits:

- `pipeline_shots.csv`: ready wait, decode, prior-buffer reset, result wait, own reset, shared dispatch/join/merge, per-shot caller latency and identity.
- `pipeline_components.csv`: prepare, predecode, import, MWPM, extract and continuation durations by component.
- With timeline mode, `pipeline_timeline.csv`: buffer/component/partition intervals and absolute endpoints, allowing overlap inspection.

Begin with `pipe-stages`, `STRUCTURED_PROFILE_TIMERS=1`, `STRUCTURED_PROFILE_TIMELINE=0`, other counters/NVTX/profile flags off. This needs **a separate instrumented source and candidate core build** as well as adapters; defining the macro only on an adapter linked to the pure core risks missing instrumentation or an ABI mismatch. Freeze both builds with matching flags, unchanged algorithms and the exact original source identities. The official reference remains its original normal serial path; there is no invented upstream pipeline-stage analogue.

Proposed second-stage ceiling: six repeats of source stages, candidate stages and official normal batch = 18 calls on the same full 10,000-record stream, with paired source/candidate ordering alternated and official position rotated. It runs only if the first-stage review warrants it and root authorizes its builds/acquisition. Do not collect PMU simultaneously. Use `pipe-timeline` only if stage durations leave a specific overlap question unresolved; its additional endpoint instrumentation requires a new recorded build/plan, not an ad hoc flag switch on the stages binary.

Profile warmup is inside the same resident team and calls `align_pipeline_warmup`. Profile mode copies per-shot/reset records after the caller stop clock and includes those copies in complete process wall/CPU time. Final drain is separately reported in profile mode, unlike the pure batch last-block drain. Preserve these differences; never compare profile `caller_elapsed_ns` directly with complete pure batch time as if the windows matched.

Compare waits separately by buffer parity, same original shot IDs and process repetition. Align all component phases to that same shot; retain overlapping/nested intervals and reset ownership. Durations from parallel components and buffers are not additive wall fractions. Increased result-wait can be a consequence of slower remote computation, so examine the corresponding remote component phases before naming synchronization as the cause. More instrumented MWPM time is also not proof that the pure binary spent the same additional amount there.

Existing import PMU is not the first diagnostic: it samples event/import construction scopes on selected calls, with its own owner-thread lifecycle and raw cache events. It does not measure the whole pipeline or its busy waits. Existing `--mode pmu` uses `CacheReadCounters` on the calling thread and the pure pipeline dispatcher accepts only `pipe-batch`/`pipe-latency`; it cannot be relabeled as an all-worker pipeline counter mode.

If these two layers still do not distinguish added computation from spin, the result is an explicitly unresolved dynamic diagnosis. A targeted IP-sampling study mapped to actual wait-loop ranges may then be proposed, with its own sample-quality and perturbation limits. It is outside this initial patch and protocol. No full matrix optimization validation starts until there is a concrete supported change to test.

## 9. Deliverables and next authorization boundary

Parent reviewed the four-item patch scope and authorized the completed r4 build/smoke. Before formal acquisition: retain the isolated source snapshot/diff, all three library hashes, actual build commands and binary hashes, collector on/off semantics, capability checks, static function comparisons, the exact 54-call plan, input seal and correctness contract. Give root the plan/controller hashes and one launch command; root coordinates serialized execution. `build-r4/assembly-comparison-literals.json` records equal normalized instruction streams for 12 selected source functions, 12 selected candidate functions and one official decode function versus their sealed counterparts, resolving relocated string literals from ELF bytes. This checks selected functions, not whole-executable equivalence.

New results live under a fresh `/tmp/beyond-dynamic-diagnosis/<run-id>` before archival. Persist all raw per-thread counters, block/shot outputs, failed calls and logs. A later report update will keep original performance windows and the rejected outlining result unchanged, add this diagnosis with its own acquisition identity, and distinguish an observation from any adopted remedy. Current status: **r3 evidence retained; r4 adapters and six-call capability smoke complete; formal 54-call acquisition not started**.
