/*
 * HD DVD (Advanced Content): the playlists (ADV_OBJ/VPLST###.XPL), parsed
 * with expat into a tree of the elements this reader knows.
 *
 * Rules (the same for every element):
 * - names are matched exactly (case-sensitive) after dropping a namespace
 *   prefix (everything up to the first ':'), element and attribute names
 *   alike;
 * - the document's only accepted root element is <Playlist>; an element
 *   type accepts only the child elements of its table: any other element
 *   is skipped with everything inside it, without a message;
 * - an element's attributes start at their defaults, then each attribute
 *   of its table present in the element is set (a repeated one: the last
 *   wins); other attributes are ignored. String attributes keep their text;
 *   number attributes are read with strtoul() (base 10, so "12abc" is 12 and
 *   "x" is 0, kept as 32 bits); boolean attributes are true for "true" or
 *   "yes" in any case, false for anything else ("1" included);
 * - character data is collected into one 4096-byte buffer (the rest is
 *   dropped): the start of an element empties it, the end of an element
 *   gives the element whatever the buffer holds (when not empty) without
 *   emptying it, so an element without text of its own whose last child had
 *   text gets that text, plus the character data after the child (e.g. the
 *   white space before the element's end tag). Character data inside a
 *   skipped element is collected too.
 * - the file is read in pieces of at most 16 KiB, cut at 2048-byte
 *   boundaries until the reads are aligned; XML that is not well-formed (or
 *   an unreadable piece) loses the whole file.
 *
 * File framing: a file whose first four bytes are "AACS" holds the XML at
 * offset 0x11b, its length a big-endian u32 at offset 7; byte 4 (the
 * header's type) must be 0x02, 0x12 or 0x21. Other files are XML as a whole.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <expat.h>

#include "base/str.h"
#include "base/err.h"
#include "base/bytes.h"
#include "base/vlog.h"
#include "base/macros.h"
#include "base/mem.h"

#include "hddvd_internal.h"

#define XPL_CHUNK       0x4000      /* largest read */
#define XPL_TEXT_MAX    0x1000      /* collected character data */
#define XPL_AACS_OFFSET 0x11b       /* XML start behind an AACS header */

/* ---- element types ---- */

enum { STR, NUM, BOOL };

typedef struct AttrDef {
    const char *name;
    int         type;
    const char *str;    /* STR default */
    uint32_t    num;    /* NUM / BOOL default */
} AttrDef;

typedef struct ClassDef {
    const char    *name;
    const AttrDef *attrs;           /* terminated by name == NULL */
    const int     *kids;            /* accepted child types, terminated by -1 */
} ClassDef;

#define S(n, d)  { n, STR,  d, 0 }
#define N(n, d)  { n, NUM,  NULL, d }
#define B(n, d)  { n, BOOL, NULL, d }
#define END      { NULL }
#define KIDS(...) (const int[]){ __VA_ARGS__, -1 }
#define NO_KIDS  (const int[]){ -1 }
#define NO_ATTRS (const AttrDef[]){ END }

/* The clip types that share one attribute table; only sync's default differs. */
#define CLIP_ATTRS(sync) (const AttrDef[]){ \
    S("clipTimeBegin", "00:00:00:00"), S("dataSource", "P-Storage"), S("description", ""), \
    S("id", ""), B("noCache", 0), S("preload", ""), S("src", ""), S("sync", sync), \
    S("titleTimeBegin", ""), S("titleTimeEnd", ""), END }

/* Audio, SubAudio, Subtitle */
#define STREAM_ATTRS (const AttrDef[]){ \
    S("description", ""), S("mediaAttr", "1"), S("streamNumber", "1"), S("track", ""), END }

