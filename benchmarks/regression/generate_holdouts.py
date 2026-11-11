#!/usr/bin/env python3
"""Independent topology controls for generic predecode dispatch decisions."""
import json
import pathlib
import random
import sys

root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
rng = random.Random(58073)
manifest = []


def save(name, nodes, pairs, active_population, event_count, description):
    edges = [(u, v, 2 * rng.randint(1, 12), 1 << rng.randrange(4)) for u, v in sorted(pairs)]
    graph_path, shots_path = root / f'{name}.graph', root / f'{name}.shots'
    with graph_path.open('w') as output:
        output.write(f'{nodes} 4 {len(edges)}\n')
        for edge in edges:
            output.write(' '.join(map(str, edge)) + '\n')
    with shots_path.open('w') as output:
        for _ in range(200):
            shot = sorted(rng.sample(active_population, event_count))
            output.write(f'{len(shot)} ' + ' '.join(map(str, shot)) + '\n')
    manifest.append({'name': name, 'kind': 'graph', 'graph': str(graph_path), 'shots': str(shots_path),
                     'nodes': nodes, 'shot_count': 200, 'requested_events': event_count,
                     'independent_seed': 58073, 'description': description})


def ring(count, offset=0):
    return {tuple(sorted((offset + node, offset + (node + 1) % count))) for node in range(count)}


# Active vertices have substantially higher degree than the graph average.
pairs = ring(1024)
for hub in range(64):
    for other in rng.sample([node for node in range(1024) if node != hub], 96):
        pairs.add(tuple(sorted((hub, other))))
save('holdout_hubs_sparse', 1024, pairs, list(range(64)), 16,
     'Sixteen active hubs in a heterogeneous 1024-node graph; nonuniform degrees and edge weights.')

# Active seeds occupy a community, so global syndrome density underestimates
# the density of the neighborhood that actually enters predecoding.
pairs = ring(768)
while len(pairs) < 768 * 48 // 2:
    u = rng.randrange(768)
    v = rng.randrange((u // 192) * 192, (u // 192 + 1) * 192) if rng.random() < 0.85 else rng.randrange(768)
    if u != v:
        pairs.add(tuple(sorted((u, v))))
save('holdout_community_sparse', 768, pairs, list(range(192)), 32,
     'Syndrome concentrated in one of four densely connected communities.')

# Most graph vertices are irrelevant to this shot. A total-node density gate
# would misclassify the dense active component as sparse.
pairs = ring(128)
while len(pairs) < 128 * 48 // 2:
    u, v = rng.sample(range(128), 2)
    pairs.add(tuple(sorted((u, v))))
save('holdout_dense_component', 8192, pairs, list(range(128)), 96,
     'Dense 128-node active component plus 8064 disconnected inactive vertices.')
(root / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
