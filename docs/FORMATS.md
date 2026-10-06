# What every format demuxer gives the program

`disc-remuxer` has no format-specific code. Each disc format has one
orchestrator demuxer in our FFmpeg (HD DVD: `hddvd`; DVD: `dvdvideo`;
Blu-ray, UHD and 3D: one Blu-ray demuxer), runs its titles through the
shared rip core (`libavformat/discrip_*.c`) and gives the program the same
things. The program tries the demuxers in turn and shows what they report.

## Options the program sets

| Option | Meaning |
|---|---|
| `title` | the title to open (index in the disc's title list, from 0) |
| `read_attempts`, `udf_reader` | from the settings `read.attempts`, `read.udf_reader` |
| `keydb` | key files (setting `aacs.key_files`), where the format has keys |

## Metadata on open (`AVFormatContext.metadata`)

| Key | Value |
|---|---|
| `disc` | the disc's kind, e.g. `HD DVD`, `DVD`, `Blu-ray`, `UHD Blu-ray` |
| `label` | the volume label |
| `filesystem` | the file system read (e.g. `UDF (NetBSD)`); `udf_revision` its UDF revision when UDF |
| `encrypted` | 1 / 0 |
| `titles` | the number of titles in the disc's title list |
| `title` | the opened title's name (the file-name template's `NAME`) |
| `chapters` | the opened title's chapter records |

Streams carry `language` (ISO 639-2) in their metadata. The title's
duration is `AVFormatContext.duration`.

## While reading

The read-only, exported option `progress` (0..10000) says how much of the
title has been read.

## At the end (after the last packet)

| Where | Key | Value |
|---|---|---|
| each stream | `frames` | frames given out |
| each stream | `warnings` | warnings logged about the track |
| each stream | `delay_us` | audio: its start delay (µs) |
| the context | `untested` | features met that no real disc has tested (the job is reported failed) |

Chapters are `AVFormatContext.chapters`, set at the end (the rip core marks
them on the video's key frames).

## Log

Everything the demuxer and the rip core have to say goes through
`av_log`: errors and warnings a reader must see at `AV_LOG_ERROR` /
`AV_LOG_WARNING` in plain language, normal steps at `AV_LOG_INFO`, details
at `AV_LOG_VERBOSE` / `AV_LOG_DEBUG`. Messages that come again on every title
open (the disc's file system, its encryption) are verbose: the program
shows the disc once from the metadata above.
