"""Hash and JSON helpers for immutable historical measurement bundles."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path

def need(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            result.update(block)
    return result.hexdigest()


def ref(path, expected=None):
    path = Path(path).resolve()
    result = dict(path=str(path), sha256=sha(path))
    need(expected is None or result['sha256'] == expected, 'Frozen file changed: ' + str(path))
    return result


def load(path):
    return json.loads(Path(path).read_text())


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f'.{os.getpid()}.tmp')
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def utc():
    return datetime.now(timezone.utc).isoformat()