static const ClassDef classes[HDDVD_XPL_NB_CLASSES] = {
    [HDDVD_XPL_DOCUMENT] = { "(document)", NO_ATTRS, KIDS(HDDVD_XPL_PLAYLIST) },
    [HDDVD_XPL_PLAYLIST] = { "Playlist",
        (const AttrDef[]){ S("description", ""), S("displayName", ""), N("majorVersion", 0),
                           N("minorVersion", 0), S("type", "Advanced"), END },
        KIDS(HDDVD_XPL_CONFIGURATION, HDDVD_XPL_MEDIA_ATTRIBUTE_LIST, HDDVD_XPL_TITLE_SET) },
    [HDDVD_XPL_CONFIGURATION] = { "Configuration", NO_ATTRS,
        KIDS(HDDVD_XPL_APERTURE, HDDVD_XPL_MAIN_VIDEO_DEFAULT_COLOR, HDDVD_XPL_NETWORK_TIMEOUT,
             HDDVD_XPL_STREAMING_BUFFER) },
    [HDDVD_XPL_APERTURE] = { "Aperture", (const AttrDef[]){ S("size", ""), END }, NO_KIDS },
    [HDDVD_XPL_MAIN_VIDEO_DEFAULT_COLOR] = { "MainVideoDefaultColor",
        (const AttrDef[]){ S("color", ""), END }, NO_KIDS },
    [HDDVD_XPL_NETWORK_TIMEOUT] = { "NetworkTimeout", (const AttrDef[]){ N("timeout", 0), END }, NO_KIDS },
    [HDDVD_XPL_STREAMING_BUFFER] = { "StreamingBuffer", (const AttrDef[]){ S("size", ""), END }, NO_KIDS },
    [HDDVD_XPL_MEDIA_ATTRIBUTE_LIST] = { "MediaAttributeList", NO_ATTRS,
        KIDS(HDDVD_XPL_AUDIO_ATTRIBUTE_ITEM, HDDVD_XPL_SUBPICTURE_ATTRIBUTE_ITEM,
             HDDVD_XPL_VIDEO_ATTRIBUTE_ITEM) },
    [HDDVD_XPL_AUDIO_ATTRIBUTE_ITEM] = { "AudioAttributeItem",
        (const AttrDef[]){ S("bitrate", ""), S("channels", ""), S("codec", ""), S("index", ""),
                           S("sampleDepth", ""), S("sampleRate", ""), END }, NO_KIDS },
    [HDDVD_XPL_SUBPICTURE_ATTRIBUTE_ITEM] = { "SubpictureAttributeItem",
        (const AttrDef[]){ S("codec", ""), S("index", ""), END }, NO_KIDS },
    [HDDVD_XPL_VIDEO_ATTRIBUTE_ITEM] = { "VideoAttributeItem",
        (const AttrDef[]){ N("activeAreaX1", 0), N("activeAreaX2", 0), N("activeAreaY1", 0),
                           N("activeAreaY2", 0), S("bitrate", ""), S("codec", ""),
                           S("encodedFrameRate", ""), S("horizontalResolution", ""), S("index", ""),
                           S("sampleAspectRatio", ""), S("sourceFrameRate", ""),
                           S("verticalResolution", ""), END }, NO_KIDS },
    [HDDVD_XPL_TITLE_SET] = { "TitleSet",
        (const AttrDef[]){ S("defaultLanguage", ""), S("tickBase", ""), S("timeBase", ""), END },
        KIDS(HDDVD_XPL_FIRST_PLAY_TITLE, HDDVD_XPL_PLAYLIST_APPLICATION, HDDVD_XPL_TITLE) },
    [HDDVD_XPL_FIRST_PLAY_TITLE] = { "FirstPlayTitle",
        (const AttrDef[]){ S("alternativeSDDisplayMode", "panscanOrLetterbox"), S("base", ""),
                           S("titleDuration", ""), END },
        KIDS(HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, HDDVD_XPL_SUBSTITUTE_AUDIO_VIDEO_CLIP) },
    [HDDVD_XPL_PLAYLIST_APPLICATION] = { "PlaylistApplication",
        (const AttrDef[]){ S("description", ""), S("id", ""), S("language", ""), S("src", ""), END },
        KIDS(HDDVD_XPL_PLAYLIST_APPLICATION_RESOURCE) },
    [HDDVD_XPL_PLAYLIST_APPLICATION_RESOURCE] = { "PlaylistApplicationResource",
        (const AttrDef[]){ S("description", ""), S("multiplexed", ""), S("size", ""), S("src", ""), END },
        NO_KIDS },
    [HDDVD_XPL_TITLE] = { "Title",
        (const AttrDef[]){ S("alternativeSDDisplayMode", "panscanOrLetterbox"), S("base", ""),
                           S("description", ""), S("displayName", ""), S("id", ""), S("onEnd", ""),
                           S("parentalLevel", "*:1"), B("selectable", 1), S("tickBaseDivisor", "1"),
                           S("titleDuration", ""), S("titleNumber", ""), S("type", "Advanced"), END },
        KIDS(HDDVD_XPL_ADVANCED_SUBTITLE_SEGMENT, HDDVD_XPL_APPLICATION_SEGMENT, HDDVD_XPL_CHAPTER_LIST,
             HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP, HDDVD_XPL_SCHEDULED_CONTROL_LIST,
             HDDVD_XPL_SECONDARY_AUDIO_VIDEO_CLIP, HDDVD_XPL_SUBSTITUTE_AUDIO_CLIP,
             HDDVD_XPL_SUBSTITUTE_AUDIO_VIDEO_CLIP, HDDVD_XPL_TITLE_RESOURCE,
             HDDVD_XPL_TRACK_NAVIGATION_LIST) },
    [HDDVD_XPL_ADVANCED_SUBTITLE_SEGMENT] = { "AdvancedSubtitleSegment",
        (const AttrDef[]){ S("description", ""), S("id", ""), S("src", ""), S("sync", "hard"),
                           S("titleTimeBegin", ""), S("titleTimeEnd", ""), END },
        KIDS(HDDVD_XPL_APPLICATION_RESOURCE, HDDVD_XPL_SUBTITLE) },
    [HDDVD_XPL_APPLICATION_SEGMENT] = { "ApplicationSegment",
        (const AttrDef[]){ S("appBlock", ""), B("autorun", 1), S("description", ""), S("group", ""),
                           S("id", ""), S("language", ""), S("src", ""), S("sync", "hard"),
                           S("titleTimeBegin", ""), S("titleTimeEnd", ""), N("zOrder", 0), END },
        KIDS(HDDVD_XPL_APPLICATION_RESOURCE) },
    [HDDVD_XPL_APPLICATION_RESOURCE] = { "ApplicationResource",
        (const AttrDef[]){ S("description", ""), S("loadingBegin", ""), S("multiplexed", ""),
                           B("noCache", 0), N("priority", 0), S("size", ""), S("src", ""), END },
        KIDS(HDDVD_XPL_NETWORK_SOURCE) },
    [HDDVD_XPL_NETWORK_SOURCE] = { "NetworkSource",
        (const AttrDef[]){ N("networkThroughput", 0), S("src", ""), END }, NO_KIDS },
    [HDDVD_XPL_CHAPTER_LIST] = { "ChapterList", NO_ATTRS, KIDS(HDDVD_XPL_CHAPTER) },
    [HDDVD_XPL_CHAPTER] = { "Chapter",
        (const AttrDef[]){ S("description", ""), S("displayName", ""), S("id", ""),
                           S("titleTimeBegin", ""), END }, NO_KIDS },
    [HDDVD_XPL_PRIMARY_AUDIO_VIDEO_CLIP] = { "PrimaryAudioVideoClip",
        (const AttrDef[]){ S("clipTimeBegin", "00:00:00:00"), S("dataSource", "Disc"),
                           S("description", ""), S("id", ""), B("seamless", 0), S("src", ""),
                           S("titleTimeBegin", ""), S("titleTimeEnd", ""), END },
        KIDS(HDDVD_XPL_AUDIO, HDDVD_XPL_SUB_AUDIO, HDDVD_XPL_SUB_VIDEO, HDDVD_XPL_SUBTITLE,
             HDDVD_XPL_VIDEO) },
    [HDDVD_XPL_AUDIO]    = { "Audio",    STREAM_ATTRS, NO_KIDS },
    [HDDVD_XPL_SUB_AUDIO] = { "SubAudio", STREAM_ATTRS, NO_KIDS },
    [HDDVD_XPL_SUBTITLE] = { "Subtitle", STREAM_ATTRS, NO_KIDS },
    [HDDVD_XPL_SUB_VIDEO] = { "SubVideo",
        (const AttrDef[]){ S("description", ""), S("mediaAttr", "1"), S("track", ""), END }, NO_KIDS },
    [HDDVD_XPL_VIDEO] = { "Video",
        (const AttrDef[]){ S("angleNumber", "1"), S("description", ""), S("mediaAttr", "1"),
                           S("track", ""), END }, NO_KIDS },
    [HDDVD_XPL_SCHEDULED_CONTROL_LIST] = { "ScheduledControlList", NO_ATTRS,
        KIDS(HDDVD_XPL_EVENT, HDDVD_XPL_PAUSE_AT) },
    [HDDVD_XPL_EVENT]    = { "Event",   (const AttrDef[]){ S("id", ""), S("titleTime", ""), END }, NO_KIDS },
    [HDDVD_XPL_PAUSE_AT] = { "PauseAt", (const AttrDef[]){ S("id", ""), S("titleTime", ""), END }, NO_KIDS },
    [HDDVD_XPL_SECONDARY_AUDIO_VIDEO_CLIP] = { "SecondaryAudioVideoClip", CLIP_ATTRS("soft"),
        KIDS(HDDVD_XPL_NETWORK_SOURCE, HDDVD_XPL_SUB_AUDIO, HDDVD_XPL_SUB_VIDEO) },
    [HDDVD_XPL_SUBSTITUTE_AUDIO_CLIP] = { "SubstituteAudioClip", CLIP_ATTRS("soft"),
        KIDS(HDDVD_XPL_AUDIO, HDDVD_XPL_NETWORK_SOURCE) },
    [HDDVD_XPL_SUBSTITUTE_AUDIO_VIDEO_CLIP] = { "SubstituteAudioVideoClip", CLIP_ATTRS("hard"),
        KIDS(HDDVD_XPL_AUDIO, HDDVD_XPL_NETWORK_SOURCE, HDDVD_XPL_VIDEO) },
    [HDDVD_XPL_TITLE_RESOURCE] = { "TitleResource",
        (const AttrDef[]){ S("description", ""), S("loadingBegin", ""), S("multiplexed", ""),
                           B("noCache", 0), N("priority", 0), S("size", ""), S("src", ""),
                           S("titleTimeBegin", ""), S("titleTimeEnd", ""), END },
        KIDS(HDDVD_XPL_NETWORK_SOURCE) },
    [HDDVD_XPL_TRACK_NAVIGATION_LIST] = { "TrackNavigationList", NO_ATTRS,
        KIDS(HDDVD_XPL_AUDIO_TRACK, HDDVD_XPL_SUBTITLE_TRACK, HDDVD_XPL_VIDEO_TRACK) },
    [HDDVD_XPL_AUDIO_TRACK] = { "AudioTrack",
        (const AttrDef[]){ S("description", ""), S("langcode", ""), B("selectable", 1), S("track", ""), END },
        NO_KIDS },
    [HDDVD_XPL_SUBTITLE_TRACK] = { "SubtitleTrack",
        (const AttrDef[]){ S("description", ""), B("forced", 0), S("langcode", ""), B("selectable", 1),
                           S("track", ""), END }, NO_KIDS },
    [HDDVD_XPL_VIDEO_TRACK] = { "VideoTrack",
        (const AttrDef[]){ S("description", ""), B("selectable", 1), S("track", ""), END }, NO_KIDS },
};

