# Distribution

NativeShift produces deterministic x64 candidates. The portable GUI is the
primary user-facing package:

- `NativeShift-<version>-portable-windows-x64.zip`
- `NativeShift-CLI-<version>-windows-x64.zip`
- `NativeShift-Symbols-<version>-windows-x64.zip`
- `NativeShift-<version>-sbom.spdx.json`
- `NativeShift-<version>-release-manifest.json`
- `SHA256SUMS`

An unsigned `NativeShift-<version>-windows-x64.msix` is optional and intended
only for package-development tests.

Build and verify them from an x64 Developer PowerShell:

```powershell
.\scripts\build-packages.ps1 -Version 0.1.0
.\scripts\verify-packages.ps1 -Version 0.1.0
```

To additionally build and verify the development MSIX, pass `-IncludeMsix` to
both commands.

The checked-in MSIX identity uses a development publisher placeholder. Windows
will not trust that package as a production installer. Production publishing
requires a protected certificate whose
subject matches the manifest publisher, timestamping, and signing performed
only in the trusted release workflow.

For local testing, create and trust a dedicated development certificate, pass
it directly to MSBuild, and never commit the PFX or password. Uninstalling the
application removes the installed package, not user-generated conversions.
User settings remain governed by Windows package data behavior.

The portable bundle is unpackaged and receives no MSIX identity or automatic
servicing. It is self-contained: extract the ZIP and run `NativeShift.exe` while
retaining its adjacent dependencies and license bundle.
