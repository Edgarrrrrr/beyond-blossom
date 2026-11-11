#!/usr/bin/env python3
"""Compile the existing Neighborhood TU with conflicting global ISA defaults."""
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'strong-feature'
OUT.mkdir(exist_ok=True)
record = json.loads((ROOT / 'neighborhood-original-command.json').read_text())
original = shlex.split(record['command'])
results = []
for name, portable in [('v3_with_portable_baseline', True), ('v3_without_portable_baseline', False)]:
    command = original.copy()
    command.insert(1, '-march=x86-64-v3')
    if not portable:
        command.remove('-march=x86-64')
    output = OUT / f'{name}.o'
    command[command.index('-o') + 1] = str(output)
    start = time.time()
    with (OUT / f'{name}.log').open('w') as log:
        proc = subprocess.run(command, cwd=record['directory'], stdout=log, stderr=subprocess.STDOUT)
    text = (OUT / f'{name}.log').read_text()
    results.append(dict(name=name, command=command, directory=record['directory'], exit_code=proc.returncode,
                        wall_seconds=time.time()-start, expected_success=portable,
                        target_mismatch='target specific option mismatch' in text,
                        object_sha256=hashlib.sha256(output.read_bytes()).hexdigest() if output.exists() else None))
    print(f'{name}: exit {proc.returncode}', flush=True)
    (OUT / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
assert results[0]['exit_code'] == 0
assert results[1]['exit_code'] != 0 and results[1]['target_mismatch']