const char *ff_hddvd_xpl_class_name(int cls)
{
    return cls >= 0 && cls < HDDVD_XPL_NB_CLASSES ? classes[cls].name : "(unknown)";
}

static int attr_index(int cls, const char *name)
{
    const AttrDef *a = classes[cls].attrs;
    for (int i = 0; a[i].name; i++)
        if (!strcmp(a[i].name, name))
            return i;
    return -1;
}

const char *ff_hddvd_xpl_str(const HDDVDXplNode *n, const char *name)
{
    int i = attr_index(n->cls, name);
    return i >= 0 && classes[n->cls].attrs[i].type == STR ? n->str[i] : NULL;
}

uint32_t ff_hddvd_xpl_num(const HDDVDXplNode *n, const char *name)
{
    int i = attr_index(n->cls, name);
    return i >= 0 && classes[n->cls].attrs[i].type != STR ? n->num[i] : 0;
}

HDDVDXplNode *ff_hddvd_xpl_child(const HDDVDXplNode *n, int cls, int i)
{
    for (int k = 0; k < n->nb_kids; k++)
        if (n->kids[k]->cls == cls && i-- == 0)
            return n->kids[k];
    return NULL;
}

int ff_hddvd_xpl_count(const HDDVDXplNode *n, int cls)
{
    int c = 0;
    for (int k = 0; k < n->nb_kids; k++)
        c += n->kids[k]->cls == cls;
    return c;
}

