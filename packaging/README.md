# Packaging

`scripts/build-packages.ps1` consumes an already verified Release build, builds
the WinUI x64 MSIX without requiring signing, stages portable and CLI bundles,
collects runtime dependencies and license texts, generates an SPDX JSON SBOM,
release manifest, symbols archive, and SHA-256 checksums.

`scripts/verify-packages.ps1` extracts every ZIP, inspects names and content,
rejects debug runtimes, fixtures, source files, secrets, and missing notices,
checks PE x64 architecture and CLI version, validates JSON, and verifies every
checksum.

MSIX signing is intentionally external to the ordinary build. Supply a valid,
protected certificate only to the trusted release workflow. Never place
certificates or passwords in this repository.
