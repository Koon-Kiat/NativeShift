# Packaging

`scripts/build-packages.ps1` consumes an already verified Release build and
stages the self-contained portable WinUI application and CLI bundles. It also
collects runtime dependencies and license texts and generates an SPDX JSON SBOM,
release manifest, symbols archive, and SHA-256 checksums. `-IncludeMsix` adds the
unsigned development MSIX when package testing is explicitly required.

`scripts/verify-packages.ps1` extracts every ZIP, inspects names and content,
rejects debug runtimes, fixtures, source files, secrets, and missing notices,
checks PE x64 architecture and CLI version, validates JSON, and verifies every
checksum.

The portable EXE/ZIP is the primary distribution. MSIX signing is intentionally
external to the ordinary build. Supply a valid, protected certificate only to
the trusted release workflow. Never place certificates or passwords in this
repository.
