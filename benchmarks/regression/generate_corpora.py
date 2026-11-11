#!/usr/bin/env python3
"""Create deterministic arbitrary-graph inputs and saved syndrome records."""
import json
import pathlib
import random
import sys

root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
rng = random.Random(728191)
manifest = []


def graph_case(name, nodes, degree, shot_count, event_count, paired=False, separated=False):
    pairs = {(node, (node + 1) % nodes) for node in range(nodes)}
    pairs = {(min(u, v), max(u, v)) for u, v in pairs}
    target = nodes * degree // 2
    while len(pairs) < target:
        u, v = rng.sample(range(nodes), 2)
        pairs.add((min(u, v), max(u, v)))
    edges = [(u, v, 2 * rng.randint(1, 8), 1 << rng.randrange(4)) for u, v in sorted(pairs)]
    graph_path, shots_path = root / f'{name}.graph', root / f'{name}.shots'
    with graph_path.open('w') as output:
        output.write(f'{nodes} 4 {len(edges)}\n')
        for edge in edges:
            output.write(' '.join(map(str, edge)) + '\n')
    with shots_path.open('w') as output:
        for _ in range(shot_count):
            if separated:
                offset = rng.randrange(2)
                shot = list(range(offset, nodes, 2))
            elif paired:
                syndrome = set()
                for u, v, *_ in rng.sample(edges, event_count // 2):
                    syndrome.symmetric_difference_update((u, v))
                shot = sorted(syndrome)
            else:
                shot = sorted(rng.sample(range(nodes), event_count))
            output.write(f'{len(shot)} ' + ' '.join(map(str, shot)) + '\n')
    manifest.append({'name': name, 'kind': 'graph', 'graph': str(graph_path), 'shots': str(shots_path),
                     'nodes': nodes, 'average_degree': degree, 'shot_count': shot_count,
                     'requested_events': event_count, 'paired_errors': paired, 'separated': separated})


graph_case('irregular_sparse_pairs', 4096, 6, 1000, 16, paired=True)
graph_case('irregular_sparse_unpaired', 4096, 6, 1000, 40)
graph_case('high_degree_sparse', 512, 64, 1000, 40)
graph_case('irregular_dense', 256, 8, 300, 128)
graph_case('ring_separated_dense', 512, 2, 300, 256, separated=True)
graph_case('high_degree_dense', 512, 64, 300, 256)
(root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
