# Threat model

## Assets and trust boundaries

NativeShift protects source files, generated output, user privacy, application
settings, and the integrity of distributed binaries. File bytes and metadata,
output paths, drag-and-drop data, optional external providers, dependencies,
installer inputs, and update artifacts are untrusted.

## Principal threats and controls

| Threat | Controls | Residual risk |
|---|---|---|
| Malformed media or metadata | Signature detection, FFmpeg/WIC error checks, bounded dimensions, RAII, transactional output | Native codec vulnerabilities |
| Path traversal or unsafe replacement | Canonical validation, reserved-name sanitation, explicit conflict policy, same-directory temporary output | Races with other local processes |
| Resource exhaustion | Input/dimension limits, weighted queue, bounded concurrency, cancellation, disk-space checks | Very complex legal media can consume CPU |
| Temporary-file disclosure | Same-directory unpredictable temporary names, cleanup on error/cancel | Local administrators can inspect storage |
| Log exposure | No content/passwords, basename-only normal logs, opt-in debug paths, rotation | Debug logs are privacy sensitive |
| External document conversion | Provider is not shipped; future provider must launch directly with an argument array, timeout, restricted temporary directory, cancellation, and cleanup | External executable security |
| Archive extraction | Provider is not shipped; future implementation requires zip-slip, size/count/ratio and unsafe-link defenses | Novel decompression bombs |
| Dependency compromise | Pinned baseline/actions, CodeQL, dependency review, vulnerability scans, SBOM, provenance | Upstream or build-system compromise |
| Unsafe installer/update | Package verification, checksums, provenance, protected signing, no embedded certificate | Unsigned development MSIX is not trusted |

NativeShift does not remove DRM, bypass document restrictions, crack archive
passwords, or send user media to a service. Password-protected formats may only
be supported after a secure user-supplied credential path exists.
