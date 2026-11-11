#!/usr/bin/env python3
"""Seal portable baseline evidence without build objects or credentials."""
from pathlib import Path
import datetime
import hashlib
import json
import shutil

WORKSPACE = Path('/home/fujy/dev/beyond_blossom/validation/20261112/portable-baseline')
REPO = Path('/home/fujy/dev/beyond_blossom/repo/benchmarks/regression/results/20261112/portable-baseline')
LOCAL = Path('/tmp/beyond-portable-baseline-validation')
FAILED = Path('/tmp/beyond-github-ci-final')
copied = []

def copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    copied.append({'source': str(source), 'destination': str(destination),
                   'sha256': hashlib.sha256(destination.read_bytes()).hexdigest()})

for source in sorted(LOCAL.rglob('*')):
    rel = source.relative_to(LOCAL)
    if source.is_file() and 'build' not in rel.parts and source.suffix not in {'.o', '.a', '.so'}:
        copy(source, WORKSPACE / 'local' / rel)
for source in sorted(FAILED.rglob('*')):
    if source.is_file() and source.suffix not in {'.o', '.a', '.so'}:
        copy(source, WORKSPACE / 'ci-failed-run-43091092149' / source.relative_to(FAILED))

run_bytes = Path('/tmp/beyond-github-run-current.json').read_bytes()
jobs_bytes = Path('/tmp/beyond-github-jobs-current.json').read_bytes()
run, jobs = json.loads(run_bytes), json.loads(jobs_bytes)
assert run['id'] == 43094516453
assert run['head_sha'] == 'd2506bde8ee9a2eba7ad97523fbc948a97142676'
ci = WORKSPACE / 'ci-fixed-run-43094516453'
ci.mkdir(parents=True, exist_ok=True)
(ci / 'run-pending.json').write_bytes(run_bytes)
(ci / 'jobs-steps-passed-pending.json').write_bytes(jobs_bytes)
steps = [step for job in jobs['jobs'] for step in job['steps']]
assert all(step['status'] == 'completed' and step['conclusion'] == 'success' for step in steps)
observation = {
    'recorded_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'run_id': run['id'], 'head_sha': run['head_sha'], 'url': run['html_url'],
    'run_status': run['status'], 'run_conclusion': run['conclusion'],
    'jobs': [{'id': job['id'], 'status': job['status'], 'conclusion': job['conclusion']} for job in jobs['jobs']],
    'all_reported_steps_completed_successfully': True,
    'reported_successful_step_count': len(steps),
    'assessment': 'steps passed; overall conclusion pending',
    'job_log_available_in_this_archive': False,
    'job_log_download_note': 'Parent reported HTTP 502; no new network request was made by this archiver.',
    'gcc15_target_features_and_skipped_tests': 'Not parsed: fixed-run complete job log is not available yet.',
}
(ci / 'observation-pending.json').write_text(json.dumps(observation, indent=2) + '\n')

readme = '''# Portable x86 baseline validation

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
'''
WORKSPACE.mkdir(parents=True, exist_ok=True)
(WORKSPACE / 'README.md').write_text(readme)
copy(Path(__file__), WORKSPACE / 'archive_portable_baseline_validation.py')

# Keep every small receipt, command and log in the repository, but omit the large
# discovery listings, local CMake cache and redundant intermediate API snapshots.
for source in sorted(WORKSPACE.rglob('*')):
    if not source.is_file():
        continue
    rel = source.relative_to(WORKSPACE)
    if source.name in {'old-test-list.json', 'new-test-list.json', 'original-CMakeCache.txt', 'artifact-sha256.json'}:
        continue
    if len(rel.parts) > 1 and rel.parts[0] == 'ci-failed-run-43091092149' and source.name.startswith('202611'):
        continue
    copy(source, REPO / rel)

def manifest(root):
    values = {str(path.relative_to(root)): {'bytes': path.stat().st_size,
              'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
              for path in sorted(root.rglob('*')) if path.is_file() and path.name != 'manifest.json'}
    (root / 'manifest.json').write_text(json.dumps({'files': values, 'file_count': len(values),
                                                  'total_bytes': sum(v['bytes'] for v in values.values())}, indent=2) + '\n')
    return {'path': str(root), 'file_count': len(values), 'total_bytes': sum(v['bytes'] for v in values.values())}

print(json.dumps({'workspace': manifest(WORKSPACE), 'repository': manifest(REPO),
                  'fixed_ci_assessment': observation['assessment']}, indent=2))
