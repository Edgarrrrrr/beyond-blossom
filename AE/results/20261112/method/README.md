# Simple equal-share (physical mode 3)

Open [index.html](index.html) directly in a browser. The page contains its own styles, model and source excerpts; it needs no server or network. [The independent diff page](code-diff/index.html) retains the [original Git patch](code-diff/diff.patch).

This note explains the frozen implementation at `75aedd433d1b129e5a973635320f199b5f26cbae`. The comparison endpoint before mode 3 is `dc56db5233d474c11628f392d12c2ae6f654b60b`; mode 3 was introduced by `7712b54dfc587e385c71c66433ef2af879427d3a`. The complete diff scope is one file, `src/beyond_blossom/structured/predecode/one_hop_predecoder.cc`. Additional fixed files supply helper, handoff, AE configuration and test context. See [the source manifest](sources/manifest.json) for full file hashes.

Mode 3 publishes every activity-constrained radius before matching. An active–active edge contributes `floor(w/2)` to each endpoint's cap. An edge to an inactive vertex or boundary contributes `w`. Each minimum cap is rounded down to an even radius; matching accepts only tight edges and proceeds in coordinate order. The existing physical mode 2 remains the historical coordinate/full-cap strategy. Current AE logical policy slot 2 selects physical mode 3. The production default remains mode 0.

The four interactive inputs are small, legal unrotated-stencil graphs on the even-weight domain. Reversing seed storage illustrates why coordinate ordering sorts indices without moving the seed backing. The model covers `run_fused_range` from entry through its mode-3 branch to its `void` return, including neighborhood preparation and `pair`. Construction, normalization and scratch allocation are preconditions. The caller's `finish` and the full-solver import are separate boundaries with adjacent source excerpts.

For each input, an independent edge-subset enumeration supplies a tiny correctness oracle. It does not simulate the full blossom solver or measure execution time. In the boundary example, the partial boundary match costs 2, while the whole problem's optimal correction is the A–B edge of weight 8. The full solver must retain the ability to change the partial match.

The [existing development validation receipt](evidence/mode3-validation.json) records a full native CMake build and 129 passing applicable tests, including 160 new even-weight correction cases and the existing 5,184-case exhaustive oracle. It is a development snapshot receipt, not a newly collected performance result. Its original paths are retained for traceability. The relevant native logs and source-to-revision check are copied alongside it. Low-level odd-edge examples validate only the predecoder's dual bounds; end-to-end correctness uses the decoder's even-weight quantization domain.

This method note contains no performance measurements. Consult the parent AE measured results for speedups, latency and hardware counters; the interactive examples are algorithm illustrations, not timing evidence. The fixed official upstream `e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd` remains the main performance baseline; the source diff here is an implementation explanation, not a timing denominator.

Regenerate the presentation from the included frozen sources and original patch:

```sh
python3 scripts/build_material.py
node scripts/verify_model.js
python3 scripts/verify_material.py
```

Run these commands from this directory. They do not compile or execute a native decoder. The retained `scripts/render_code_diff.py` is the standard-library renderer from the performance-experiment workflow; its original SHA is recorded in the diff manifest. `build_material.py` adds the separate Chinese line explanations and generates the mechanism page. [optimization-coverage.json](optimization-coverage.json) records identities and scope; [verification/receipt.json](verification/receipt.json) records local checks. The [browser validation index](evidence/browser-validation.json) binds the final two pages to their 1440/760-width checks and persistent screenshots. Small receipts are included here; browser profiles and full screenshots remain at the recorded workspace locations. There is no separate web publication in this task.

The copied C++ source retains its original Apache-2.0 notices. The surrounding documentation and presentation are distributed under the included [Apache-2.0 license](LICENSE).
