# CPU accounting reference correction

The r3 source/on 32-record capability smoke passed exact weights and observables, thread mapping, hardware event IDs, complete reads and non-multiplexed hardware counting. It failed the draft CPU-accounting gate. No full diagnostic matrix had started.

| Quantity | Observed ns |
| --- | ---: |
| Sum of four per-thread task-clock deltas | 649492 |
| Native process getrusage user + system delta | 147000 |
| Sum of conservative extra read envelopes | 73958 |
| Draft upper bound, process delta + envelopes + 4000 | 224958 |

The 4000 ns allowance covered timeval rounding only. It did not account for when running threads on other CPUs update the process accounting fields.

Upstream Linux v6.1 `kernel/sys.c` routes `RUSAGE_SELF` through `thread_group_cputime_adjusted`. In `kernel/sched/cputime.c`, `thread_group_cputime` refreshes the current task's runtime, but uses already-accounted runtime for other running tasks to avoid the cost of refreshing other CPUs. Those remote values can await a tick or scheduler action. This invalidates the claimed four-microsecond reference precision. It supports a delayed-accounting explanation for this observation; it does not quantitatively attribute the entire difference to one cause.

Primary sources:

- https://raw.githubusercontent.com/torvalds/linux/v6.1/kernel/sys.c
- https://raw.githubusercontent.com/torvalds/linux/v6.1/kernel/sched/cputime.c

The host runs Debian kernel `6.1.0-41-amd64`. `kernel-accounting.json` records the actual configuration path, hash, relevant options and runtime `nohz_full` value. The upstream source explains the mechanism; it is not represented as a byte-identical copy of the installed Debian kernel source.

An independent check, `task_clock_probe.cc`, ran on CPUs 2, 4, 6 and 8. Each worker sampled its own `CLOCK_THREAD_CPUTIME_ID` before and after each software task-clock read, then executed separate 1 ms and 50 ms spin windows. In all eight records, the perf task-clock delta lay inside the strict CPU-clock brackets. No tolerance was fitted and no constant was subtracted. Full readings are in `task_clock_probe.json`. This is a counter capability check, not a decoder benchmark or a useful-work versus spin classifier.

Revision r4 keeps all r3 raw files and removes the unsupported precise getrusage gate. It continues to report getrusage for comparison. Per-thread task-clock readings must remain monotonic and fit within their conservative monotonic-clock read envelopes. Hardware groups must still have exact event-ID coverage, complete reads and equal enabled/running deltas; worker identity and affinity checks remain mandatory. The predeclared full-window 0.25% extra-envelope limit is unchanged. No decoder algorithm, original performance record or performance-acceptance threshold changes.
