# Boundary outlining follow-up: variant not adopted

This separate experiment tested one possible cause of three regressions in the original 819-call migration matrix. The production runtime remains `edd29b6`. The only experimental source change added `[[gnu::noinline]]` to `structured::Mwpm::handle_tree_hitting_boundary`; it was **not merged**. The original measurements and their conclusions remain in the [validation report](../report/index.html).

Five arms used the fixed official `e27b8c6` reference, migration source `2411603`, the sealed candidate, a rebuilt candidate, and the outlined variant. The two candidate controls are byte-identical, including the linked core and Stim libraries. Each of three selected cases reused its original 10,000 saved records for eight process repetitions: 120 native calls and **30,000 distinct saved records**, not 1.2 million independent samples. All calls passed integer-weight and observable checks. Official measurements use one CPU; the three structured methods use two, four, and eight CPUs.

Negative changes below mean lower elapsed time. The original column uses three process repetitions; the follow-up uses eight. These windows are not pooled.

| Case / method | Original candidate vs source | Follow-up candidate vs source | Variant vs rebuilt, ratio of medians | Variant vs rebuilt, median paired change | Faster variant pairs |
| --- | ---: | ---: | ---: | ---: | ---: |
| d17, p=.003, component-parallel | +3.4265% | -1.3897% | -0.2975% | +0.0320% | 4/8 |
| d17, p=.001, pipelined | +1.4384% | +0.8876% | -0.5708% | -0.3032% | 5/8 |
| d33, p=.0005, parallel-predecode | +1.4503% | -0.5123% | -0.6086% | -0.9256% | 6/8 |

Outlining shrank the two timeline functions from 5528/5469 to 511/467 bytes, and their local stack frames from 232 to 56 bytes. That establishes the code-generation difference. It does not establish that this difference caused the original regressions. The largest original regression case has no consistent paired gain; the outlined pipeline remains 0.5907% slower than the migration source. The byte-identical controls also show process variation, including a slow pipeline launch whose ten blocks and complete measurement are retained. No sample was removed, and the observed ranges are not confidence intervals or equivalence bounds.

The fixed official baseline remains separate: official/variant median speedups are 4.5211x, 4.6955x and 8.8772x, with the thread counts stated above. These selected cases do not establish performance across the full 126 comparisons. The evidence did not justify adopting this variant or starting another full matrix.

Inspect [the frozen protocol](PROTOCOL.md), [raw measurements](raw.csv), [summary](summary.json), [independent audit](audit.json), [both windows](window-comparison.json), [static assembly receipt](static-assembly.json), and [the complete patch](outline-boundary.patch). The [before](source-excerpts/mwpm-before.inl) and [experimental](source-excerpts/mwpm-outlined.inl) headers retain the full source context and copyright. The original four-arm protocol is preserved as [unexecuted revision r1](PROTOCOL.r1-four-arm.md).

The separate [portable-baseline receipt](portable-baseline-validation-receipt.json) establishes that the GCC compatibility fix produced identical 44/44 linked objects and the complete library with local GCC 12.2. The real GCC 15 CI result is a separate publication check; the local receipt does not claim that compiler was tested here.

[Archive metadata](archive.json) identifies the durable workspace archive, its hash and its path map. It contains all five binaries, all inputs and per-call outputs, the exact source snapshot and patch, build receipts and libraries, assembly, and validation logs. Original receipt paths are preserved verbatim; the path map resolves them to archive members. CMake intermediate objects are excluded.

After extracting the archive into a new directory, its saved summary can be verified without running a decoder:

```sh
python3 EXTRACTED/boundary-outline/prepared-run/controller/run_confirmatory.py \
  --summarize-only --output EXTRACTED/boundary-outline/prepared-run
```

The frozen execution commands describe the historical run and retain their original absolute paths. A new collection needs a new prepared directory and explicit execution authorization; it must not overwrite these records.
