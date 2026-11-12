#!/usr/bin/env python3
"""Wait for the byte-preserved archive, then render a presentation revision."""
from pathlib import Path
import hashlib, json, os, shutil, subprocess, time

work = Path(__file__).resolve().parent
root = work.parent.parent / 'repo'
out = root / 'AE/results/20261112'
manifest_path = out / 'manifest.json'
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
while not manifest_path.is_file():
    time.sleep(60)
manifest = json.loads(manifest_path.read_text())
manifest['original_exporter_sha256'] = manifest['exporter_sha256']
manifest['exporter_sha256'] = sha(root / 'AE/export_results.py')
manifest['presentation_revision'] = 'Vertical predecoder bar labels; unchanged canonical measurements and preserved automatic figures.'
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
python = str(work / 'venv-clean/bin/python')
env = dict(os.environ, MPLCONFIGDIR=str(work / '.matplotlib'), OPENBLAS_NUM_THREADS='1')
subprocess.run([python, str(out / 'report.py')], check=True, cwd='/tmp', env=env)
redraw = work / 'final-archive-redraw'
shutil.copy2(manifest_path, out / 'evidence/validation/redraw-input-manifest.json')
subprocess.run([python, str(out / 'replot.py'), '--output', str(redraw)], check=True, cwd='/tmp', env=env)
receipt = json.loads((redraw / 'replot.json').read_text())
assert receipt['complete'] and len(receipt['files']) == 12
for name, digest in receipt['files'].items():
    assert sha(redraw / name) == digest
shutil.copytree(redraw / 'figures', out / 'presentation')
validation = out / 'evidence/validation'
shutil.copy2(redraw / 'replot.json', validation / 'final-archive-redraw.json')
shutil.copy2(__file__, validation / 'finalize-analysis.py')
print(json.dumps({'status':'complete', 'redraw_artifacts':len(receipt['files']), 'original_exporter_sha256':manifest['original_exporter_sha256'], 'exporter_sha256':manifest['exporter_sha256']}))
