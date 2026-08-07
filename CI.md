# Continuous integration

Workflow: `.github/workflows/ci.yml`.

- Triggers: pushes and pull requests for `main` and `dev`, plus manual dispatch.
- Platforms: `windows-latest`, `ubuntu-latest`, and `macos-latest`.
- Toolchains: the runner's stable MSVC, GCC, or Apple Clang, latest stable Go
  and Python 3,
  and vcpkg commit `d015e31e90838a4c9dfa3eed45979bc70d9357fc`.
- Gates: C++23 build, 25 engine cases, real Go interop, example build,
  ASan+UBSan, and TSan.
- Spec pin: `45729f1075ec5afcd9fd811db944385d6672eec3`; changing it requires an explicit
  reviewed workflow edit.
- Cache: disabled until run timings demonstrate that vcpkg caching is beneficial.
- Timeout: 10 minutes for spec validation and 60 minutes for build or sanitizer jobs.
- Artifacts: per-case report, essential logs, sanitizer logs, cleanup diagnostics,
  toolchain versions, and repository commits, retained for 14 days.

After configuring a build with `YUUMI_BUILD_INTEROP_TESTS=ON`, the local gate is:

```powershell
cmake --build yuumi-cpp/build/ci --config Debug
python yuumi-spec/conformance/harness.py --repositories-root . --spec-sha 45729f1075ec5afcd9fd811db944385d6672eec3 --platform windows --sdk cpp --stage all --cpp-build-dir yuumi-cpp/build/ci --cpp-configuration Debug --output-dir artifacts/cpp
```

Sanitizer jobs execute every C++ conformance and interop case. Cleanup runs in
the harness even after timeout and records whether its owned process group exited.
