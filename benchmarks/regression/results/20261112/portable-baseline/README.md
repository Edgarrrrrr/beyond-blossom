# Portable x86 baseline validation

The compatibility fix is commit `d2506bde8ee9a2eba7ad97523fbc948a97142676`.
It changes CMake, CI and documentation; `src/` and `python/` are unchanged from
the measured implementation `edd29b6780713dc78f6a99d67c4c4d192e9c52b5`.

Local GCC 12.2 validation found all 44 linked core objects and the complete core
archive byte-identical before and after the explicit `-march=x86-64` baseline.
Both archive SHA256 values are
`b3f84f05588766cfe38e0ad9e47e65b84d6a2350ca176b192b9e80b1fb586a7a`.
The four old-path objects listed in the comparison were stale files excluded from
both current archive link commands. No CMake build objects or duplicate archive
are stored here; the exact hashes and link commands identify the compared code.

The existing Neighborhood test translation unit compiled with global
`-march=x86-64-v3` followed by the target's `-march=x86-64`. Its object was also
byte-identical to the ordinary build. Removing that baseline reproduced the
`always_inline` target mismatch. The two Neighborhood tests passed, and the full
native CTest run passed all 356 tests in 18.40 seconds.

The first local CTest invocation is retained: 349 tests passed and the unbuilt
profile-test target appeared as one `NOT_BUILT` placeholder. The separate profile
target was then built, and the complete 356-test run passed. This setup history is
not omitted or reported as an algorithm failure.

`ci-failed-run-43091092149/` preserves the original CI failure, including run/job
API responses and the full job log. The original CI used GCC 15.2.0 and failed
while compiling the Neighborhood test; Python tests had passed and CTest was
skipped. The exact original compiler default feature set was not printed, so
the global-feature explanation is a reproduced mechanism, not an assertion that
all Ubuntu 26.04 installations default to x86-64-v3.

For fixed run `43094516453`, all 12 reported steps, including native compilation
and CTest, have completed successfully. Its archived API snapshots still report
the run and job as `in_progress` with null conclusions: **steps passed; overall
conclusion pending**. The complete new job log was unavailable (HTTP 502 reported
by the parent), so this archive does not invent GCC 15 default-feature or skipped
test counts. A later terminal receipt can supplement these immutable pending
snapshots.

`local/validation-receipt.json` is the local result index;
`local/source-identity.json` records actual source hashes. Commands and logs are
retained for both strong-feature controls and test invocations. The workspace
archive also retains the original CMake cache and complete test discovery JSON.
No timing, runtime modification or additional network request was performed
during this archival step.
