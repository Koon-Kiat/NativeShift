# Adding a converter

Create a provider library that depends on `NativeShift.Core`, not on the CLI or
WinUI application.

Implement `IConversionProvider`:

- `Name` returns a stable diagnostic identifier.
- `CanHandle` is fast, deterministic, and must not inspect a file.
- `Validate` checks provider options and capability constraints.
- `EstimateOutput` returns a conservative disk-space estimate.
- `Convert` performs synchronous worker-thread conversion and checks its
  `std::stop_token` at safe boundaries.
- `GetSupportedFormats` and `GetAvailableOptions` expose capabilities without UI
  knowledge.

Provider errors must become `ProviderOutcome`; do not show dialogs, terminate
the process, or let codec exceptions cross the provider boundary. Never write
directly to the user-selected final path. The engine replaces `output_path` with
its temporary path before invoking the provider and commits it afterward.

## Checklist

1. Add new formats to `formats.hpp`, string mapping, kind mapping, and signature
   detection where reliable.
2. Add a new `src/NativeShift.<Name>` library and vcpkg dependencies.
3. Implement content validation beyond the initial signature.
4. Define dimension, duration, recursion, allocation, and decompression limits.
5. Preserve Unicode/long paths without narrowing Windows paths.
6. Stream data and bound buffers where the codec API permits.
7. Report progress monotonically and check cancellation.
8. Register the provider in the composition root (CLI and future App).
9. Generate tiny legal fixtures in integration tests.
10. Document dependency licenses and distribution configuration.

For document conversion, use a separate provider. Do not add Office automation
or document code to `NativeShift.Image` or `NativeShift.Media`.
