# Architecture

## Component structure

```text
src/
  NativeShift.Core/             Requests, validation, registry, queue, settings
  NativeShift.Image/            PNG/JPEG/WebP and Windows BMP/TIFF
  NativeShift.Media/            Native FFmpeg media and capabilities
  NativeShift.Platform.Windows/ Windows storage and application data
  NativeShift.GuiBridge/        Versioned JSON DTOs over a narrow C ABI
  NativeShift.Cli/              Automation interface
  NativeShift.App/              WinUI 3 views and presentation models
tests/
  NativeShift.Core.Tests/
  NativeShift.Image.Tests/
  NativeShift.Media.Tests/
  NativeShift.GuiBridge.Tests/
```

## Dependency direction

```text
CLI -----------+
               v
WinUI -> GuiBridge -> Core <- Image / Media providers
                         ^
                         |
                  Windows platform services
```

Core knows only `IConversionProvider` and the narrow `IPlatformServices`
interface. It has no WinUI, file-picker, or codec dependency. The GUI receives
JSON DTOs, so generated C++/WinRT code never reaches conversion internals.

Provider conversion is synchronous and runs only on a bounded queue worker.
This makes the queue the single asynchronous boundary and prevents providers
from launching unbounded hidden work. Requests and results are values;
providers are registered in a composition root and selected by capability.

## Conversion lifecycle

1. Inspect content signatures and establish the authoritative input format.
2. Validate common safety constraints and provider-specific options.
3. Select a compatible provider and resolve the output conflict policy.
4. Estimate output and check available disk space.
5. Create an unpredictable temporary path beside the destination.
6. Decode, transform, and encode with progress and cooperative cancellation.
7. Commit with a same-volume rename only after successful encoding.
8. Remove temporary output on failure or cancellation.
9. Return an isolated result and write a privacy-filtered structured event.

No job exception is allowed across the engine or worker boundary.

## Scheduling

The queue has a fixed worker pool, bounded pending work, per-job stop sources,
observable snapshots, pause/resume/retry, and failure isolation. Admission is
resource weighted: images and audio cost one unit, hardware video two, and
software video four. The default resource budget is derived conservatively
from processor count and available memory. FIFO ordering is retained among
jobs that fit the current budget.

The GUI bridge owns one process-lifetime composition root because the DLL must
retain the queue behind its C ABI. This is the only singleton-like boundary;
the core and providers use explicit ownership and dependency injection.

## Provider choices

The pinned vcpkg registry has no official libvips port. NativeShift uses
maintained libpng, libjpeg-turbo, libwebp ports and Windows Imaging Component
inside `NativeShift.Image`. `NativeShift.Media` uses FFmpeg libraries directly,
not an executable or shell command. Both remain replaceable providers.

Optional document and archive features must be separate providers. Document
tools require direct argument-array process launch, timeout, cancellation, and
restricted temporary storage. Archive support requires traversal,
decompression, file-count, ratio, and unsafe-link defenses.

## Threading contract

- Provider instances support calls from multiple workers.
- Progress callbacks may be concurrent and are shielded from exceptions.
- WinUI polling and callbacks marshal presentation changes to its dispatcher.
- Cancellation is cooperative; workers are never forcibly terminated.
- Capability discovery is immutable and cached after one guarded probe.
