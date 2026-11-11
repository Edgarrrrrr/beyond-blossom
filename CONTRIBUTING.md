# Contributing

Use a topic branch and keep behavior changes separate from measurement results. Include the triggering input and expected behavior in bug reports. Pull requests should describe the change and the checks actually run.

## Development

```sh
python -m pip install -e '.[test]'
python -m pytest tests
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBB_BUILD_PYTHON=OFF
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

C++ uses C++20 and the repository `.clang-format`. Public APIs must validate dimensions, geometry, and strategy constraints. New optimizations require differential tests against exact matching weights, including negative weights, boundaries, repeated calls, and degenerate matchings where applicable. Observable predictions may differ between equally weighted valid matchings.

Structured and unstructured storage remain separate. Share value types and proven algorithms through `core`; select layouts, kernels, and policies before entering a hot loop. Header implementations must be safe in multiple translation units.

Performance changes require paired measurements on the same machine, compiler settings, fixed inputs, and CPU mapping. Record source and binary hashes, absolute latency, repeat count, and regressions. Keep diagnostic instrumentation out of timing runs. AE documents the collection contract.

Retain original copyright and license notices. New source contributions are distributed under Apache-2.0.
