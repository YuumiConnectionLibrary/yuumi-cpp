#!/usr/bin/env bash
set -euo pipefail

repository="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
vcpkg_root="$repository/vcpkg"
vcpkg_commit="d015e31e90838a4c9dfa3eed45979bc70d9357fc"

if [[ ! -d "$vcpkg_root/.git" ]]; then
    mkdir -p "$vcpkg_root"
    git -C "$vcpkg_root" init
    git -C "$vcpkg_root" remote add origin https://github.com/microsoft/vcpkg.git
    git -C "$vcpkg_root" fetch --depth 1 origin "$vcpkg_commit"
    git -C "$vcpkg_root" checkout --detach FETCH_HEAD
fi

actual_commit="$(git -C "$vcpkg_root" rev-parse HEAD)"
if [[ "$actual_commit" != "$vcpkg_commit" ]]; then
    printf ''vcpkg checkout mismatch: expected %s, found %s\n'' "$vcpkg_commit" "$actual_commit" >&2
    exit 1
fi

"$vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics
cd "$repository"
cmake --preset wsl-clang-debug
cmake --build --preset wsl-clang-debug-build
ctest --preset wsl-clang-debug-test --output-on-failure
