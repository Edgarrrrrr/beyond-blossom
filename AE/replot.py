#!/usr/bin/env python3
"""Verify bundled evidence and regenerate figures and paper values offline."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    output=args.output.resolve()
    if output.exists():parser.error('Use a new output directory')
    if HERE==output or HERE in output.parents and output.parts[len(HERE.parts)] in {'data','reference','m4_pro'}:
        parser.error('Output must not replace bundled evidence')
    output.mkdir(parents=True)
    commands=[
        ['pmu_data.py','verify'],['tailreset_data.py','verify'],
        ['migrate_paper_numbers.py','--check'],['generate_predecoder_macros.py','--check'],
        ['plot.py','all','--output',str(output/'figures')],
        ['m4_pro/plot_m4_per_round.py','--output-dir',str(output/'figures/m4')],
        ['migrate_paper_numbers.py','--output',str(output/'performance-macros.tex')],
        ['generate_predecoder_macros.py','--output',str(output/'predecoder_numbers.tex')],
    ]
    for command in commands:
        subprocess.run([sys.executable,str(HERE/command[0]),*command[1:]],check=True)
    (output/'complete.json').write_text(json.dumps(dict(complete=True,scope='Bundled evidence checks, numerical regeneration and figure rendering; no decoder execution',commands=commands),indent=2)+'\n')

if __name__=='__main__':main()
