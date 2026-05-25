# yuumi-cpp

[![CI](https://github.com/ilmartotch/yuumi-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/ilmartotch/yuumi-cpp/actions/workflows/ci.yml)

> C++23 header-only implementation of the [Yuumi IPC protocol](https://github.com/ilmartotch/yuumi-spec).

## Requirements

- CMake 3.28+
- C++23 compiler (MSVC 19.38+, GCC 13+, Clang 17+)
- [vcpkg](https://github.com/microsoft/vcpkg)

## Quick start

```bash
# Clone with vcpkg as submodule
git clone --recurse-submodules https://github.com/ilmartotch/yuumi-cpp

# Build (choose preset for your platform)
cmake --preset linux-clang-debug
cmake --build --preset linux-clang-debug-build

cmake --preset macos-clang-debug
cmake --build --preset macos-clang-debug-build

cmake --preset windows-msvc-debug        # PowerShell
cmake --build --preset windows-msvc-debug-build
```

If `VCPKG_ROOT` is set in your environment, CMake uses it automatically.

## Wire protocol

See [yuumi-spec](https://github.com/ilmartotch/yuumi-spec) for the canonical wire format definition.

## Dependencies (via vcpkg)

- [asio](https://github.com/chriskohlhoff/asio) — async I/O
- [nlohmann-json](https://github.com/nlohmann/json) — JSON encoding