/* ---- the document ---- */

static void node_free(HDDVDXplNode *n)
{
    for (int k = 0; k < n->nb_kids; k++) {
        node_free(n->kids[k]);
        dr_free(n->kids[k]);
    }
    dr_freep(&n->kids);
}

void ff_hddvd_xpl_free(HDDVDXpl **pxpl)
{
    HDDVDXpl *x = *pxpl;

    if (!x)
        return;
    node_free(&x->root);
    for (int i = 0; i < x->nb_strings; i++)
        dr_free(x->strings[i]);
    dr_free(x->strings);
    dr_freep(pxpl);
}

void ff_hddvd_xpl_free_all(HDDVDXpl ***pxpls, int nb)
{
    HDDVDXpl **x = *pxpls;

    if (!x)
        return;
    for (int i = 0; i < nb; i++)
        ff_hddvd_xpl_free(&x[i]);
    dr_freep(pxpls);
}

/* ---- parsing ---- */

typedef struct ParseCtx {
    void         *log;
    HDDVDXpl     *xpl;
    HDDVDXplNode *cur;
    int           skip;                     /* depth inside a skipped element */
    char          text[XPL_TEXT_MAX + 1];
    int           text_len;
    int           nomem;
} ParseCtx;

static const char *keep(ParseCtx *c, const char *s, size_t len)
{
    HDDVDXpl *x = c->xpl;
    char *d;

    if (c->nomem)
        return NULL;
    if (dr_array_add_ptr(&x->strings, &x->nb_strings, NULL) < 0 ||
        !(d = dr_strndup(s, len))) {
        c->nomem = 1;
        return NULL;
    }
    x->strings[x->nb_strings - 1] = d;
    return d;
}

