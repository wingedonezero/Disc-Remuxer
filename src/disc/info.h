/*
 * Disc info: what the disc layer knows about the disc it opened, for the
 * caller's overview. The disc (kind, label, file system), each title of its
 * title list (name, length, size, the files it plays, chapters) and each
 * title's streams (kind, codec, language and the stream details). A value
 * of 0 (a number) or NULL (a string) is not known.
 *
 * The enum values are pinned to the processing library's (so values cross
 * the migration seam unchanged; asserted in libavformat): the stream kinds
 * to its media types, the codec ids to DRCodecID (timeline/disctl.h) =
 * its codec ids, the unknown profile / level to its -99.
 */

#ifndef DISC_DISC_INFO_H
#define DISC_DISC_INFO_H

#include <stdint.h>

/** What a stream is (DiscStreamInfo.type). */
enum DiscMediaKind {
    DISC_MEDIA_VIDEO    = 0,
    DISC_MEDIA_AUDIO    = 1,
    DISC_MEDIA_DATA     = 2,
    DISC_MEDIA_SUBTITLE = 3,
};

/** The kind in words ("video", "audio", "subtitle", "data"), as the
 *  processing library names its media types. */
static inline const char *disc_media_name(int type)
{
    switch (type) {
    case DISC_MEDIA_VIDEO:    return "video";
    case DISC_MEDIA_AUDIO:    return "audio";
    case DISC_MEDIA_SUBTITLE: return "subtitle";
    case DISC_MEDIA_DATA:     return "data";
    }
    return "?";
}

#define DISC_PROFILE_UNKNOWN (-99)
#define DISC_LEVEL_UNKNOWN   (-99)

/* The profiles the names depend on (DiscStreamInfo.profile; pinned like the
 * rest). H.264 (profile_idc, with constraint_set1 / set3 as bits 9 / 11)
 * and H.265 (general_profile_idc) give their profile's name. */
#define DISC_PROFILE_DTS_HD_HRA              50
#define DISC_PROFILE_DTS_HD_MA               60
#define DISC_PROFILE_DTS_HD_MA_X             61
#define DISC_PROFILE_DTS_HD_MA_X_IMAX        62
#define DISC_PROFILE_DTS_EXPRESS             70
#define DISC_PROFILE_TRUEHD_ATMOS            30
#define DISC_PROFILE_EAC3_DDP_ATMOS          30
#define DISC_PROFILE_H264_CONSTRAINED        (1 << 9)
#define DISC_PROFILE_H264_INTRA              (1 << 11)
#define DISC_PROFILE_H264_BASELINE           66
#define DISC_PROFILE_H264_MAIN               77
#define DISC_PROFILE_H264_EXTENDED           88
#define DISC_PROFILE_H264_HIGH               100
#define DISC_PROFILE_H264_HIGH_10            110
#define DISC_PROFILE_H264_MULTIVIEW_HIGH     118
#define DISC_PROFILE_H264_HIGH_422           122
#define DISC_PROFILE_H264_STEREO_HIGH        128
#define DISC_PROFILE_H264_HIGH_444           144
#define DISC_PROFILE_H264_HIGH_444_PREDICTIVE 244
#define DISC_PROFILE_H264_CAVLC_444          44
#define DISC_PROFILE_HEVC_MAIN               1
#define DISC_PROFILE_HEVC_MAIN_10            2
#define DISC_PROFILE_HEVC_MAIN_STILL_PICTURE 3
#define DISC_PROFILE_HEVC_REXT               4
#define DISC_PROFILE_HEVC_MULTIVIEW_MAIN     6
#define DISC_PROFILE_HEVC_SCC                9

/* Channel bits of DiscStreamInfo.layout (pinned): bit n is speaker n of
 * the list in codecs.c (FL FR FC LFE BL BR ...). */
#define DISC_CH_FRONT_LEFT       0x1ULL
#define DISC_CH_FRONT_RIGHT      0x2ULL
#define DISC_CH_FRONT_CENTER     0x4ULL
#define DISC_CH_LOW_FREQUENCY    0x8ULL
#define DISC_CH_LOW_FREQUENCY_2  0x800000000ULL
#define DISC_CH_LAYOUT_MONO      (DISC_CH_FRONT_CENTER)
#define DISC_CH_LAYOUT_STEREO    (DISC_CH_FRONT_LEFT | DISC_CH_FRONT_RIGHT)

/** A ratio (an aspect, a frame rate): num / den; 0 / 0 when not known. */
typedef struct DrRational {
    int num, den;
} DrRational;

/** A stream of a title. */
/* DiscStreamInfo.roles: what the disc says a stream is for */
#define DISC_ROLE_COMMENTARY        0x0001
#define DISC_ROLE_ALT_COMMENTARY    0x0002
#define DISC_ROLE_VISUALLY_IMPAIRED 0x0004
#define DISC_ROLE_LARGE             0x0008
#define DISC_ROLE_CHILDREN          0x0010
#define DISC_ROLE_FORCED            0x1000  /**< a stream of forced subtitles (not DiscStreamInfo.forced) */

