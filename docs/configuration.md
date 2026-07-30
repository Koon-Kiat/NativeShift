# Configuration

NativeShift keeps versioned JSON settings below the current user's local
application-data directory. It stores output preferences, last-used
directories, preferred formats, custom presets, concurrency, acceleration,
metadata and conflict policy, theme, log level, notifications, and recent
preset identifiers. It does not persist file contents, passphrases, or recent
filenames.

Schema version 2 migrates the earlier `UniversalFileConverter` identity. Before
a migration, NativeShift creates a `.vN.bak` backup. Corrupt files are reported
and replaced with safe defaults; unknown fields are ignored, while a
future-version file is rejected rather than overwritten. The Settings page can
reset defaults.

Built-in presets are read-only. Duplicating one creates a custom preset that
can be edited, renamed, or deleted. Compatibility is checked against the
selected input and output formats before a job starts.

Logs are local, structured, rotated, and size bounded. Information-level logs
use filenames rather than full paths; full paths are available only when the
user explicitly enables debug logging.
