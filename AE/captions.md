# Figure captions

## Predecoder strategies

Mean remaining syndromes after predecoding and mean full decoding time for three predecoder strategies in the current four-core TailReset decoder. Each setting uses 1,000,000 independent inputs, p = 0.3%, rounds = d, and LTO OFF. The no-predecoder series retains the normalized initial syndrome count. Coordinate-ordered predecoder visits embedded grid coordinates; Degree-ordered predecoder uses the current fused degree-ordered one-hop algorithm. Decoding time pools three fresh-process block-timing passes and includes predecoding, dispatch, waits, result stores, resets and final pipeline drain. A separate compile-time instrumented pipeline replay counts unmatched seeds after the joined prefix and before full MWPM; those replay times do not enter the timing bars. Data: AE-TAILRESET-20261110/formal-r04, completed 2026-11-11.

## Decoder comparison

Mean native decoding time versus code distance under uniform noise. Panels show p = 0.05%, 0.1%, and 0.3%, with rounds = d, 1,000 independent inputs per setting, and three repetitions. Beyond blossom selects the minimum three-repeat mean among six measured configurations in each setting, using 1–8 physical cores; PyMatching uses one core. Fusion blossom uses two primal workers with four-core process affinity. All methods share the same host and inputs within a setting.

Native windows follow each implementation: Parity/Fusion/Parity-UF include solve and subgraph extraction with clear outside; UnionFind includes boundary-adapted decode with projection and clear outside; LDPC-UF uses a prepared core with cluster preparation and cleanup outside. The three Union-Find configurations perform approximate decoding.

## Structured-grid speedup

SIMD-Enabled Structured Grid speedup over PyMatching for mean and 99th-percentile latency. Each setting uses one million independent inputs and three complete-sequence repetitions with the repaired benchmark. Each input contributes its minimum measured time; percentiles use empirical nearest ranks. Both configurations use one thread and no predecoder. The horizontal line marks equal performance.

## Cache locality

Memory-access measurements for d = rounds = 29 and p = 0.3%. Bars show three-round aggregate values; whiskers show the observed range. Load events and L2 fills are normalized per input. L2 miss intensity is the ratio of summed L2-missing retired loads to simultaneous summed retired loads, multiplied by 1,000. L2 fill payload uses 64 bytes per cache line; it measures cache-fill volume rather than DRAM traffic. Retired-load events include software prefetches.

## Tail latency

Strict empirical tail probability P(L > t) for d = rounds = 29 and p = 0.3%, using all one million input values per decoder after taking the minimum of three complete-sequence repetitions. Curves use exact multiplicities for every distinct measured latency. Zero tail probability falls below the logarithmic axis; 10⁻⁶ corresponds to one input. Both decoders use the same physical inputs, one thread, and no predecoder.

## Incremental ablation

Incremental Beyond blossom ablation for three noise rates and seven code distances. Each cell shows the measured speedup over official PyMatching v2.3.1 (LTO OFF) and pooled batch time per input in microseconds. GridSIMD, Predecode, Component, TailReset and Spatial use LTO ON. Each setting contains one million independent inputs and three batch measurement rounds, including final drain; each call has at least two seconds of separate warmup. Per-shot latency measurement is disabled. Orange-red outlines mark the minimum time in each row. Worker counts are 1, 1, 1, 2, 4 and 8 across columns. Labels show two speedup decimals and three time decimals; the bundled data retains full precision. All panels share a horizontal 0–12× color scale below the method labels, with zero outer export padding. The comparison includes both implementation and LTO differences.
