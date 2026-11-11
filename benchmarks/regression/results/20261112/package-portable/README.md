# Portable-baseline package audit

The source distribution and wheel were freshly built from selected committed
package paths at `d2506bde8ee9a2eba7ad97523fbc948a97142676`. All 256 source manifest files
survive the source distribution byte-for-byte. The unrelated working-tree report
changes are explicitly recorded and excluded by `git archive`; this receipt does
not claim that the whole working tree was clean.

The wheel was built offline from the unpacked source distribution, then installed
into a new target directory and tested from outside the package source directory.
All 250 packaged Python tests passed (see `summary.json` for skipped-test count),
including Python/native APIs. CLI help/error/prediction checks passed. The same
unpacked source built the native CLI before SDK install, and an independent CMake
consumer found `BeyondBlossom::decoder`, compiled and ran successfully.

Every core compile command contains the explicit `-march=x86-64` baseline; LTO
is absent. The executed controller and its hash are retained. This new audit
incorporates the already resolved native-CLI-before-install and Stim-definition
consumer setup, so the old r2 audit and its setup-failure history remain intact.

The workspace archive contains the new `.tar.gz` source package, `.whl`, selected
source tar and complete small receipts/logs. This repository directory keeps only
receipts, source hashes, commands, test results and consumer inputs. It omits
intermediate objects, installed binaries and duplicate dependency trees.
Delivery filenames and pointers were not changed by this archival task.
