# NativeShift

NativeShift is a privacy-focused native Windows conversion engine.
All file inspection and conversion happens in-process on the local computer.
There are no uploads, accounts, advertisements, telemetry, or shell-based codec
commands.

The naming and compatibility decision is recorded in
[docs/naming.md](docs/naming.md).

This repository currently contains **Phase 1**: a C++23 conversion engine,
bounded job queue, native image provider, command-line interface, and tests. The
WinUI 3 application is intentionally deferred until the engine is proven.

## Current support

| Input | Output | Provider |
|---|---|---|
| PNG | PNG, JPEG, WebP | libpng / libjpeg-turbo / libwebp |
| JPEG | PNG, JPEG, WebP | libpng / libjpeg-turbo / libwebp |
| WebP | PNG, JPEG, WebP | libpng / libjpeg-turbo / libwebp |

Phase 1 supports JPEG/WebP quality, optional width and height, aspect-ratio
preservation, conflict policies, cancellation, batch folders, recursive
folders, configurable concurrency, Unicode paths, JSON output, and transactional
temporary outputs.

Metadata is removed. `--preserve-metadata` is accepted as a forward-compatible
option, but Phase 1 emits a warning because preservation is not implemented yet.

## Not yet supported

- The WinUI 3 interface
- BMP/TIFF conversion (their signatures are detected only)
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
.\out\build\debug\src\NativeShift.Cli\nativeshift-cli.exe input.png --to webp --output output.webp
.\out\build\debug\src\NativeShift.Cli\nativeshift-cli.exe input.jpg --to png --width 1920
.\out\build\debug\src\NativeShift.Cli\nativeshift-cli.exe .\input-folder --to jpeg --output .\converted --recursive --jobs 4
.\out\build\debug\src\NativeShift.Cli\nativeshift-cli.exe input.png --to webp --json
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
distributor every obligation needed for a shipped product. Phase 1 uses libpng,
libjpeg-turbo, and libwebp; each binary distribution must include the notices
required by the exact versions in `vcpkg_installed`.

libvips is not in the official vcpkg catalog at the pinned baseline, so Phase 1
uses the individual maintained codecs. A future libvips provider must account
for libvips' LGPL terms and every enabled transitive codec. FFmpeg licensing is
configuration-dependent: enabling GPL codecs changes the resulting binary's
license obligations, and nonfree configurations are not redistributable under
the normal FFmpeg terms. Audit the actual build configuration and obtain legal
review before release.

This repository does not yet declare a license for the application source.
Choose and add one before accepting external contributions or distributing
source/binaries.
