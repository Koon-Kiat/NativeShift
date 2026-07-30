# Distribution

NativeShift produces deterministic x64 candidates:

- `NativeShift-<version>-windows-x64.msix`
- `NativeShift-<version>-portable-windows-x64.zip`
- `NativeShift-CLI-<version>-windows-x64.zip`
- `NativeShift-Symbols-<version>-windows-x64.zip`
- `NativeShift-<version>-sbom.spdx.json`
- `NativeShift-<version>-release-manifest.json`
- `SHA256SUMS`

Build and verify them from an x64 Developer PowerShell:

```powershell
.\scripts\build-packages.ps1 -Version 0.1.0
.\scripts\verify-packages.ps1 -Version 0.1.0
```

The checked-in MSIX identity uses a development publisher placeholder. CI may
validate an unsigned package, but Windows will not trust it as a production
installer. Production publishing requires a protected certificate whose
subject matches the manifest publisher, timestamping, and signing performed
only in the trusted release workflow.

For local testing, create and trust a dedicated development certificate, pass
it directly to MSBuild, and never commit the PFX or password. Uninstalling the
application removes the installed package, not user-generated conversions.
User settings remain governed by Windows package data behavior.

The portable bundle is unpackaged and receives no MSIX isolation, identity, or
automatic servicing. It must retain every adjacent native dependency and the
license bundle.
