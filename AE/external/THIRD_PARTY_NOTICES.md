# Third-party source notices

Bundled third-party sources retain their own licenses. The repository's Apache-2.0 license does not replace the licenses below. `sources.lock.json` identifies the exact files used by the external benchmark builds.

| Component | Attribution and license | Preserved notice |
| --- | --- | --- |
| Fusion Blossom 0.2.13 | Copyright 2022 Yue Wu; MIT | [`sources/fusion-blossom/LICENSE`](sources/fusion-blossom/LICENSE) |
| UnionFind C++ | Chaeyeun Park and upstream contributors; GNU Lesser General Public License v2.1 | [`sources/unionfind/upstream/LICENSE`](sources/unionfind/upstream/LICENSE) |
| LDPC 2.4.1 | Copyright 2024 Joschka Roffe; MIT | [`sources/ldpc-union-find/upstream/LICENSE`](sources/ldpc-union-find/upstream/LICENSE) |
| robin-map, both bundled copies | Copyright 2017 Thibaut Goetghebuer-Planchon; MIT | Full notices in the [`UnionFind headers`](sources/unionfind/upstream/UnionFindPy/cpp/externals/robin-map/include/tsl/) and [`LDPC headers`](sources/ldpc-union-find/upstream/include/robin_map/) |
| nlohmann JSON 3.10.4 | Copyright 2013–2019 Niels Lohmann; MIT | Full notice in [`json.hpp`](sources/unionfind/deps/nlohmann/json.hpp) |
| Bottle embedded by Fusion Blossom | Copyright 2009–2024 Marcel Hellkamp; MIT | Full notice in [`bottle.py`](sources/fusion-blossom/src/bottle.py) |

The UnionFind files in `patched_include/` are modified copies of the LGPL upstream headers. Their upstream originals, the full license, and the exact changes in [`patches/unionfind.patch`](patches/unionfind.patch) are included. These recovered changes add boundary handling and peeling rules and replace the endpoint hash; they are not unmodified upstream releases. The LDPC diagnostic header is a modified MIT upstream header; its exact changes are in [`patches/ldpc-uf.patch`](patches/ldpc-uf.patch).

The native benchmark adapters, `boundary_lattice.hpp`, and `uf_decode_core.hpp` were recovered from the original experiment source. Their file identities and relationship to the upstream implementations are recorded in the lock file. No upstream copyright notice was removed. The build-time UF portability adaptation makes an explicit checked index conversion outside the timer and records its generated source separately.

Blossom V's separate native implementation is not bundled or enabled by these builds. Optional Fusion Blossom visualization and Python helper source files retain their original notices but are not enabled in the native adapter's feature set. Cargo dependencies are fetched or supplied as separate checksum-verified archives under their own package licenses; they are not redistributed as source in this repository.

The independent optimality oracle links the separately built official PyMatching source, whose Apache-2.0 license is preserved by `AE/build_reference.py`. Its Stim dependency is also supplied through that separate reference build. Neither dependency's source nor its binary is copied into this source directory.
