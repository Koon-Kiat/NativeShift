# Security

All selected files are untrusted, including files with familiar extensions.

## Implemented controls

- File signatures determine format; extensions are hints only outside the
  engine.
- The codec fully validates content and returns malformed-data errors.
- Encoded images are limited to 512 MiB.
- Decoded images are limited to 32,768 pixels per dimension and 100 million
  pixels total.
- Output filenames are built from `input.filename().stem()` only; parent
  traversal from input names is discarded.
- Recursive CLI output verifies that relative parents contain no `..`.
- Codec integration is in-process and never constructs shell commands.
- The Windows CLI is long-path aware; codec file handles use wide, extended
  paths.
- Outputs are written beside the destination under unpredictable temporary
  names and committed only after success.
- Failure and cancellation remove incomplete temporary files.
- Existing files require an explicit policy.
- Exceptions are caught at provider, engine, and job boundaries.
- Logs omit full paths unless debug path logging is explicitly enabled.
- No file contents or metadata are logged.

## Residual Phase 1 risks

Image decoding remains native parsing of hostile data. Keep vcpkg baselines and
codec security updates current, fuzz provider entry points before public
release, and run test/fuzz corpora under AddressSanitizer.

Decoded pixels are held in one RGBA buffer. The pixel cap bounds a buffer to
about 400 MiB, but several concurrent maximum-size images could still exhaust
memory. Do not increase the default concurrency or safety limits without
measurement. A future scheduler should admit jobs by estimated memory weight.

WebP decode is a single codec call over a read-only file mapping; cancellation
is observed immediately before and after that call, not from inside it. JPEG
decode/encode and resizing check cancellation per scanline. PNG's simplified
decode/encode API has the same between-call cancellation limitation as WebP.

BMP and TIFF are decoded through Windows Imaging Component into the same
bounded RGBA representation. Multi-page TIFF input uses only its first frame.

Phase 1 does not sandbox codecs in a separate process. Before handling files
from high-risk adversarial sources, evaluate a low-privilege broker/provider
process with job objects, memory limits, and a narrow IPC contract.

## Release checklist

- Review compiler warnings; project code builds with warnings as errors.
- Run unit/integration tests in Debug and Release.
- Run AddressSanitizer and fuzz suites.
- Audit generated dependency versions and licenses.
- Sign application binaries and installer.
- Verify no debug path logging is enabled in release configuration.
- Test standard-user execution; the manifest requests no elevation.
