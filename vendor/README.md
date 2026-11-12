# Offline build sources

This directory contains the fixed project dependencies needed by a clean clone. Standard build tools and Python packages must be installed separately; no precompiled decoder libraries or compiler toolchains are bundled.

| Component | Identity | Use |
| --- | --- | --- |
| `core/stim.tar.gz` | Stim `1320ad7eac7de34d2e9c70daa44fbc6d84174450` | Decoder library and saved-input generation |
| `core/googletest.tar.gz` | GoogleTest `58d77fa8070e8cec2dc1ed015d66b454c8d78850` | Native tests and fixed reference configuration |
| `ae/official.tar.gz` | PyMatching 2.3.1, `e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd` | Independent official reference |
| `ae/reference-adapter.tar.gz` | Original source241 native acquisition files | Reference adapter, adapted for the host during build |
| `ae/migration-source.tar.gz` | Buildable subset of source `241160314087e5ae698c6e76c3c055a11c3ad422` | Paired migration control, distinct from the official reference |
| `ae/crates.tar.gz` | Exactly the 107 registry packages in the external adapter's Cargo.lock | Offline native Rust external decoders |

[manifest.json](manifest.json) records every archive SHA-256, every unpacked file SHA-256, source identity evidence, and retained license paths. Stim and GoogleTest contain their required source/build files, verified against the original snapshot's per-file hashes and Git blob identities. The official reference is a complete fixed-revision Git archive. The migration control is explicitly a subset of the recovered original source, with its original adapter and required headers; it is not presented as a complete original commit archive.

## Native and Python builds

CMake uses the local Stim and GoogleTest archives by default and verifies their hashes before extraction. Offline mode is enabled by default:

```sh
cmake -S . -B build -DBB_OFFLINE=ON -DBB_BUILD_PYTHON=OFF
cmake --build build --parallel 2
```

An explicit `FETCHCONTENT_SOURCE_DIR_STIM` or `FETCHCONTENT_SOURCE_DIR_GOOGLETEST` can supply an already extracted local source. `BB_OFFLINE=OFF` permits the pinned Git fallback only when the bundled archive is missing. Python package builds also use the bundled sources; installing Python build requirements such as pybind11 remains an environment preparation step. Source distributions include the two `core/` archives; the complete AE archive set is available in the repository.

## Artifact evaluation

The following operation performs no network access or compilation:

```sh
python AE/vendor.py --output /absolute/work/deps
```

It extracts and checks all six components, then writes `dependencies.json` with `stim_source`, `googletest_source`, `official_source`, `reference_adapter`, `crate_cache`, and `migration_source` paths. `--only` selects individual components. Reusing an extraction checks its entire file set and hashes; changed files are rejected.

The fixed reference can be built without another repository or an installed Stim library:

```sh
python AE/build_reference.py --offline --output /absolute/work/reference --jobs 2
python AE/build_native.py --offline --profile paper --output /absolute/work/candidate --jobs 2
```

The reference builder first builds the bundled Stim source if no explicit archive is supplied. Its receipt records both source and built-library identities. External builds use that receipt to locate the same Stim files and use the bundled Cargo crates by default:

```sh
python AE/external/build.py --offline --output /absolute/work/external \
  --reference-build /absolute/work/reference --toolchain INSTALLED_RUST_TOOLCHAIN --jobs 2
```

The Rust toolchain must already be installed and identified explicitly. Cargo remains locked to the supplied dependency versions and checks every original `.crate` archive against Cargo.lock before creating its isolated directory source. The source trees and original license notices for Fusion Blossom, UnionFind, LDPC, and their C++ headers are already in [AE/external/sources](../AE/external/sources), with attribution in [THIRD_PARTY_NOTICES.md](../AE/external/THIRD_PARTY_NOTICES.md).

## Licenses

Stim, PyMatching, and the migration/control sources retain Apache-2.0 notices. GoogleTest retains its BSD-3-Clause license. The Rust packages retain their individual upstream licenses and notices inside the original crate archives; `origin.packages` in the manifest lists each package's declared license and preserved notice files. No dependency is relicensed by this packaging step.
