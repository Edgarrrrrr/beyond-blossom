#!/usr/bin/env python3
"""Seal final offline report files after numerical and browser verification."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    summary = json.loads((ROOT / "summary.json").read_text())
    browser = json.loads((ROOT / "browser-verification.json").read_text())
    static = json.loads((ROOT / "static-verification.json").read_text())
    models = json.loads((ROOT / "model-verification.json").read_text())
    assert summary["status"] == "complete"
    assert browser["status"] == static["status"] == "passed"
    assert models["complete"] and models["model_sha256"] == sha(ROOT / "scripts/mechanisms.js")
    assert models['outline_model_sha256'] == sha(ROOT / 'scripts/boundary_outline.js')
    assert browser['scenarios_per_viewport'] == models['scenarios'] == 18
    assert static["summary_sha256"] == sha(ROOT / "summary.json")
    for path, expected in browser["page_sha256"].items():
        assert sha(ROOT / path) == expected, path
    coverage_file = ROOT / "optimization-coverage.json"
    coverage = json.loads(coverage_file.read_text())
    for item in coverage["optimizations"]:
        item["verification"]["browser"] = "passed: browser-verification.json"
        item["verification"]["semantic_models"] = "passed: model-verification.json"
    coverage_file.write_text(json.dumps(coverage, indent=2, ensure_ascii=False) + "\n")
    # Refresh the static receipt's coverage link after recording the browser status.
    static["coverage_sha256"] = sha(coverage_file)
    (ROOT / "static-verification.json").write_text(json.dumps(static, indent=2) + "\n")
    files = []
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file() or path.name == "manifest.json" and path.parent == ROOT:
            continue
        assert "__pycache__" not in path.parts and path.suffix != ".pyc", path
        assert path.name != "previous-patch.sha256", "Remove draft patch comparison before sealing"
        files.append({"path": str(path.relative_to(ROOT)), "bytes": path.stat().st_size, "sha256": sha(path)})
    manifest = {
        "schema": 1, "report_revision": 2, "complete": True, "kind": "standalone offline migration and optimization report",
        "runtime_commit": summary["implementation_commit"],
        "collector_commits": summary["collector_commits"],
        "migration_source_revision": summary["migration_source_revision"],
        "official_revision": summary["official_revision"],
        "followup": {"decision": summary['boundary_outline']['decision'], "native_calls":120,
                     "source_snapshot":summary['boundary_outline']['base_commit'],
                     "distinct_saved_records":30000,"process_repeats":8},
        "summary_sha256": sha(ROOT / "summary.json"),
        "files": files, "total_bytes": sum(item["bytes"] for item in files),
        "publication": "Offline files only; no external deployment or document publication",
        "large_inputs_and_binaries": "Excluded from this compact report; archived run manifests identify their hashes and parent workspace archival location",
        "verification": {
            "sources_and_links": "static-verification.json",
            "models": "model-verification.json",
            "browser": "browser-verification.json",
            "native_package_and_correctness": "validation.json",
        },
    }
    (ROOT / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({"complete": True, "files": len(files), "bytes": manifest["total_bytes"]}))


if __name__ == "__main__":
    main()
