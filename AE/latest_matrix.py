"""Render both latest all-LTO-OFF Batch3 matrices with the shared AE renderer."""
import argparse
import json
from pathlib import Path

import ablation
import migrate_paper_numbers as M


def render(output):
    output = Path(output).resolve()
    M.need(not output.exists(), f'Output directory already exists: {output}')
    config_path = M.HERE / 'paper_num_mapping.json'
    config = M.read_json(config_path)
    matrix = M.Matrix(config, config_path.parent)
    data = M.HERE / 'data/latest-matrix/heatmaps'
    kinds = {'latest-six': 'Upstream', 'official-reference': 'Official'}
    # Check every displayed input against the same source used for the macros.
    for kind, baseline in kinds.items():
        raw = json.loads((data / kind / 'data/ablation.json').read_text())
        M.need(raw['baseline'] == baseline and raw['build_lto'] == config['source']['lto'], 'Figure baseline or LTO setting differs')
        M.need(raw['implementation_commit'] == config['source']['implementation_commit'], 'Figure source identity differs')
        for panel in raw['panels']:
            for d, times, speedups in zip(raw['distances'], panel['time_us'], panel['speedups'], strict=True):
                case, = matrix.selected({'d': d, 'p': str(panel['p'])})
                expected = [float(case['methods'][method]['batch3_us']) for method in raw['methods']]
                M.need(times == expected and speedups == [times[0] / t for t in times], 'Macro and figure values use different sources')
    output.mkdir(parents=True)
    for kind in kinds:
        ablation.render(output / kind, data_dir=data / kind)
    (output / 'complete.json').write_text(M.json_text(dict(
        complete=True, baseline_by_figure=kinds, metric='batch3_us',
        matrix=matrix.provenance, mapping=M.reference(config_path))))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=M.HERE / 'output/latest-matrix')
    args = parser.parse_args()
    render(args.output)
