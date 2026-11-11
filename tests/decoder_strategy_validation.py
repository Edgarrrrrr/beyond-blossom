"""Run the shared-library regression binaries and preserve exact receipts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import xml.etree.ElementTree as ET


def reference(path):
    path = Path(path).resolve()
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return {"path": str(path), "sha256": digest.hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
    env["OMP_DYNAMIC"] = "FALSE"
    # Decoder teams retain their own atomic waits. Park the OpenMP runtime pool
    # after a session so it does not contend with subsequent regression cases.
    env["OMP_WAIT_POLICY"] = "PASSIVE"
    env["GOMP_SPINCOUNT"] = "0"
    records = []
    success = True
    for name in ("beyond_blossom_tests", "beyond_blossom_profile_tests"):
        executable = build / name
        xml = output / (name + ".xml")
        log = output / (name + ".log")
        record = {"binary": reference(executable), "started_unix": time.time()}
        command = [str(executable), "--gtest_output=xml:" + str(xml)]
        with log.open("wb") as stream:
            result = subprocess.run(command, cwd=root, env=env, stdout=stream, stderr=subprocess.STDOUT)
        record.update(argv=command, exit_code=result.returncode, log=reference(log), finished_unix=time.time())
        ok = result.returncode == 0 and xml.is_file()
        if xml.is_file():
            tree = ET.parse(xml)
            cases = tree.findall(".//testcase")
            record.update(xml=reference(xml), tests=len(cases),
                          failures=len(tree.findall(".//failure")),
                          errors=len(tree.findall(".//error")),
                          skipped=len(tree.findall(".//skipped")),
                          not_run=sum(c.get("status") == "notrun" for c in cases))
            ok = (ok and len(cases) > 0 and
                  int(tree.getroot().get("tests", "0")) == len(cases) and
                  not any(record[k] for k in ("failures", "errors", "skipped", "not_run")))
        record["passed"] = bool(ok)
        records.append(record)
        success = success and ok
        print(name, "PASS" if ok else "FAIL", flush=True)
    receipt = {"complete": bool(success), "host": socket.gethostname(),
               "allowed_cpus": sorted(os.sched_getaffinity(0)),
               "slurm_job_id": os.environ.get("SLURM_JOB_ID"),
               "controller": reference(__file__), "build_directory": str(build),
               "tests": records}
    (output / "complete.json").write_text(json.dumps(receipt, indent=2) + "\n")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
