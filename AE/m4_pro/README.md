# M4 Pro measurements

`data/feishu_m4_six_roles.csv` preserves 126 rows: seven distances, three error
rates, and six configurations. Each setting uses one million saved inputs.
`time_us` is the median of the three complete-batch means. The plotting code
checks those values and common input hashes before selecting the fastest
optimized configuration for each setting.

```sh
python3 AE/m4_pro/plot_m4_per_round.py --output-dir /absolute/path/new-m4-figures
```

The output contains six curves, 42 selected rows, a PDF/SVG/PNG, and
`plot_summary.json` with speedups and the one-microsecond-per-round coverage.
Per-round time is block time divided by distance; it is an amortized processing
budget, not a per-shot latency bound.

The recorded machine is a 14-core Apple M4 Pro, 24 GiB memory, Apple Clang 21,
C++20, `-O3 -mcpu=apple-m4`, and LTO off. Threads used USER_INITIATED QoS;
physical P/E affinity was not established. The final two configurations were
measured in a later acquisition window. `source/platform_metadata.json` and
original CSV hashes retain these distinctions. This command recalculates archived
measurements and does not run a decoder on M4.
