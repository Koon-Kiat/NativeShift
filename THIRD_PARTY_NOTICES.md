# Third-party notices

NativeShift source is licensed under the MIT License. Binary distributions
include dynamically linked third-party components resolved from the pinned
vcpkg baseline in `vcpkg.json`; the precise package versions and hashes are
captured in the release SBOM and manifest.

| Component | Use | License family |
|---|---|---|
| FFmpeg 8.1.2 | Media demux, decode, encode, remux, resample, scale | LGPL 2.1+ for this configuration |
| libaom | AV1 | BSD-2-Clause |
| libjpeg-turbo | JPEG | IJG/BSD/zlib |
| libmp3lame | MP3 encoding | LGPL 2.0+ |
| libogg/libvorbis | Ogg/Vorbis | BSD-3-Clause |
| libopenh264 | H.264 software encoding | BSD-2-Clause; Cisco binary/patent terms may also apply |
| libopus | Opus | BSD-3-Clause |
| libpng | PNG | libpng-2.0 |
| libvpx | VP8/VP9 | BSD-3-Clause |
| libwebp | WebP | BSD-3-Clause |
| nlohmann/json | JSON | MIT |
| GoogleTest | Tests only | BSD-3-Clause |
| Microsoft Windows App SDK | WinUI application | Microsoft Software License Terms |
| Microsoft C++/WinRT | WinRT projection tooling | MIT |
| Windows Implementation Library | Windows helpers | MIT |

The FFmpeg feature set intentionally omits `libx264`, `libx265`, and all
`--enable-gpl` or `--enable-nonfree` options. It enables avcodec, avformat,
swresample, swscale, AMF, libaom, libmp3lame, NVIDIA codecs, OpenH264, Opus,
QSV, Vorbis, and libvpx as recorded in `vcpkg.json`.

Copyright notices and full license texts for redistributed dependencies must
be generated from the resolved package metadata and included under `LICENSES/`
by the packaging script. Codec patents and platform redistribution rights are
separate from copyright licenses; distributors remain responsible for their
jurisdiction and use case. This notice is an engineering inventory, not legal
advice.
