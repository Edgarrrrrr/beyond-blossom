#!/usr/bin/env python3
"""Verify the compact report's source map, hashes and offline links."""
from pathlib import Path
from html.parser import HTMLParser
from urllib.parse import unquote, urlsplit
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Links(HTMLParser):
    def __init__(self):
        super().__init__()
        self.links = []
        self.ids = set()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            self.ids.add(attrs["id"])
        for name in ("href", "src"):
            if name in attrs:
                self.links.append(attrs[name])


def main():
    sources = json.loads((ROOT / "sources/manifest.json").read_text())
    annotations = json.loads((ROOT / "sources/line-explanations.json").read_text())
    count = 0
    unique = set()
    for name, sides in sources["excerpts"].items():
        for side, source in sides.items():
            if source is None:
                continue
            excerpt = ROOT / source["export_path"]
            full = ROOT / source["full_source"]
            assert sha(excerpt) == source["excerpt_sha256"], excerpt
            assert sha(full) == source["source_sha256"], full
            body = "".join(full.read_text().splitlines(True)[source["start"] - 1:source["end"]])
            assert body == source["text"] == excerpt.read_text(), excerpt
            lines = annotations["excerpts"][name][side]
            assert len(lines) == source["end"] - source["start"] + 1
            for number, (line, note) in enumerate(zip(body.splitlines(), lines), source["start"]):
                assert note["source"] == line and note["line"] == number
                if line.strip():
                    assert note["explanation"].strip(), (name, side, number)
                    unique.add((source["commit"], source["path"], number))
                    count += 1
    assert count == annotations["nonempty_lines"]
    coverage = json.loads((ROOT / "optimization-coverage.json").read_text())
    for item in coverage["optimizations"]:
        for name in ("html", "line_explanations", "summary"):
            assert sha(ROOT / item[name]["path"]) == item[name]["sha256"], (item["optimization_id"], name)
    diff = json.loads((ROOT / "code-diff/manifest.json").read_text())
    for name in ("source_mapping", "line_explanations"):
        assert sha(ROOT / "code-diff" / diff[name]["path"]) == diff[name]["sha256"]
    documents = {}
    for path in ROOT.rglob("*.html"):
        parser = Links()
        parser.feed(path.read_text())
        documents[path.resolve()] = parser
    links = 0
    for path, parser in documents.items():
        for link in parser.links:
            parts = urlsplit(link)
            if parts.scheme == "data":
                continue
            assert not parts.scheme and not parts.netloc, (path, "non-offline link", link)
            target = (path.parent / unquote(parts.path)).resolve() if parts.path else path
            assert target.is_relative_to(ROOT), (path, "outside report", link)
            assert target.is_file(), (path, "missing target", link)
            if parts.fragment and target in documents:
                assert unquote(parts.fragment) in documents[target].ids, (path, "missing anchor", link)
            links += 1
    result = {
        "status": "passed", "kind": "static report verification",
        "source_scopes": len(sources["excerpts"]), "explained_nonempty_lines": count,
        "unique_source_lines": len(unique), "standalone_html_pages": len(documents),
        "verified_local_links": links, "browser_execution": "separate verification",
        "summary_sha256": sha(ROOT / "summary.json"),
        "source_manifest_sha256": sha(ROOT / "sources/manifest.json"),
        "coverage_sha256": sha(ROOT / "optimization-coverage.json"),
    }
    (ROOT / "static-verification.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
