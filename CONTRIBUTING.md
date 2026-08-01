# Contributing to NativeShift

Use a focused branch, keep commits imperative and capitalized, and open a pull
request against the default branch. Do not commit source media, credentials,
build trees, caches, logs, certificates, or generated conversion output.

## Development gate

From an x64 Developer PowerShell:

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Run `clang-format --dry-run --Werror` over project-owned C++ files before
submitting. New behavior needs deterministic tests that create tiny fixtures at
runtime. Hardware-only behavior must retain a mandatory software fallback.

Report security issues privately as described in [SECURITY.md](SECURITY.md).
Contributions are accepted under the repository's MIT license; dependencies
and codec changes must also update `THIRD_PARTY_NOTICES.md` and
`docs/codec-obligations.md`.
