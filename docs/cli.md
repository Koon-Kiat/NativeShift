# Command-line interface

`nativeshift-cli` uses the same providers, validation, presets, transactional
output writer, and cancellation model as the GUI.

```powershell
nativeshift-cli input.png --to webp --quality 82
nativeshift-cli input.wav --to mp3 --audio-bitrate 320k
nativeshift-cli input.mkv --to mp4 --video-codec h264 --audio-codec aac
nativeshift-cli input.mkv --to mp4 --hardware auto
nativeshift-cli .\input-folder --to jpeg --output .\converted --recursive
nativeshift-cli --list-formats
nativeshift-cli --list-codecs
nativeshift-cli --list-hardware
nativeshift-cli --list-presets
```

Use `--preset <id>` as a baseline; later explicit arguments override its
values. `--conflict fail|overwrite|rename|skip` controls existing outputs.
`--json` emits stable JSON to standard output and sends no progress text there.
Use `--verbose` for progress on standard error or `--quiet` to suppress normal
text. Ctrl+C requests cooperative cancellation.

Exit codes:

| Code | Meaning |
|---:|---|
| 0 | Every requested conversion succeeded or was skipped by policy |
| 2 | Invalid command-line syntax |
| 3 | Invalid or unsupported input |
| 4 | Conversion or file-system failure |
| 130 | Cancelled |

Run `nativeshift-cli --help` for the authoritative option list for the installed
version.