static const char *local_name(const char *name)
{
    const char *colon = strchr(name, ':');
    return colon ? colon + 1 : name;
}

static HDDVDXplNode *create_child(ParseCtx *c, HDDVDXplNode *parent, const char *name)
{
    const int *kids = classes[parent->cls].kids;
    HDDVDXplNode *n;

    for (int k = 0; kids[k] >= 0; k++) {
        if (strcmp(classes[kids[k]].name, name))
            continue;
        if (parent->cls == HDDVD_XPL_DOCUMENT)
            return &c->xpl->root;           /* the root is part of the document */
        if (!(n = dr_mallocz(sizeof(*n))) ||
            dr_array_add_ptr(&parent->kids, &parent->nb_kids, n) < 0) {
            dr_free(n);
            c->nomem = 1;
            return NULL;
        }
        n->cls = kids[k];
        return n;
    }
    return NULL;
}

static void XMLCALL start_element(void *opaque, const XML_Char *name, const XML_Char **atts)
{
    ParseCtx *c = opaque;
    const AttrDef *defs;
    HDDVDXplNode *n;

    c->text_len = 0;
    if (c->skip) {
        c->skip++;
        return;
    }
    if (!(n = create_child(c, c->cur, local_name(name)))) {
        dr_vlog(c->log, DR_VLOG_DEBUG, "XPL: <%s> in <%s> is not read\n", name, classes[c->cur->cls].name);
        c->skip++;
        return;
    }
    defs = classes[n->cls].attrs;
    for (int i = 0; defs[i].name; i++) {
        n->str[i] = defs[i].type == STR ? defs[i].str : NULL;
        n->num[i] = defs[i].num;
    }
    n->parent = c->cur;
    n->text   = NULL;
    for (int k = 0; atts && atts[2 * k]; k++) {
        const char *v = atts[2 * k + 1];
        int i = attr_index(n->cls, local_name(atts[2 * k]));
        if (i < 0)
            continue;
        switch (defs[i].type) {
        case STR:  n->str[i] = keep(c, v, strlen(v));                               break;
        case NUM:  n->num[i] = (uint32_t)strtoul(v, NULL, 10);                       break;
        case BOOL: n->num[i] = !strcasecmp(v, "true") || !strcasecmp(v, "yes");      break;
        }
    }
    c->cur = n;
}

