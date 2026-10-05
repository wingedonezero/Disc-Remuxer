# Settings

Generated from the settings registry (`crates/disc-core/src/settings.rs`); a test keeps this page in step (`UPDATE_DOCS=1 cargo test -p disc-core` rewrites it).

The settings file (`~/.config/disc-remuxer/settings.toml`, or `--settings FILE`) lists every setting below with its value. It is created on the first run and kept in step with the program on every start: missing settings are added with their default, unknown entries removed, kept values never changed, the old file saved as a backup first. A wrong value stops the program and leaves the file as it is. The value in effect comes from the default, then the settings file, then the command line (`--set group.key=value`).

## [output]

| Setting | Allowed | Default | What it does |
|---|---|---|---|
| `output.root` | a folder; empty = not set | `""` | Folder the job folders are created in. Not set: the output folder must be given on the command line (--out). |
| `output.keep_structure` | true or false | `true` | When a folder holding several discs is given, recreate its sub-folders under the output root, so each disc's job folder sits where the disc sat. |
| `output.scan_depth` | 0 to 20 | `5` | How many folder levels below a given folder are searched for discs (VIDEO_TS folders, BDMV folders, disc images). |

## [read]

| Setting | Allowed | Default | What it does |
|---|---|---|---|
| `read.attempts` | 1 to 99 | `5` | How many times a read from a disc is tried before it counts as failed; after the first retry the rest of the read goes one sector at a time. |
| `read.udf_reader` | netbsd, linux | `"netbsd"` | Which UDF reader opens disc images: netbsd (based on NetBSD's UDF code) or linux (based on Linux's). |
| `read.prefer_iso_for_old_udf102` | true or false | `true` | Read a DVD image whose UDF 1.02 file system was recorded before 2006 through its ISO 9660 file system, when that holds a valid DVD-Video structure. |

## [dvd]

| Setting | Allowed | Default | What it does |
|---|---|---|---|
| `dvd.cell_mode` | auto, walk, trim, walk_trim | `"auto"` | How a DVD title's cells are chosen: walk = the cells the disc's navigation plays (found by playing the navigation through), trim = cells that do not look like content are trimmed off the program chain's ends (the navigation is not played), walk_trim = walk, trim where walk finds nothing, auto = walk when the navigation found titles, else trim. |
| `dvd.title_order` | auto, scan_first, table | `"auto"` | Order of a DVD's titles: scan_first = the titles the disc's navigation leads to first, then the others; table = the disc's title table order; auto = scan_first with the cell modes auto and walk, table with trim and walk_trim. |
| `dvd.min_title_length` | 0 to 86400 | `120` | Titles shorter than this many seconds are listed but not selected. |

## [log]

| Setting | Allowed | Default | What it does |
|---|---|---|---|
| `log.console` | error, warning, info, debug, trace | `"info"` | Most detailed level shown on the console; -q and -v override it for one run. |
| `log.debug_file` | true or false | `false` | Also write the detailed debug log (every step, library messages included) into the job folder, next to the job log. |
