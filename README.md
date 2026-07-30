# NativeShift

NativeShift is a privacy-focused native Windows file converter. The WinUI 3
application and automation-friendly CLI process files locally: no uploads,
accounts, advertisements, telemetry, or shell-built codec commands.

> Project status: release candidate. Image conversion is complete; native
> FFmpeg audio conversion and video remux/software encoding are implemented.
> Production MSIX signing and hardware-specific validation require the release
> environment described below.

![NativeShift logo](src/NativeShift.App/Assets/NativeShiftLogoMaster.png)

## Supported conversion

| Kind | Inputs | Outputs |
|---|---|---|
| Image | PNG, JPEG, WebP, BMP, TIFF | PNG, JPEG, WebP, BMP, TIFF |
| Audio | MP3, WAV, FLAC, AAC, M4A, OGG, Opus | MP3, WAV, FLAC, AAC, M4A, OGG, Opus |
| Video | MP4, MKV, MOV, AVI, WebM | MP4, MKV, WebM |

Images support quality/lossless controls, resize modes, EXIF orientation,
rotation, alpha compositing, TIFF compression, conflict policies, and 25
pairwise routes. Audio supports bitrate/VBR, sample rate, channels, metadata
policy, and stream copy. Video supports remuxing, stream selection,
metadata/subtitles, resolution, frame rate, quality/bitrate, H.264,
H.265/HEVC, VP9, AV1, and runtime-probed NVENC/QSV/AMF/Media Foundation
candidates with visible software fallback.

The selected codec must be compatible with its container and available in the
resolved FFmpeg build. NativeShift does not support animated/multi-page image
output, document or archive conversion, DRM bypass, or password cracking.

## GUI

The accessible WinUI queue supports drag-and-drop, file and recursive-folder
pickers, search/type filters, presets, detailed format settings, bounded
parallel work, per-job progress/details, pause/resume/cancel/retry/remove,
conflict policies, capabilities, settings, logs, and diagnostics. State is
never conveyed by color alone.

## Build and test

Requirements are Windows 10/11 x64, Visual Studio with Desktop C++ and a
current Windows SDK, CMake 3.28+, Ninja, and vcpkg.

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset release
cmake --build --preset release
ctest --preset release
```

Build the WinUI project after the CMake Release build:

```powershell
msbuild .\src\NativeShift.App\NativeShift.App.vcxproj /restore `
  /p:Configuration=Release /p:Platform=x64
```

See [building](docs/building.md) and [troubleshooting](docs/troubleshooting.md).

## CLI

```powershell
nativeshift-cli input.png --to webp --quality 82
nativeshift-cli input.wav --to mp3 --audio-bitrate 320k
nativeshift-cli input.mkv --to mp4 --video-codec h264 --hardware auto
nativeshift-cli .\input --to jpeg --output .\converted --recursive
nativeshift-cli --list-formats --json
```

JSON mode reserves standard output for stable JSON. See the
[CLI reference](docs/cli.md) for presets, conflict policies, and exit codes.

## Packages

```powershell
.\scripts\build-packages.ps1 -Version 0.1.0
.\scripts\verify-packages.ps1 -Version 0.1.0
```

The pipeline creates an x64 MSIX, portable GUI ZIP, CLI ZIP, symbols ZIP, SPDX
JSON SBOM, release manifest, license bundle, and SHA-256 checksums. The checked
in publisher is a placeholder; a protected subject-matched certificate is
required for a trusted release. See [distribution](docs/distribution.md).

## Privacy and security

NativeShift detects content signatures, bounds decoded dimensions and queue
resources, validates output paths, handles existing files explicitly, writes
transactionally beside the destination, cleans incomplete output, and keeps
privacy-filtered rotating local logs. Native codecs still parse untrusted data,
so dependencies must remain patched and release security gates must pass.

- [Architecture](docs/architecture.md)
- [User guide](docs/user-guide.md)
- [Configuration](docs/configuration.md)
- [Security controls](docs/security.md)
- [Threat model](docs/threat-model.md)
- [Performance](docs/performance.md)
- [Licensing and codecs](docs/licensing.md)
- [Release process](docs/releasing.md)

NativeShift is MIT licensed. Third-party and codec terms remain independent;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
