# Performance

NativeShift includes `NativeShift.Benchmarks`; it creates tiny legal inputs in a
temporary directory and emits JSON.

```powershell
New-Item -ItemType Directory -Force .\out\benchmarks | Out-Null
.\out\build\nativeshift-release\benchmarks\NativeShift.Benchmarks.exe |
  Set-Content .\out\benchmarks\core.json -Encoding utf8

.\out\build\nativeshift-release\benchmarks\NativeShift.Benchmarks.exe `
  --include-media |
  Set-Content .\out\benchmarks\media.json -Encoding utf8
```

The core run measures signature detection, output naming, BMP-to-PNG encoding,
queue scheduling, and a batch conversion. The optional media run adds
WAV-to-FLAC. Results include wall-clock time and operation throughput. Record
the JSON with CPU, memory, storage, Windows, compiler, build preset, codec
versions, and power mode because results vary by hardware and codec.

Image codecs use scanline or mapped input where supported and a bounded RGBA
representation for transforms. Media pipelines use FFmpeg packet/frame
streaming, bounded FIFOs, RAII contexts, and timestamp rescaling. The weighted
scheduler prevents software video work from consuming the same budget as a
small image. Runtime capability discovery is cached.

The decoded image ceiling is 100 million pixels. Do not raise resource limits,
worker count, or codec thread counts based on a single benchmark. Measure peak
memory and batch tail latency as well as throughput, and never trade output
integrity or validation for speed.

