/*
 * disc-remuxer: the numbers of the program's own messages, by area:
 * 1000 program and settings, 2000 sources and discs, 3000 titles,
 * 5000 streams and checks, 6000 output. Lines from FFmpeg and the
 * libraries under it carry no number (code 0) and name their source.
 */

#ifndef DISC_REMUXER_MSG_H
#define DISC_REMUXER_MSG_H

enum {
    MSG_PROGRAM_START        = 1001,
    MSG_SETTINGS_CREATED     = 1011,
    MSG_SETTING_ADDED        = 1012,
    MSG_SETTING_REMOVED      = 1013,
    MSG_SETTINGS_CHANGED     = 1015,
    MSG_SETTING_BAD          = 1016,
    MSG_SETTINGS_UNREADABLE  = 1017,
    MSG_SETTINGS_INVALID     = 1018,
    MSG_SETTINGS_UNWRITABLE  = 1019,
    MSG_SET_SYNTAX           = 1020,
    MSG_SET_UNKNOWN          = 1021,
    MSG_SET_BAD              = 1022,
    MSG_USAGE                = 1030,
    MSG_CONFIG_DIR           = 1040,

    MSG_SOURCE               = 2001,
    MSG_SOURCE_FAILED        = 2002,
    MSG_SOURCE_UNSUPPORTED   = 2004,

    MSG_TITLES               = 3001,
    MSG_TITLE_SELECTION      = 3002,
    MSG_TITLE_MISSING        = 3003,
    MSG_TITLE_OPEN_FAILED    = 3004,

    MSG_TRACK                = 5001,
    MSG_TRACK_EMPTY          = 5002,

    MSG_OUTPUT_FOLDER        = 6001,
    MSG_TITLE_DONE           = 6002,
    MSG_TITLE_FAILED         = 6003,
    MSG_FOLDER_FAILED        = 6007,
    MSG_UNTESTED             = 6008,
    MSG_CHAPTERS             = 6009,
    MSG_LOG_FILE             = 6010,
    MSG_SUMMARY              = 6020,
};

#endif
