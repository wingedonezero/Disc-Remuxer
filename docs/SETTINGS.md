# Settings

Generated from the settings registry (`crates/disc-core/src/settings.rs`); a test keeps this page in step (`UPDATE_DOCS=1 cargo test -p disc-core` rewrites it).

The settings file (`~/.config/disc-remuxer/settings.toml`, or `--settings FILE`) lists every setting below with its value. It is created on the first run and kept in step with the program on every start: missing settings are added with their default, unknown entries removed, kept values never changed, the old file saved as a backup first. A wrong value stops the program and leaves the file as it is. The value in effect comes from the default, then the settings file, then the command line (`--set group.key=value`).

## [output]

| Setting | Allowed | Default | What it does |
|---|---|---|---|
| `output.root` | a folder; empty = not set | `""` | Folder the job folders are created in. Not set: the output folder must be given on the command line (--out). |
| `output.keep_structure` | true or false | `true` | When a folder holding several discs is given, recreate its sub-folders under the output root, so each disc's job folder sits where the disc sat. |
| `output.scan_depth` | 0 to 20 | `5` | How many folder levels below a given folder are searched for discs (VIDEO_TS folders, BDMV folders, disc images). |

## [log]

| Setting | Allowed | Default | What it does |
|---|---|---|---|
| `log.console` | error, warning, info, debug, trace | `"info"` | Most detailed level shown on the console; -q and -v override it for one run. |
| `log.debug_file` | true or false | `false` | Also write the detailed debug log (every step, library messages included) into the job folder, next to the job log. |