static void XMLCALL end_element(void *opaque, const XML_Char *name)
{
    ParseCtx *c = opaque;

    if (c->skip) {
        c->skip--;
        return;
    }
    if (c->text_len)                        /* not emptied: see the file header */
        c->cur->text = keep(c, c->text, c->text_len);
    c->cur = c->cur->parent;
}

static void XMLCALL character_data(void *opaque, const XML_Char *s, int len)
{
    ParseCtx *c = opaque;
    int n = DR_MIN(len, XPL_TEXT_MAX - c->text_len);

    if (n > 0) {
        memcpy(c->text + c->text_len, s, n);
        c->text_len += n;
    }
}

static void XMLCALL default_handler(void *opaque, const XML_Char *s, int len)
{
    /* markup without a handler of its own (declaration, comments, ...) */
}

static int xml_error(void *logctx, XML_Parser p)
{
    enum XML_Error e = XML_GetErrorCode(p);

    dr_vlog(logctx, DR_VLOG_ERROR, "XPL: not well-formed XML: expat error %d (%s) at line %llu, column %llu\n",
           (int)e, XML_ErrorString(e), (unsigned long long)XML_GetCurrentLineNumber64(p),
           (unsigned long long)XML_GetCurrentColumnNumber64(p));
    return DR_ERR_INVALID;
}

int ff_hddvd_xpl_parse(void *logctx, HDDVDReadFn read, void *opaque,
                       int64_t offset, int64_t length, HDDVDXpl **out)
{
    ParseCtx *c;
    XML_Parser p;
    uint8_t *buf;
    int ret = 0;

    *out = NULL;
    c   = dr_mallocz(sizeof(*c));
    buf = dr_malloc(XPL_CHUNK);
    if (!c || !buf || !(c->xpl = dr_mallocz(sizeof(*c->xpl))) || !(p = XML_ParserCreate(NULL))) {
        if (c)
            dr_free(c->xpl);
        dr_free(c);
        dr_free(buf);
        return DR_ERR(ENOMEM);
    }
    c->log          = logctx;
    c->xpl->doc.cls = HDDVD_XPL_DOCUMENT;
    c->xpl->root.cls = HDDVD_XPL_PLAYLIST;  /* empty until <Playlist> starts */
    c->cur          = &c->xpl->doc;
    XML_SetUserData(p, c);
    XML_SetElementHandler(p, start_element, end_element);
    XML_SetCharacterDataHandler(p, character_data);
    XML_SetDefaultHandlerExpand(p, default_handler);

    while (length > 0 && !c->nomem) {
        int n = DR_MIN(length, XPL_CHUNK);
        if (offset % 2048)
            n = DR_MIN(n, 2048 - offset % 2048);
        if ((ret = read(opaque, offset, buf, n)) < 0) {
            dr_vlog(logctx, DR_VLOG_ERROR, "XPL: cannot read %d bytes at %"PRId64": %s\n",
                   n, offset, dr_err_text(ret));
            goto done;
        }
        if (XML_Parse(p, (const char *)buf, n, 0) != XML_STATUS_OK) {
            ret = xml_error(logctx, p);
            goto done;
        }
        offset += n;
        length -= n;
    }
    if (!c->nomem && XML_Parse(p, NULL, 0, 1) != XML_STATUS_OK)
        ret = xml_error(logctx, p);
done:
    XML_ParserFree(p);
    dr_free(buf);
    if (ret >= 0 && c->nomem)
        ret = DR_ERR(ENOMEM);
    if (ret < 0)
        ff_hddvd_xpl_free(&c->xpl);
    else
        *out = c->xpl;
    dr_free(c);
    return ret < 0 ? ret : 0;
}

/* ---- the playlist files ---- */

typedef struct FileReader {
    DiscIOFS   *fs;
    DiscIOFile *file;
} FileReader;

static int file_read(void *opaque, int64_t pos, uint8_t *buf, int len)
{
    FileReader *r = opaque;
    return ff_discio_file_read(r->fs, r->file, pos, buf, len);
}

