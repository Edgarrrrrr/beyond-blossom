# Final package audit (prepared; not started)

`run_audit.py` is inert unless `--execute` is supplied. No source snapshot has been taken and no build has started. The script captures an exact clean final HEAD only when run, so the pending generic optimization is included in the eventual artifact.

After root authorizes the final build and supplies its complete 40-character commit:

```sh
/home/fujy/dev/PyMatching/.conda-env/bin/python \
  /tmp/beyond-package-final/run_audit.py \
  --execute --commit FINAL_40_CHARACTER_HEAD_SHA --jobs 2
```

The default output `/tmp/beyond-package-final/artifacts` must not exist. Failed artifacts are retained; use a new `--output` for a second attempt.

The audit archives only the committed packaging/runtime inputs: CMake, cmake/, src/, python/, tests/, data/, licenses/, setup/pyproject/manifest, and public metadata/documents. It includes no AE datasets or benchmark tooling. Collector mock tests live beside their benchmark tooling and are tested separately.

The chain builds an sdist, verifies that every selected tracked input survived the manifest, extracts that sdist, and builds the wheel from the extracted directory. Local pybind11 and rustworkx wheels and pinned Stim/GoogleTest trees provide all dependencies; pip network access and CMake network fetching are disabled. No existing compiled Beyond Blossom artifact is reused.

The wheel is installed to a fresh target outside both checkouts. The audit asserts that package and native-extension imports resolve there, runs every packaged Python test, and checks console help, error status, missing-input handling and `.dets`/`.b8` prediction fixtures. Quickstart checks exercise the public Python StructuredDecoder and the direct native binding, including packed input/output and sparse/edge methods. Finally it installs the wheel build's CMake SDK and compiles a separate public C++ StructuredDecoder consumer against the installed target.

Own LICENSE/NOTICE and the Stim/pybind11 notices must be present byte-for-byte in the wheel. The native SDK must retain own LICENSE/NOTICE and Stim's license. CMake commands are inspected to reject unexpected LTO.

`audit.json` is written only on success. Source/dependency hashes, sdist/wheel hashes, compile commands, all command logs, pytest JUnit results, and installed native-extension hashes remain with the audit. `failure.json` preserves the stage and command history if anything fails.
