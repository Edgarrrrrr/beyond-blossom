# Final hosted CI observation

GitHub Actions [run 43094516453](https://github.com/Edgarrrrrr/beyond-blossom/actions/runs/43094516453) and its Linux job `130779786311` both finished with `status=completed`, `conclusion=success` for commit `d2506bde8ee9a2eba7ad97523fbc948a97142676`. All 12 reported steps succeeded. The earlier pending observation remains unchanged in `../portable-baseline/ci-fixed-run-43094516453/`.

| Recorded check | Final log result |
| --- | --- |
| Compiler | GNU C++ 15.2.0, Ubuntu 15.2.0-16ubuntu1 |
| Compiler default target query | `c++ -Q --help=target`: `-march=x86-64-v3`, `-mpopcnt [enabled]` |
| Python | Python 3.11.18; 250 collected, **248 passed, 2 skipped**, 3 warnings; 8.83 seconds |
| CTest | 356 scheduled, **355 passed, 1 skipped**, 0 failed; 250.08 seconds |

CTest test 20, `DecoderStrategy.ParallelSessionEmptyOddExceptionalStreamsAndCapabilityBoundaries`, is explicitly marked `***Skipped` and listed under “The following tests did not run.” Although the footer says “100% tests passed out of 356,” this is not 356 executed passes. The log does not report the skipped test's reason. The two Python skip reasons and individual skipped test IDs are also absent from the captured log; no reasons are inferred from source or runner characteristics.

The [audit](audit.json) checks final API states and commit identity, then independently accounts for all 356 individual CTest records and the Python summary. It links every compiler and summary fact to the original log line. The hosted result is separate from the local GCC 12.2 validation and the installed-package audit with 250 Python passes and zero skips. It makes no new performance claim.

The raw [run response](run.json), [jobs response](jobs.json), and [188,833-byte job log](job-130779786311.log) are preserved byte-for-byte. [acquisition.json](acquisition.json) records their original local paths and hashes; HTTP 200 is the collecting parent's recorded observation, rather than an HTTP transaction replayed by this audit. [preserved-history.json](preserved-history.json) records the earlier manifests and pending observations checked unchanged. The new [manifest](manifest.json) excludes its own bytes.

Replay the audit without network access, compilation, or test execution:

```sh
python3 audit.py --check
```
