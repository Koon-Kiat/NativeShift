# Licensing and codecs

NativeShift application source is MIT licensed. That does not change the
licenses, patent exposure, or redistribution obligations of its dependencies.

The pinned FFmpeg 8.1.2 vcpkg build is dynamically linked and disables default
features. Enabled features are `amf`, `aom`, `avcodec`, `avformat`, `mp3lame`,
`nvcodec`, `openh264`, `opus`, `qsv`, `swresample`, `swscale`, `vorbis`, and
`vpx`. GPL-only `libx264` and `libx265` and nonfree mode are not enabled. This
is intended to retain FFmpeg's LGPL configuration, including the applicable
notices, license text, source/relinking obligations, and prohibition on
restricting reverse engineering for debugging modifications.

H.264, H.265/HEVC, AAC, MP3, AV1, and other codecs can be covered by patents in
some jurisdictions even when their software implementation has a permissive or
LGPL-compatible copyright license. Hardware vendor SDKs and OpenH264 binaries
can add separate terms. Obtain legal advice before public binary distribution.

Images use libpng, libjpeg-turbo, libwebp, and Windows Imaging Component.
libvips is not currently shipped. GoogleTest is test-only. Windows App SDK,
C++/WinRT, WIL, resolved NuGet components, optional future LibreOffice
providers, and future archive libraries must retain their own terms and
notices.

Packaging inventories the resolved components, copies available license texts,
and emits an SPDX JSON SBOM. Automated checks are evidence, not legal
clearance. See `THIRD_PARTY_NOTICES.md` for the current inventory.