typedef struct DiscStreamInfo {
    int              type;           /**< enum DiscMediaKind */
    int              codec;          /**< DRCodecID; DR_CODEC_NONE: no codec for the disc's stream */
    char             language[4];    /**< ISO 639-2, "" when the disc names none */
    int              inner_track;    /**< the stream taken out of this one (DTS out of DTS-HD MA), -1 none */
    int              from_track;     /**< the stream this one is taken out of, -1 none */
    int              forced;         /**< subtitles: only the forced ones (taken out of from_track) */
    int              second_view;    /**< video: the second view of a 3-D video (from_track is the first) */
    int              second_layer;   /**< video: a second (enhancement) layer on its own PID is read with it */
    int              offset_sequence;/**< subtitles of a 3-D title: the disc's offset sequence for its depth, -1 none */
    int              roles;          /**< DISC_ROLE_*, 0 none */
    int              width, height;  /**< video: the frame; subtitles: the canvas the units are drawn on */
    /* the stream details (from the stream itself; 0 / unknown when not read) */
    DrRational       aspect;         /**< video: display aspect ratio */
    DrRational       frame_rate;     /**< video */
    int              profile;        /**< the library's profile value of the codec, DISC_PROFILE_UNKNOWN */
    int              level;          /**< video: the codec's level (level_idc), DISC_LEVEL_UNKNOWN */
    char             dolby_vision[40];/**< video: the Dolby Vision type ("dvhe.07.06 BL+FEL+RPU"), "" none */
    char             hdr[16];        /**< video: "HDR10", "HDR10+", "HLG" or "SDR" from its first picture's colour
                                          description and metadata, "" not known */
    char             colour[96];     /**< video: its colour description (primaries, transfer, matrix, range), ""
                                          not stated */
    int              full_range;     /**< video: its first picture says full colour range (disc video is limited) */
    char             mastering[192]; /**< video: the mastering display's luminance (and primaries), "" none */
    int              max_cll, max_fall;/**< video: content light level (cd/m²), 0 none */
    int              channels;       /**< audio */
    uint64_t         layout;         /**< audio: channel layout mask (the library's AV_CH_* bits), 0 unknown */
    int              sample_rate;    /**< audio, Hz */
    int              bits;           /**< audio: bits per sample the stream carries (0 unknown) */
    int64_t          bit_rate;       /**< bit/s (0 unknown) */
} DiscStreamInfo;

/** A chapter of a title, as the disc records it. */
typedef struct DiscChapterInfo {
    int64_t          start;          /**< microseconds, from the title's start */
    const char      *name;           /**< NULL when the disc names none */
} DiscChapterInfo;

/** A part of a title that plays whole stream files back to back (the
 *  episodes of a series in one title): what the option part opens on its
 *  own, as if it were a title of its own. */
typedef struct DiscPartInfo {
    int64_t          start;          /**< microseconds, from the title's start */
    int64_t          duration;       /**< microseconds */
    int              first_file;     /**< its files: files[first_file .. first_file + nb_files) of the title */
    int              nb_files;
    int              nb_chapters;    /**< its own chapters (from its own start) */
} DiscPartInfo;

/** What a property (DiscPropertyInfo) is: its words, its group and how its
 *  value reads come from the key (disc_prop_name() and the others below),
 *  so nothing is ever matched by its English words. */
typedef enum DiscPropKey {
    DISC_PROP_KIND,
    DISC_PROP_MARKET_LANGUAGE,
    DISC_PROP_REGION,
    DISC_PROP_IFO_CHECK,
    DISC_PROP_MENU_NAMES,
    DISC_PROP_FAKE_PLAYLISTS,
    DISC_PROP_NAME_LANGUAGE,
    DISC_PROP_ENGLISH_NAME,
    DISC_PROP_CONTENT_HASH_CHECK,
    DISC_PROP_PLAYLISTS,
    DISC_PROP_CLIPS,
    DISC_PROP_BDJ_ARCHIVES,
    DISC_PROP_CSS,
    DISC_PROP_AACS,
    DISC_PROP_AACS2,
    DISC_PROP_AACS_DISC_ID,
    DISC_PROP_BDPLUS,
    DISC_PROP_BUS_ENCRYPTION,
    DISC_PROP_TITLE_ON_DISC,
    DISC_PROP_TITLE_SET,
    DISC_PROP_PROGRAM_CHAIN,
    DISC_PROP_NAVIGATION,
    DISC_PROP_ANGLES,
    DISC_PROP_CELLS,
    DISC_PROP_LENGTH_MEASURED,
    DISC_PROP_OTHER_NAME,
    DISC_PROP_RECORDED,
    DISC_PROP_SIDE_CLIPS,
    DISC_PROP_FAKE_PLAYLIST_CHECK,
    DISC_PROP_FROM,
    DISC_PROP_TRACKS_FROM,
    DISC_PROP_NB
} DiscPropKey;

