# Building

## Prerequisites

Install Visual Studio with:

- Desktop development with C++
- MSVC x64 build tools
- A current Windows SDK
- CMake tools for Windows
- Windows application development and MSIX packaging tools

Install or use a Visual Studio-bundled vcpkg. CMake 3.28+ and Ninja must be
available in the x64 Developer PowerShell.

## Configure and build

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Release:

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The first configure downloads/builds manifest dependencies. `vcpkg.json` pins a
registry baseline and FFmpeg feature set for repeatability. Its first build can
take a substantial amount of time. Do not edit installed dependency sources
under `vcpkg_installed`.

## WinUI application

Build the CMake Release tree first so the native bridge and runtime
dependencies exist, then:

```powershell
msbuild .\src\NativeShift.App\NativeShift.App.vcxproj /restore `
  /p:Configuration=Debug /p:Platform=x64
msbuild .\src\NativeShift.App\NativeShift.App.vcxproj /restore `
  /p:Configuration=Release /p:Platform=x64
```

NuGet versions are pinned in the project. The MSIX development publisher is
not a trusted production identity.

## AddressSanitizer

MSVC supports AddressSanitizer for x64 project code. The dedicated preset
removes Debug's incompatible `/RTC1` and incremental-link options:

```powershell
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

Run the sanitizer tests from the same x64 Developer PowerShell so the MSVC
AddressSanitizer runtime is present on `PATH`.

Do not combine AddressSanitizer with incompatible incremental-link or runtime
options.

## Troubleshooting

- If a preset says `VCPKG_ROOT` is missing, set it to the directory containing
  `vcpkg.exe` and `scripts/buildsystems/vcpkg.cmake`.
- Run from an x64 Developer PowerShell so `cl.exe`, the linker, SDK, CMake, and
  Ninja are initialized.
- If a dependency download fails transiently, rerun configure before changing
  project code.
- Remove only the specific `out/build/<preset>` directory when a clean
  reconfigure is necessary. `vcpkg_installed` is reusable.
