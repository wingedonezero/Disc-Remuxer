# Commands and options

Generated from the command-line definitions (`crates/disc-cli/src/main.rs`); a test keeps this page in step (`UPDATE_DOCS=1 cargo test -p disc-cli` rewrites it). Settings: `docs/SETTINGS.md`.

## `disc-remuxer`

```text
Rips DVD, Blu-ray and HD DVD sources

Usage: disc-remuxer [OPTIONS] <COMMAND>

Commands:
  version   Program version and the versions of the bundled libraries
  settings  The settings in effect, or where the settings file is
  scan      List the discs found under a path and the job folder each would get. Creates nothing
  probe     Open one title of every disc found with FFmpeg's DVD-Video demuxer as it is and log the streams it finds. With an output folder, every disc is a job with its own job folder and job log
  demux     Write the streams of HD DVD titles as elementary-stream files, one file per track, and the chapters as Matroska XML, into each disc's job folder (files <title>_<stream>_<language>.<ext>, <title>_chapters.xml)
  debug     Developer tools: the steps of the DVD processing one at a time, with their raw results on standard output
  help      Print this message or the help of the given subcommand(s)

Options:
  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help

  -V, --version
          Print version
```

## `disc-remuxer version`

```text
Program version and the versions of the bundled libraries

Usage: disc-remuxer version [OPTIONS]

Options:
  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```

## `disc-remuxer settings`

```text
The settings in effect, or where the settings file is

Usage: disc-remuxer settings [OPTIONS] [COMMAND]

Commands:
  show  Every setting with its value and where the value came from (default)
  path  The path of the settings file
  help  Print this message or the help of the given subcommand(s)

Options:
  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```

## `disc-remuxer settings show`

```text
Every setting with its value and where the value came from (default)

Usage: disc-remuxer settings show [OPTIONS]

Options:
  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```

## `disc-remuxer settings path`

```text
The path of the settings file

Usage: disc-remuxer settings path [OPTIONS]

Options:
  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```

## `disc-remuxer scan`

```text
List the discs found under a path and the job folder each would get. Creates nothing

Usage: disc-remuxer scan [OPTIONS] <SOURCE>

Arguments:
  <SOURCE>
          A disc (folder or image) or a folder holding discs

Options:
      --out <DIR>
          Output folder (overrides output.root)

  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```

## `disc-remuxer probe`

```text
Open one title of every disc found with FFmpeg's DVD-Video demuxer as it is and log the streams it finds. With an output folder, every disc is a job with its own job folder and job log

Usage: disc-remuxer probe [OPTIONS] <SOURCE>

Arguments:
  <SOURCE>
          A disc (folder or image) or a folder holding discs

Options:
      --title <TITLE>
          Title number (1-based; 0 = the demuxer's own choice)
          
          [default: 1]

  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

      --out <DIR>
          Output folder for the job folders (overrides output.root)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```

## `disc-remuxer demux`

```text
Write the streams of HD DVD titles as elementary-stream files, one file per track, and the chapters as Matroska XML, into each disc's job folder (files <title>_<stream>_<language>.<ext>, <title>_chapters.xml)

Usage: disc-remuxer demux [OPTIONS] <SOURCE>

Arguments:
  <SOURCE>
          A disc image or a folder holding disc images

Options:
      --title <TITLE>
          Title number (0-based, as listed); every title when not given

  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

      --out <DIR>
          Output folder for the job folders (overrides output.root)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```

## `disc-remuxer debug`

```text
Developer tools: the steps of the DVD processing one at a time, with their raw results on standard output

Usage: disc-remuxer debug [OPTIONS] <COMMAND>

Commands:
  dvd-scan    Scan the navigation of one DVD (folder or image) and print the cell sequence found for each title and program chain: `result <start> title <n> pgc <n> cells <list>`, then `entered <title set>` for every title set the navigation played, or `scan failed: <reason>`
  dvd-titles  Build the title plan of one DVD (folder or image) with the dvd.* settings and print the findings made (`event <name>` and its arguments, tab-separated) and every title: `title <name> vts <n> pgc <n> angle <a>/<n> cells <n> chapters <n> length <h:mm:ss> measured <s> size <bytes> segments <n> map <cells> scan <start> selected <yes|short|fake>`
  help        Print this message or the help of the given subcommand(s)

Options:
  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```

## `disc-remuxer debug dvd-scan`

```text
Scan the navigation of one DVD (folder or image) and print the cell sequence found for each title and program chain: `result <start> title <n> pgc <n> cells <list>`, then `entered <title set>` for every title set the navigation played, or `scan failed: <reason>`

Usage: disc-remuxer debug dvd-scan [OPTIONS] <SOURCE>

Arguments:
  <SOURCE>
          The disc: a DVD folder or image

Options:
      --trace
          Print every navigator call, event and sector read before the results

  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```

## `disc-remuxer debug dvd-titles`

```text
Build the title plan of one DVD (folder or image) with the dvd.* settings and print the findings made (`event <name>` and its arguments, tab-separated) and every title: `title <name> vts <n> pgc <n> angle <a>/<n> cells <n> chapters <n> length <h:mm:ss> measured <s> size <bytes> segments <n> map <cells> scan <start> selected <yes|short|fake>`

Usage: disc-remuxer debug dvd-titles [OPTIONS] <SOURCE>

Arguments:
  <SOURCE>
          The disc: a DVD folder or image

Options:
  -v, --verbose...
          More log detail on the console: -v debug, -vv trace (overrides log.console)

  -q, --quiet
          Errors only on the console (overrides log.console)

      --settings <FILE>
          Settings file to use instead of the default one

      --set <GROUP.KEY=VALUE>
          Override one setting for this run (repeatable), e.g. `--set output.scan_depth=2`

  -h, --help
          Print help
```
