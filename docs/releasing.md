# Release process

NativeShift uses Semantic Versioning.

1. Update the CMake, vcpkg, MSIX, and changelog versions together.
2. Confirm dependency versions, FFmpeg configuration, notices, and any
   vulnerability exceptions.
3. Configure and build clean Debug and Release trees.
4. Run CTest, CLI smokes, formatting, static analysis, sanitizer tests where
   supported, and the core benchmark.
5. Build WinUI and its MSIX, then build and verify every package candidate.
6. Inspect the SPDX SBOM, release manifest, checksums, and provenance inputs.
7. Test installation, startup, conversion, and uninstall on a clean Windows
   machine. Confirm user output is retained.
8. Sign only in the protected release environment when a valid publisher
   certificate and timestamp service are configured.
9. Push the version tag and dispatch the release workflow with the exact tag.
10. Review the draft GitHub Release and its assets before publication.

The trusted workflow refuses a tag/version mismatch and does not manufacture a
certificate. Rollback means marking the affected release as a draft or
prerelease, documenting the reason, fixing forward with a new patch version,
and retaining evidence for already downloaded artifacts.
