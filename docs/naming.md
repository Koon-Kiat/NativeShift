# Naming decision

## Selected name

The application is named **NativeShift**.

The name combines the product's native Windows implementation with its purpose:
shifting files between formats entirely on the user's computer. It is short,
easy to spell, not tied to one media category, and works as a product name,
repository name, executable prefix, package identity, and C++ namespace.

The standard identifiers are:

- Product: `NativeShift`
- Repository: `NativeShift`
- C++ namespace: `nativeshift`
- CLI executable: `nativeshift-cli.exe`
- CMake targets: `NativeShift.*`
- Application-data directory: `%LOCALAPPDATA%\NativeShift`
- Release artifacts: `NativeShift-<version>-*`

## Candidates considered

| Candidate | Outcome |
|---|---|
| NativeShift | Selected; no obvious file-converter collision found in a preliminary exact-name web search |
| Formatloom | Memorable, but less immediately associated with native desktop software |
| FileWeave | Rejected because the name has been used by file-transfer software |
| Veloform | Rejected because established companies already use the name |
| LocalMorph | Rejected because an existing local image/audio/video converter uses the name |

Earlier descriptive candidates such as FileShift, FormatForge, and LocalConvert
were also rejected because active converter products already use them.

These checks were limited to public web and product-search results on
2026-07-31. They are not trademark clearance, do not establish international
availability, and should be followed by jurisdiction-specific legal review
before commercial distribution.

## Compatibility

Phase 1 stored settings under `%LOCALAPPDATA%\UniversalFileConverter`.
NativeShift checks that legacy location once when the new settings file is
absent, migrates valid settings transactionally, and archives the legacy file
as `settings.json.migration.bak`. Third-party namespaces and dependency names
are unchanged.