/* One file; < 0 = left out (logged), ENOMEM = stop. */
static int load_one(void *logctx, FileReader *r, const char *path, HDDVDXpl **out)
{
    uint8_t h[11];
    int64_t offset = 0, length = r->file->size;
    int ret;

    if ((ret = file_read(r, 0, h, 4)) < 0) {
        dr_vlog(logctx, DR_VLOG_WARNING, "%s: cannot read its first bytes; playlist left out\n", path);
        return ret;
    }
    if (!memcmp(h, "AACS", 4)) {
        if ((ret = file_read(r, 0, h, 11)) < 0) {
            dr_vlog(logctx, DR_VLOG_WARNING, "%s: cannot read its AACS header; playlist left out\n", path);
            return ret;
        }
        if (h[4] != 0x02 && h[4] != 0x12 && h[4] != 0x21) {
            dr_vlog(logctx, DR_VLOG_WARNING, "%s: AACS header type 0x%02x (0x02, 0x12, 0x21); "
                   "playlist left out\n", path, h[4]);
            return DR_ERR_INVALID;
        }
        offset = XPL_AACS_OFFSET;
        length = dr_rb32(h + 7);
        dr_vlog(logctx, DR_VLOG_VERBOSE, "%s: AACS header type 0x%02x, %"PRId64" bytes of XML at %"PRId64"\n",
               path, h[4], length, offset);
    }
    if ((ret = ff_hddvd_xpl_parse(logctx, file_read, r, offset, length, out)) < 0 && ret != DR_ERR(ENOMEM))
        dr_vlog(logctx, DR_VLOG_WARNING, "%s: playlist left out\n", path);
    return ret;
}

int ff_hddvd_xpl_load(void *logctx, DiscIOFS *fs, HDDVDXpl ***out, int *nb_out)
{
    HDDVDXpl **xpls = NULL;
    int nb = 0, ret = 0;

    *out = NULL;
    *nb_out = 0;
    for (int i = 0; i < HDDVD_XPL_MAX_FILES; i++) {
        FileReader r = { fs, NULL };
        HDDVDXpl *x = NULL;
        char path[32];

        snprintf(path, sizeof(path), "/ADV_OBJ/VPLST%03d.XPL", i);
        if ((ret = fs->ops->open_file(fs, path, &r.file)) < 0) {
            if (ret != DR_ERR(ENOENT))
                dr_vlog(logctx, DR_VLOG_WARNING, "%s: cannot open it (%s); no further playlists read\n",
                       path, dr_err_text(ret));
            break;
        }
        ret = load_one(logctx, &r, path, &x);
        ff_discio_file_free(&r.file);
        if (ret == DR_ERR(ENOMEM))
            goto fail;
        if (ret < 0)
            continue;
        x->file = i;
        if ((ret = dr_array_add_ptr(&xpls, &nb, x)) < 0) {
            ff_hddvd_xpl_free(&x);
            goto fail;
        }
        dr_vlog(logctx, DR_VLOG_VERBOSE, "%s: playlist %d\n", path, nb - 1);
    }
    if (!nb) {
        dr_vlog(logctx, DR_VLOG_ERROR, "No playlist (/ADV_OBJ/VPLST000.XPL ...) could be read\n");
        return DR_ERR_INVALID;
    }
    *out = xpls;
    *nb_out = nb;
    return 0;
fail:
    ff_hddvd_xpl_free_all(&xpls, nb);
    return DR_ERR(ENOMEM);
}

/* ---- dump ---- */

static void dump_node(DrStr *bp, const HDDVDXplNode *n, int depth)
{
    const AttrDef *a = classes[n->cls].attrs;

    dr_str_catf(bp, "%*s%s", 2 * depth, "", classes[n->cls].name);
    for (int i = 0; a[i].name; i++) {
        if (a[i].type != STR)
            dr_str_catf(bp, " %s=%"PRIu32, a[i].name, n->num[i]);
        else if (n->str[i])
            dr_str_catf(bp, " %s=\"%s\"", a[i].name, n->str[i]);
        else
            dr_str_catf(bp, " %s=(null)", a[i].name);
    }
    if (n->text)
        dr_str_catf(bp, " text=\"%s\"", n->text);
    dr_str_catf(bp, "\n");
    for (int k = 0; k < n->nb_kids; k++)
        dump_node(bp, n->kids[k], depth + 1);
}

char *ff_hddvd_xpl_dump(const HDDVDXpl *x)
{
    DrStr bp;

    dr_str_init(&bp);
    dump_node(&bp, &x->root, 0);
    return dr_str_take(&bp, 1);
}