/** Where a property belongs in a disc's details. */
typedef enum DiscPropGroup {
    DISC_PROP_GENERAL,
    DISC_PROP_PROTECTION,              /**< a block of its own, "Protection" */
} DiscPropGroup;

/** Something more the format part tells of a disc or a title (for the
 *  caller's details): what it is (key; its words: name) and its value in
 *  words. */
typedef struct DiscPropertyInfo {
    DiscPropKey      key;
    const char      *name;           /**< disc_prop_name(key), e.g. "Title set" */
    char            *value;          /**< e.g. "VTS_03, its title 2" */
} DiscPropertyInfo;

/** A property's name ("Protection: CSS"), its group, its label within its
 *  group ("CSS"), and whether its value names a language. */
const char   *disc_prop_name(DiscPropKey key);
DiscPropGroup disc_prop_group(DiscPropKey key);
const char   *disc_prop_label(DiscPropKey key);
int           disc_prop_language(DiscPropKey key);
/** The property of key among n props, NULL when there is none. */
const DiscPropertyInfo *disc_prop_find(const DiscPropertyInfo *props, int n, DiscPropKey key);

/** A title's mark from a disc's fake playlist check (DiscTitleInfo.playlist_check). */
enum DiscPlaylistCheck {
    DISC_PLAYLIST_NOT_MARKED = 0,   /**< no check, nothing found, or the title is neither */
    DISC_PLAYLIST_LIKELY_FAKE,      /**< plays copies of one clip more than once (a fake hiding the main feature) */
    DISC_PLAYLIST_LIKELY_REAL,      /**< on a disc with fakes: no clip played twice, as long as the fakes */
};

/** A title of the disc's title list. */
typedef struct DiscTitleInfo {
    int              index;          /**< its number in the title list (the option title), from 0 */
    const char      *name;           /**< NULL when the disc names none */
    const char      *source;         /**< what the title is made from (e.g. "00800.mpls(1)"), NULL when not told */
    int              angle;          /**< its angle (1-based) when the disc has several, else 0 */
    int              main_angle;     /**< with angle: the angle a player plays (1-based), 0 when not told */
    int              main_title;     /**< with main_angle: the index of that angle's title, -1 not listed */
    uint8_t          angle_order[10]; /**< with angle: the angles in the order the disc plays them (1-based,
                                          0 ends); empty when not told */
    const char      *shared_cells;   /**< with angle: the cells (DVD) all its angles play, "1, 2"; NULL none */
    char             language[4];    /**< the title's language (ISO 639-2), "" when none */
    int64_t          duration;       /**< microseconds, as the disc records it */
    int64_t          size;           /**< bytes of the files it plays */
    int              selected;       /**< 0: not ripped with "all" (an angle other than the main one) */
    int              nb_files;
    const char     **files;          /**< the disc's files it plays, in order (e.g. "DEL01.EVO") */
    int              nb_chapters;
    DiscChapterInfo *chapters;
    int              nb_streams;
    DiscStreamInfo  *streams;
    int              nb_parts;       /**< 0: the title is one piece */
    DiscPartInfo    *parts;
    int              parts_on_request; /**< written as parts only when chosen to be split (not by the setting
                                            output.split_joined_titles) */
    const char      *part_unit;      /**< what one part is ("separately authored piece"); NULL: a stream file */
    int              nb_props;
    DiscPropertyInfo *props;         /**< what the format part tells more of it, in its order */
    int              playlist_check; /**< enum DiscPlaylistCheck: marked only on a disc with fakes found */
    const char      *menu_name;      /**< the name the disc's menu data gives it (e.g. "FPL_MainFeature"), NULL none */
} DiscTitleInfo;

/** The disc. */
typedef struct DiscInfo {
    const char      *kind;           /**< "HD DVD", ... */
    const char      *label;          /**< the volume label */
    const char      *name;           /**< the disc's own name (e.g. a Blu-ray disc library title), NULL when none */
    const char      *filesystem;     /**< the file system read, e.g. "UDF (NetBSD)" */
    int              udf_revision;   /**< its UDF revision (0x0250 = 2.50), 0 when not UDF */
    int              encrypted;
    int              nb_titles;
    DiscTitleInfo   *titles;
    int              nb_props;
    DiscPropertyInfo *props;         /**< what the format part tells more of the disc, in its order */
    int              fake_playlists; /**< playlists found likely fake (titles marked), 0 none */
    int              fakes_suspected;/**< no known kind of fake found, but the disc looks like one with fakes */
    const char      *menu_name;      /**< the project name in the disc's menu data, NULL none */
    int              bdplus;         /**< the disc uses BD+ (not supported: its video may be damaged) */
} DiscInfo;

#endif /* DISC_DISC_INFO_H */
