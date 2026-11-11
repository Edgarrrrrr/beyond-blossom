#!/usr/bin/env python3
"""Compare the actual archived objects; do not count stale build-directory files."""
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parent
OLD = Path('/tmp/beyond-build/candidate')
NEW = ROOT / 'build'
OUT = ROOT / 'object-comparison'
OUT.mkdir(exist_ok=True)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def archive_objects(build):
    command = (build / 'CMakeFiles/libbeyond_blossom.dir/link.txt').read_text().splitlines()[0]
    return [arg for arg in shlex.split(command) if arg.endswith('.o')]

old_objects = archive_objects(OLD)
new_objects = archive_objects(NEW)
assert old_objects == new_objects, 'Archive object lists differ'
rows = []
for rel in new_objects:
    a, b = OLD / rel, NEW / rel
    rows.append(dict(object=rel, old_sha256=sha(a), new_sha256=sha(b), byte_equal=a.read_bytes() == b.read_bytes()))
launch = json.loads((ROOT / 'launch.json').read_text())
report = dict(
    old_archive_sha256=sha(OLD / 'libbeyond_blossom.a'),
    new_archive_sha256=sha(NEW / 'libbeyond_blossom.a'),
    archive_byte_equal=(OLD / 'libbeyond_blossom.a').read_bytes() == (NEW / 'libbeyond_blossom.a').read_bytes(),
    old_archive_unchanged=sha(OLD / 'libbeyond_blossom.a') == launch['original_library_sha256'],
    linked_object_count=len(rows),
    byte_equal_count=sum(row['byte_equal'] for row in rows),
    stale_old_objects=sorted(str(p.relative_to(OLD)) for p in (OLD / 'CMakeFiles/libbeyond_blossom.dir').rglob('*.o') if str(p.relative_to(OLD)) not in old_objects),
    compiler=subprocess.check_output(['/usr/bin/c++', '--version'], text=True),
    objects=rows,
)
(OUT / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
for name, build in [('old', OLD), ('new', NEW)]:
    (OUT / f'{name}-link.txt').write_text((build / 'CMakeFiles/libbeyond_blossom.dir/link.txt').read_text())
    (OUT / f'{name}-flags.make').write_text((build / 'CMakeFiles/libbeyond_blossom.dir/flags.make').read_text())
print(json.dumps({key: value for key, value in report.items() if key != 'objects'}, indent=2))
assert report['old_archive_unchanged']
assert report['archive_byte_equal'] and report['byte_equal_count'] == report['linked_object_count']
