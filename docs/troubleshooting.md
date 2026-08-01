# Troubleshooting

## CMake cannot find vcpkg

Set `VCPKG_ROOT` to a vcpkg checkout containing
`scripts/buildsystems/vcpkg.cmake`, then start from an x64 Developer
PowerShell.

## FFmpeg takes a long time to build

The first configure compiles the pinned FFmpeg feature set and its codecs.
NativeShift deliberately limits vcpkg concurrency for reliable Windows builds.
Subsequent builds reuse the binary cache.

## A hardware encoder is listed but unavailable

Registration means the codec was compiled in; availability also depends on the
GPU, driver, and the ability to create a hardware device. Use `--list-hardware`
for runtime diagnostics. `--hardware auto` falls back to software with a
warning.

## A conversion leaves no output

NativeShift commits only a fully encoded temporary output. Check the selected
conflict policy, available disk space, local structured log, and destination
permissions. Cancellation and errors intentionally remove partial output.

## The MSIX will not install

The repository uses a development publisher placeholder and does not contain a
trusted certificate. Use a correctly subject-matched trusted development
certificate for package tests or a protected production certificate for
release. For normal use, download the portable ZIP, extract it, and launch
`NativeShift.exe`; it does not require MSIX installation.

## WinUI cannot locate the bridge DLL

Build the CMake Release preset before the application project and make sure the
post-build copy includes `NativeShift.GuiBridge.dll` and all dynamic vcpkg
runtime DLLs next to the executable.
