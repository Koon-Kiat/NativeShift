# Architecture

## Component structure

```text
src/
  NativeShift.Core/   Models, detection, validation, output transaction,
                      provider registry, settings, logging, bounded queue
  NativeShift.Image/  PNG/JPEG/WebP provider using native codec libraries
  NativeShift.Platform.Windows/
                      Windows storage and application-data services
  NativeShift.Cli/    Automation interface using the same engine and queue
tests/
  NativeShift.Core.Tests/
  NativeShift.Image.Tests/
```

Later phases add `App`, `NativeShift.Media`, and optional document
providers without moving codec logic into the UI.

## Dependency direction

```text
CLI / future WinUI
        |
        v
  NativeShift.Core <---- NativeShift.Image / NativeShift.Media
        ^
        |
 NativeShift.Platform.Windows / future Document providers
```

`NativeShift.Core` knows only `IConversionProvider` and the narrow
`IPlatformServices` abstraction. It has no WinUI, codec, or file-picker
dependency. `ProviderRegistry` owns provider discovery and capability
enumeration. Presentation layers construct an engine, register providers,
submit requests, and consume progress/results.

Provider conversion is synchronous by design and is invoked only on a bounded
queue worker. Returning an internally launched future from each provider would
allow codecs to create unbounded work outside the scheduler. The queue itself is
the asynchronous boundary exposed to the CLI and future WinUI view models.

## Conversion lifecycle

1. Inspect content signatures and set the authoritative input format.
2. Validate common request fields and safety limits.
3. Select a provider and run provider-specific validation.
4. Resolve the existing-file policy.
5. Check available disk space against a conservative estimate.
6. Create a unique temporary path beside the final output.
7. Decode, transform, and encode on a worker thread.
8. Commit the temporary file with a same-volume rename.
9. Remove temporary output on failure or cancellation.
10. Return an isolated result and write a privacy-filtered structured log event.

No single job exception is allowed to escape the engine or worker boundary.

## Scheduling

`JobQueue` has a fixed worker count (1-32), a bounded pending deque, per-job stop
sources, pause/resume for jobs that have not started, and failure isolation.
The safe default is half the logical hardware threads, clamped to 1–4. The CLI
can change this using `--jobs`; future settings and UI use the same constructor.

The media phase should add a weighted or class-aware admission controller so a
small image and a memory-intensive video job do not count as equivalent work.

## Image provider choice

The requested preference was libvips where practical. At the Phase 1 vcpkg
baseline there is no official libvips port. Maintaining an overlay port would
add a large GLib stack and create project-owned package maintenance before the
engine is validated. Phase 1 therefore uses official vcpkg ports for libpng,
libjpeg-turbo, and libwebp.

This decision is contained inside `NativeShift.Image`. A libvips or WIC provider
can be registered later, ordered by capability, without changing requests,
scheduling, CLI parsing, or transactional output behavior.

## Threading contract

- Requests and results are value objects.
- Provider instances must support calls from multiple queue workers.
- Progress callbacks may run concurrently and never run on a UI thread by
  implication.
- The engine shields jobs from callback exceptions.
- WinUI view models must marshal progress onto their dispatcher.
- Cancellation is cooperative; no worker is forcibly terminated.
