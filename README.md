# NativeShift

NativeShift is a privacy-focused native Windows conversion engine.
All file inspection and conversion happens in-process on the local computer.
There are no uploads, accounts, advertisements, telemetry, or shell-based codec
commands.

The repository is under active development. The shared C++23 engine, bounded
job queue, complete still-image provider, command-line interface, and tests are
implemented. Media and WinUI components are being added in subsequent commits.

## Current support

| Input | Output | Provider |
|---|---|---|
| PNG | PNG, JPEG, WebP, BMP, TIFF | libpng / libjpeg-turbo / libwebp / WIC |
| JPEG | PNG, JPEG, WebP, BMP, TIFF | libpng / libjpeg-turbo / libwebp / WIC |
| WebP | PNG, JPEG, WebP, BMP, TIFF | libpng / libjpeg-turbo / libwebp / WIC |
| BMP | PNG, JPEG, WebP, BMP, TIFF | Windows Imaging Component |
| TIFF | PNG, JPEG, WebP, BMP, TIFF | Windows Imaging Component |

Image conversion supports JPEG/WebP quality, lossless WebP, optional width and
height, fit/fill/stretch, enlargement prevention, EXIF orientation, rotation,
alpha compositing, TIFF compression selection, conflict policies,
cancellation, batch folders, recursive folders, configurable concurrency,
Unicode paths, JSON output, and transactional temporary outputs.

Metadata is removed. `--preserve-metadata` and
`--preserve-color-profile` emit explicit warnings when the selected native
conversion path cannot honour them.

## Not yet supported

- The WinUI 3 interface
- Audio and video conversion or FFmpeg
- Hardware acceleration
- Office document conversion
- Animated or multi-page images
- EXIF orientation and metadata preservation

These capabilities belong to later providers and do not require changes to the
core scheduler or CLI architecture.

## Required tools

- Windows 11 or a supported Windows 10 release
- Visual Studio with the MSVC x64 C++ workload and a current Windows SDK
- CMake 3.28 or newer
- Ninja
- vcpkg

Visual Studio includes CMake, Ninja, and vcpkg when the corresponding components
are selected. The presets read the vcpkg root from `VCPKG_ROOT`.

## Build

Run these commands from an x64 Developer PowerShell:

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset debug
cmake --build --preset debug
```

For an optimized build:

```powershell
cmake --preset release
cmake --build --preset release
```

See [docs/building.md](docs/building.md) for complete setup, AddressSanitizer,
and troubleshooting instructions.

## Test

```powershell
ctest --preset debug
```

Fixtures are generated during tests; the repository does not contain large
media files.

## CLI

```powershell
.\out\build\nativeshift-debug\src\NativeShift.Cli\nativeshift-cli.exe input.png --to webp --output output.webp
.\out\build\nativeshift-debug\src\NativeShift.Cli\nativeshift-cli.exe input.jpg --to png --width 1920
.\out\build\nativeshift-debug\src\NativeShift.Cli\nativeshift-cli.exe .\input-folder --to jpeg --output .\converted --recursive --jobs 4
.\out\build\nativeshift-debug\src\NativeShift.Cli\nativeshift-cli.exe input.png --to webp --json
```

Run `nativeshift-cli --help` for all options. Exit codes are `0` for success,
`2` for command-line usage errors, `3` for invalid/unsupported input, `4` for
conversion or I/O failures, and `130` for cancellation.

## Design and security

- [Architecture](docs/architecture.md)
- [Adding a converter](docs/adding-a-converter.md)
- [Security model](docs/security.md)
- [Performance](docs/performance.md)

The engine identifies files from their content rather than trusting extensions.
It constrains decoded dimensions and file sizes, never builds shell commands,
writes to a same-directory temporary file, and commits only after successful
encoding.

## Codec licensing and distribution

Dependency installation for development does not automatically grant a
distributor every obligation needed for a shipped product. The image provider
uses libpng, libjpeg-turbo, libwebp, and Windows Imaging Component; each binary
distribution must include the notices required by the exact versions in
`vcpkg_installed`.

libvips is not in the official vcpkg catalog at the pinned baseline, so
NativeShift uses individual maintained codecs and Windows Imaging Component. A
future libvips provider must account
for libvips' LGPL terms and every enabled transitive codec. FFmpeg licensing is
configuration-dependent: enabling GPL codecs changes the resulting binary's
license obligations, and nonfree configurations are not redistributable under
the normal FFmpeg terms. Audit the actual build configuration and obtain legal
review before release.

This repository does not yet declare a license for the application source.
Choose and add one before accepting external contributions or distributing
source/binaries.
