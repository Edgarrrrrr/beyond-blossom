# Beyond Blossom

Beyond Blossom is a minimum-weight perfect-matching decoder for quantum error correction. It provides an arbitrary-graph decoder and a structured surface-code decoder with SIMD, certified predecoding, and persistent OpenMP teams.

The project is based on PyMatching, with redesigned backends and additional optimizations. The familiar `Matching` Python API is available under `beyond_blossom`. Original copyright notices and the Apache-2.0 license are retained.

## Installation

A C++20 compiler, CMake 3.18+, Python 3.10+, and OpenMP are required. On Debian/Ubuntu, install `g++`, `cmake`, `python3-dev`, and `libgomp1`. On macOS, install CMake and LLVM OpenMP and pass the OpenMP prefix to CMake as needed.

```sh
python -m pip install .
```

The build downloads a pinned Stim revision. `CMAKE_ARGS` accepts ordinary CMake arguments, including `-DFETCHCONTENT_SOURCE_DIR_STIM=/path/to/stim` for offline builds. Wheels build the native extension without requiring LTO.

```python
import beyond_blossom as bb

matching = bb.Matching()
matching.add_edge(0, 1, fault_ids=0, weight=1.0)
matching.add_boundary_edge(0, fault_ids=1, weight=2.0)
print(matching.decode([1, 1]))
```

`Matching` accepts check matrices, NetworkX graphs, and Stim detector error models. A decoder instance owns mutable state; use a separate instance for each concurrent caller.

The installed Python console command is `beyond-blossom`; run `beyond-blossom --help`
for decoding options. The native CMake build provides `beyond_blossom` with the
same command interface.

## Structured decoding

```python
import stim
from beyond_blossom import StructuredDecoder

circuit = stim.Circuit.generated(
    "surface_code:rotated_memory_x", distance=5, rounds=5,
    after_clifford_depolarization=0.001,
)
model = circuit.detector_error_model(decompose_errors=True)
decoder = StructuredDecoder.from_detector_error_model(model, distance=5, rounds=5)
shots = circuit.compile_detector_sampler(seed=17).sample(shots=100)
predictions, weights = decoder.decode_batch(shots, return_weights=True)
print(decoder.metadata)
```

Geometry is explicit and validated against the detector coordinates. Strategies
are `structured`, `predecoded`, `component-parallel`, `pipelined`, and
`parallel-predecode`. Parallel strategies require an explicit `cpus` list of
2, 4, or 8 available CPU IDs respectively. On macOS these identify logical
slots; the runtime requests QoS rather than physical core affinity.
`decode_batch` reuses one worker team across the complete batch.

The C++ entry point is `beyond_blossom::structured::Decoder` in
`beyond_blossom/structured/decoder.h`. `Matching` and the unstructured native
driver support arbitrary graphs and correlated decoding.

## Native build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBB_BUILD_PYTHON=OFF
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Native consumers can link `BeyondBlossom::decoder` after `add_subdirectory`,
or install the library and use its CMake package:

```sh
cmake --install build --prefix /path/to/install
```

```cmake
find_package(BeyondBlossom CONFIG REQUIRED)
target_link_libraries(my_decoder PRIVATE BeyondBlossom::decoder)
```

`BB_PREDECODE_SIMD=ON` enables construction-time selection of the supported SIMD kernels. The portable executable does not require AVX2 for every function. `BB_SANITIZERS=ON` enables ASan and UBSan. `BB_BUILD_PROFILE_TOOLS=ON` builds separate diagnostic tools; their instrumentation is excluded from the normal decoder.

## Source layout

| Directory | Responsibility |
| --- | --- |
| `src/beyond_blossom/core` | Shared result types, observable helpers, queues, and geometry |
| `src/beyond_blossom/unstructured` | Arbitrary-graph flooding, primal matching, and search |
| `src/beyond_blossom/structured` | Fixed-stencil storage, SIMD kernels, predecoding, and parallel execution |
| `src/beyond_blossom/driver` | Graph construction and construction-bound decoder strategies |
| `src/beyond_blossom/bindings` | Native Python interfaces |
| `python/beyond_blossom` | Python API |
| `tests` | Public API and differential tests |
| `AE` | Paper data, collection commands, verification, and plotting |

Structured primal hot paths are inline definitions shared by callers. Backend selection occurs during decoder construction. Arbitrary graphs remain supported by the unstructured backend.

## Reproduction and contributions

[AE/README.md](AE/README.md) describes smoke runs, full collection, and replay of archived paper data. Archived measurements describe their original binaries; a new build requires new measurements before a performance claim is made.

[Migration validation, 2026-11-12](benchmarks/regression/results/20261112/README.md)
records this migration's correctness checks, paired measurements, and artifacts.

See [CONTRIBUTING.md](CONTRIBUTING.md) for development and [SECURITY.md](SECURITY.md) for reporting vulnerabilities. The project uses the [Apache License 2.0](LICENSE); attribution is recorded in [NOTICE](NOTICE).
