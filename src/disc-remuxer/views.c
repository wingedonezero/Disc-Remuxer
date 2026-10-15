/*
 * What the front ends show of a disc, in the engine's words (views.h).
 */

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "base/mem.h"
#include "timeline/disctl.h"
#include "codecs.h"
#include "engine.h"
#include "views.h"

/* ---- rows ---- */

static int add(EngineRows *r, EngineRowKind kind, int level, const char *label, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static int add(EngineRows *r, EngineRowKind kind, int level, const char *label, const char *fmt, ...)
{
    EngineRow *row;
    va_list vl;

    if (r->nb == r->alloc) {
        int alloc = r->alloc ? 2 * r->alloc : 32;
        EngineRow *n = dr_realloc_array(r->rows, alloc, sizeof(*n));
        if (!n)
            return -1;
        r->rows  = n;
        r->alloc = alloc;
    }
    row = &r->rows[r->nb];
    row->kind  = kind;
    row->level = level;
    row->value = NULL;
    if (!(row->label = dr_strdup(label)))
        return -1;
    if (fmt) {
        va_start(vl, fmt);
        if (vasprintf(&row->value, fmt, vl) < 0)
            row->value = NULL;
        va_end(vl);
        if (!row->value) {
            dr_free(row->label);
            return -1;
        }
    }
    r->nb++;
    return 0;
}

#define HEAD(r, ...)  head_f(r, ENGINE_ROW_HEAD, 0, __VA_ARGS__)
#define SUB(r, l, ...) head_f(r, ENGINE_ROW_SUB, l, __VA_ARGS__)

static int head_f(EngineRows *r, EngineRowKind kind, int level, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int head_f(EngineRows *r, EngineRowKind kind, int level, const char *fmt, ...)
{
    char text[1024];
    va_list vl;

    va_start(vl, fmt);
    vsnprintf(text, sizeof(text), fmt, vl);
    va_end(vl);
    return add(r, kind, level, text, NULL);
}

/* a field: label: value */
#define FIELD(r, level, label, ...) add(r, ENGINE_ROW_FIELD, level, label, __VA_ARGS__)

void engine_rows_free(EngineRows *r)
{
    for (int i = 0; i < r->nb; i++) {
        dr_free(r->rows[i].label);
        dr_free(r->rows[i].value);
    }
    dr_freep(&r->rows);
    r->nb = r->alloc = 0;
}

/* ---- words ---- */

const char *engine_hms(int64_t us, char *buf, size_t size)
{
    int64_t t = (us + 500000) / 1000000;
    snprintf(buf, size, "%"PRId64":%02d:%02d", t / 3600, (int)(t / 60 % 60), (int)(t % 60));
    return buf;
}

const char *engine_hms_ms(int64_t us, char *buf, size_t size)
{
    int64_t ms = (us + 500) / 1000;
    snprintf(buf, size, "%"PRId64":%02d:%02d.%03d", ms / 3600000, (int)(ms / 60000 % 60), (int)(ms / 1000 % 60),
             (int)(ms % 1000));
    return buf;
}

const char *engine_size(int64_t bytes, char *buf, size_t size)
{
    int gb = bytes >= (1LL << 30);
    int64_t tenths = bytes * 10 / (gb ? 1LL << 30 : 1LL << 20);

    snprintf(buf, size, "%"PRId64".%"PRId64" %s", tenths / 10, tenths % 10, gb ? "GB" : "MB");
    return buf;
}

/* digits with a comma between thousands (and a decimal part kept) */
static const char *grouped(const char *num, char *buf, size_t size)
{
    const char *dot = strchr(num, '.');
    size_t ints = dot ? (size_t)(dot - num) : strlen(num), at = 0, lead = *num == '-';

    for (size_t i = 0; i < ints && at + 2 < size; i++) {
        if (i > lead && (ints - i) % 3 == 0)
            buf[at++] = ',';
        buf[at++] = num[i];
    }
    snprintf(buf + at, size - at, "%s", dot ? dot : "");
    return buf;
}

/* "2.3 GB (2,345.67 MB, 2,459,893,760 bytes)" */
static const char *bytes_text(int64_t bytes, char *buf, size_t size)
{
    char s[32], mb[64], gmb[80], b[32], gb[48];

    snprintf(mb, sizeof(mb), "%.2f", bytes / 1048576.0);
    snprintf(b, sizeof(b), "%"PRId64, bytes);
    snprintf(buf, size, "%s (%s MB, %s bytes)", engine_size(bytes, s, sizeof(s)), grouped(mb, gmb, sizeof(gmb)),
             grouped(b, gb, sizeof(gb)));
    return buf;
}

/* "eng (English)", "eng", or "none" */
static const char *language_text(const char *code, char *buf, size_t size)
{
    const char *name;

    if (!code || !*code)
        return "none";
    name = disc_language_name(code);
    if (name)
        snprintf(buf, size, "%s (%s)", code, name);
    else
        snprintf(buf, size, "%s", code);
    return buf;
}

static const char *kind_text(const DiscStreamInfo *k)
{
    switch (k->type) {
    case DISC_MEDIA_VIDEO:    return "Video";
    case DISC_MEDIA_AUDIO:    return "Audio";
    case DISC_MEDIA_SUBTITLE: return "Subtitle";
    default:                  return "Other";
    }
}

/* what a track's file is made of */
static const char *conversion_text(const DiscStreamInfo *k)
{
    if (k->second_view)
        return "the 3-D second view (MVC), as on the disc";
    switch (k->codec) {
    case DR_CODEC_PCM_DVD:
    case DR_CODEC_PCM_BLURAY:   return "LPCM written as PCM in a WAVE file (lossless)";
    case DR_CODEC_EIA_608:      return "closed captions made into SRT text";
    case DR_CODEC_DVD_SUBTITLE: return "VobSub (.sub with its .idx), as on the disc";
    default:                       return "as on the disc (copied, lossless)";
    }
}

/* remove spaces at both ends, in place */
static char *trimmed(char *s)
{
    size_t n;

    while (*s == ' ')
        memmove(s, s + 1, strlen(s));
    n = strlen(s);
    while (n && s[n - 1] == ' ')
        s[--n] = 0;
    return s;
}

/* ---- the details ---- */

int engine_disc_rows(const DiscInfo *d, const char *source, EngineRows *r)
{
    char fs[96], lt[128];
    int protection = 0;

    if (HEAD(r, "Disc information") < 0 || FIELD(r, 0, "Source", "%s", source) < 0 ||
        FIELD(r, 0, "Type", "%s", d->kind) < 0 ||
        (d->label && FIELD(r, 0, "Volume name", "%s", d->label) < 0) ||
        (d->name && FIELD(r, 0, "Name", "%s", d->name) < 0))
        return -1;
    snprintf(fs, sizeof(fs), "%s", d->filesystem ? d->filesystem : "?");
    if (d->udf_revision)
        snprintf(fs + strlen(fs), sizeof(fs) - strlen(fs), " %x.%02x", (unsigned)(d->udf_revision >> 8),
                 (unsigned)(d->udf_revision & 0xff));
    if (FIELD(r, 0, "File system", "%s", fs) < 0 || FIELD(r, 0, "Encrypted", "%s", d->encrypted ? "yes" : "no") < 0 ||
        FIELD(r, 0, "Titles found", "%d", d->nb_titles) < 0 ||
        (d->menu_name && FIELD(r, 0, "Menu name", "%s (the project name in the disc's menu data)", d->menu_name) < 0))
        return -1;
    /* the format part's own details; the protection ones in a block of their own */
    for (int p = 0; p < d->nb_props; p++) {
        const DiscPropertyInfo *q = &d->props[p];
        if (disc_prop_group(q->key) != DISC_PROP_PROTECTION &&
            FIELD(r, 0, q->name, "%s", disc_prop_language(q->key) ? language_text(q->value, lt, sizeof(lt)) :
                                       q->value) < 0)
            return -1;
    }
    for (int p = 0; p < d->nb_props; p++) {
        const DiscPropertyInfo *q = &d->props[p];
        if (disc_prop_group(q->key) != DISC_PROP_PROTECTION)
            continue;
        if (!protection && HEAD(r, "Protection") < 0)
            return -1;
        protection = 1;
        if (FIELD(r, 0, disc_prop_label(q->key), "%s", q->value) < 0)
            return -1;
    }
    return 0;
}

static int track_rows(const DiscTitleInfo *t, int i, const uint8_t *keep, EngineRows *r, int level)
{
    const DiscStreamInfo *k = &t->streams[i];
    char codec[96], rest[512], buf[96], lt[128], flags[1024] = "";
    const char *mkv = disc_mkv_codec_id(k);
    static const struct { int bit; const char *word; } roles[] = {
        { DISC_ROLE_COMMENTARY, "commentary" }, { DISC_ROLE_ALT_COMMENTARY, "alternate commentary" },
        { DISC_ROLE_VISUALLY_IMPAIRED, "for the visually impaired" }, { DISC_ROLE_LARGE, "large" },
        { DISC_ROLE_CHILDREN, "for children" }, { DISC_ROLE_FORCED, "forced subtitles" },
    };

    engine_track_parts(t, i, codec, sizeof(codec), rest, sizeof(rest));
    {
        char head[1024];
        snprintf(head, sizeof(head), "Track #%d: %s %s%s", i, kind_text(k), codec, rest);
        if (SUB(r, level, "%s", trimmed(head)) < 0)
            return -1;
    }
    level++;
    if (FIELD(r, level, "Kind", "%s", kind_text(k)) < 0 || FIELD(r, level, "Codec (full)", "%s", codec) < 0 ||
        FIELD(r, level, "Codec (short)", "%s", codec_short(k)) < 0 ||
        (mkv && FIELD(r, level, "Codec ID", "%s", mkv) < 0) ||
        (*codec_profile_level(k, buf, sizeof(buf)) && FIELD(r, level, "Profile", "%s", buf) < 0) ||
        FIELD(r, level, "Language", "%s", language_text(k->language, lt, sizeof(lt))) < 0 ||
        (*audio_name(k, buf, sizeof(buf)) && FIELD(r, level, "Name", "%s", buf) < 0))
        return -1;
    /* TrueHD / MLP: variable; the peak rate their header states reads far too
     * low on real discs (look into again later), so none is shown. DTS-HD:
     * the rate is its DTS core's. */
    if (k->codec == DR_CODEC_TRUEHD || k->codec == DR_CODEC_MLP) {
        if (FIELD(r, level, "Bit rate", "variable (lossless)") < 0)
            return -1;
    } else if (k->bit_rate && *bit_rate_text(k->bit_rate, buf, sizeof(buf))) {
        if (FIELD(r, level, "Bit rate", "%s%s", buf, k->codec == DR_CODEC_DTS && k->inner_track >= 0 ?
                  " (its DTS core's)" : "") < 0)
            return -1;
    }
    if (k->type == DISC_MEDIA_VIDEO) {
        if ((k->width && FIELD(r, level, "Video size", "%dx%d", k->width, k->height) < 0) ||
            (k->aspect.num > 0 && *aspect_text(k, buf, sizeof(buf)) && FIELD(r, level, "Aspect ratio", "%s", buf) < 0) ||
            (k->frame_rate.num > 0 && *frame_rate_text(k, buf, sizeof(buf)) &&
             FIELD(r, level, "Frame rate", "%s", buf) < 0) ||
            (k->hdr[0] && FIELD(r, level, "HDR", "%s%s", k->hdr, k->dolby_vision[0] ?
                                " (the base layer), with Dolby Vision" : "") < 0) ||
            (k->dolby_vision[0] && FIELD(r, level, "Dolby Vision", "%s", k->dolby_vision) < 0) ||
            (k->colour[0] && FIELD(r, level, "Colour", "%s", k->colour) < 0) ||
            (k->mastering[0] && FIELD(r, level, "Mastering display", "%s", k->mastering) < 0) ||
            ((k->max_cll || k->max_fall) &&
             FIELD(r, level, "Content light level", "MaxCLL %d, MaxFALL %d cd/m2", (int)k->max_cll,
                   (int)k->max_fall) < 0))
            return -1;
    }
    if (k->type == DISC_MEDIA_AUDIO) {
        if ((k->channels && FIELD(r, level, "Channels", "%d", k->channels) < 0) ||
            (k->layout && *layout_text(k, buf, sizeof(buf)) && FIELD(r, level, "Channel layout", "%s", buf) < 0) ||
            (k->sample_rate && FIELD(r, level, "Sample rate", "%d Hz", k->sample_rate) < 0) ||
            (k->bits && FIELD(r, level, "Sample size", "%d bits", k->bits) < 0))
            return -1;
    }
    if (k->type == DISC_MEDIA_SUBTITLE && k->width &&
        FIELD(r, level, "Canvas", "%dx%d", k->width, k->height) < 0)
        return -1;
    /* what the disc says of it and how it is linked to other tracks */
#define FLAG(...) snprintf(flags + strlen(flags), sizeof(flags) - strlen(flags), "%s", *flags ? "; " : ""), \
                  snprintf(flags + strlen(flags), sizeof(flags) - strlen(flags), __VA_ARGS__)
    for (size_t n = 0; n < sizeof(roles) / sizeof(*roles); n++)
        if (k->roles & roles[n].bit)
            FLAG("%s", roles[n].word);
    if (k->forced)
        FLAG("the forced subtitles of track %d", k->from_track);
    else if (k->second_view)
        FLAG("the 3-D second view of track %d", k->from_track);
    else if (k->from_track >= 0)
        FLAG("taken out of track %d (its core)", k->from_track);
    if (k->inner_track >= 0)
        FLAG("has a core: track %d is taken out of it", k->inner_track);
    if (k->second_layer)
        FLAG("a second layer is read with it (Dolby Vision enhancement layer)");
    if (k->offset_sequence >= 0)
        FLAG("3-D offset sequence %d", k->offset_sequence);
#undef FLAG
    if ((*flags && FIELD(r, level, "Flags", "%s", flags) < 0) ||
        FIELD(r, level, "Output", ".%s: %s", engine_track_extension(k), conversion_text(k)) < 0 ||
        FIELD(r, level, "Ripped", "%s", i == 0 ? "yes (the video is always ripped)" : keep[i] ? "yes" :
                                          "no (unchecked)") < 0)
        return -1;
    return 0;
}

static int chapter_list(const DiscTitleInfo *t, EngineRows *r)
{
    if (HEAD(r, "Chapters (%d, as the disc marks them)", t->nb_chapters) < 0)
        return -1;
    for (int i = 0; i < t->nb_chapters; i++) {
        char label[32], tm[48];
        snprintf(label, sizeof(label), "Chapter %d", i + 1);
        if (FIELD(r, 0, label, "%s%s%s", engine_hms_ms(t->chapters[i].start, tm, sizeof(tm)),
                  t->chapters[i].name ? "  " : "", t->chapters[i].name ? t->chapters[i].name : "") < 0)
            return -1;
    }
    return 0;
}

int engine_title_rows(const DiscTitleInfo *t, const uint8_t *keep, EngineRows *r)
{
    char name[512], tm[48], bt[160], lt[128];

    engine_title_file_name(t, name, sizeof(name));
    if (HEAD(r, "Title information: #%d", t->index) < 0 ||
        (t->name && FIELD(r, 0, "Name", "%s", t->name) < 0) ||
        (t->menu_name && FIELD(r, 0, "Menu name", "%s (the name the disc's menu data gives it)", t->menu_name) < 0) ||
        FIELD(r, 0, "Duration", "%s", engine_hms(t->duration, tm, sizeof(tm))) < 0 ||
        FIELD(r, 0, "Size", "%s", bytes_text(t->size, bt, sizeof(bt))) < 0 ||
        FIELD(r, 0, "Chapters", "%d", t->nb_chapters) < 0 ||
        (t->source && FIELD(r, 0, "Source", "%s", t->source) < 0))
        return -1;
    if (t->nb_files) {
        char files[4096] = "";
        for (int i = 0; i < t->nb_files; i++)
            snprintf(files + strlen(files), sizeof(files) - strlen(files), "%s%s", i ? ", " : "", t->files[i]);
        if (FIELD(r, 0, "Segments", "%d", t->nb_files) < 0 || FIELD(r, 0, "Segment map", "%s", files) < 0)
            return -1;
    }
    if (t->angle) {
        if (FIELD(r, 0, "Angle", "%d", t->angle) < 0)
            return -1;
        if (t->main_angle == t->angle) {
            if (FIELD(r, 0, "Main angle", "%d (this title)", t->main_angle) < 0)
                return -1;
        } else if (t->main_angle) {
            if ((t->main_title >= 0 ? FIELD(r, 0, "Main angle", "%d (title #%d)", t->main_angle, t->main_title)
                                    : FIELD(r, 0, "Main angle", "%d", t->main_angle)) < 0)
                return -1;
        }
        if (t->angle_order[0] && t->angle_order[1]) {
            char order[64] = "";
            for (int k = 0; k < 9 && t->angle_order[k]; k++)
                snprintf(order + strlen(order), sizeof(order) - strlen(order), "%s%d", k ? ", " : "",
                         t->angle_order[k]);
            if (FIELD(r, 0, "Angle order", "%s (as the disc plays them)", order) < 0)
                return -1;
        }
        if (t->shared_cells && FIELD(r, 0, "Shared cells", "%s (all angles play them)", t->shared_cells) < 0)
            return -1;
    }
    if (FIELD(r, 0, "Language", "%s", language_text(t->language, lt, sizeof(lt))) < 0 ||
        FIELD(r, 0, "Output", "%s/%s_<track>_…  (as parts: %s_p1/%s_p1_<track>_…, one folder per part)",
              name, name, name, name) < 0 ||
        FIELD(r, 0, "Ripped with all", "%s", t->selected ? "yes" : "no: not the main angle (tick it to rip it)") < 0)
        return -1;
    for (int p = 0; p < t->nb_props; p++)
        if (FIELD(r, 0, t->props[p].name, "%s", t->props[p].value) < 0)
            return -1;
    if (t->nb_parts) {
        if (HEAD(r, "Parts (%d)", t->nb_parts) < 0)
            return -1;
        for (int p = 0; p < t->nb_parts; p++) {
            const DiscPartInfo *q = &t->parts[p];
            char label[24], files[2048] = "", st[48], du[48];
            for (int f = 0; f < q->nb_files; f++)
                snprintf(files + strlen(files), sizeof(files) - strlen(files), "%s%s", f ? ", " : "",
                         t->files[q->first_file + f]);
            snprintf(label, sizeof(label), "Part %d", p + 1);
            if (FIELD(r, 0, label, "at %s, %s, %s, %d chapter(s)", engine_hms_ms(q->start, st, sizeof(st)),
                      engine_hms(q->duration, du, sizeof(du)), files, q->nb_chapters) < 0)
                return -1;
        }
    }
    /* the tracks by kind */
    for (int type = 0; type < 3; type++) {
        static const int types[3] = { DISC_MEDIA_VIDEO, DISC_MEDIA_AUDIO, DISC_MEDIA_SUBTITLE };
        int any = 0;
        for (int i = 0; i < t->nb_streams; i++) {
            if (t->streams[i].type != types[type])
                continue;
            if (!any && HEAD(r, "%s", type == 0 ? "Video" : type == 1 ? "Audio" : "Subtitles") < 0)
                return -1;
            any = 1;
            if (track_rows(t, i, keep, r, 0) < 0)
                return -1;
        }
    }
    return chapter_list(t, r);
}

int engine_track_rows(const DiscTitleInfo *t, int i, const uint8_t *keep, EngineRows *r)
{
    if (HEAD(r, "Track information (title #%d)", t->index) < 0)
        return -1;
    return track_rows(t, i, keep, r, 0);
}

int engine_chapters_rows(const DiscTitleInfo *t, EngineRows *r)
{
    if (t->name ? HEAD(r, "Title #%d: %s", t->index, t->name) < 0 : HEAD(r, "Title #%d", t->index) < 0)
        return -1;
    return chapter_list(t, r);
}

/* ---- the queue's lines ---- */

int engine_disc_line(const DiscInfo *d, char *desc, size_t dsize, char *state, size_t ssize, char *why, size_t wsize)
{
    char notes[256] = "";

    snprintf(desc, dsize, "%s", d->kind);
    if (d->name || d->label)
        snprintf(desc + strlen(desc), dsize - strlen(desc), "  %s", d->name ? d->name : d->label);
    why[0] = 0;
    if (d->bdplus) {
        snprintf(notes, sizeof(notes), "BD+");
        snprintf(why, wsize, "The disc uses BD+, which is not supported: unless this copy was made with BD+ "
                 "removed, parts of the video may be damaged.");
    }
    if (d->fake_playlists) {
        snprintf(notes + strlen(notes), sizeof(notes) - strlen(notes), "%s%d fake playlists", *notes ? ", " : "",
                 d->fake_playlists);
        snprintf(why + strlen(why), wsize - strlen(why), "%s%d playlists are likely fakes that hide the main "
                 "feature; the titles marked likely real are the ones to rip.", *why ? "\n" : "", d->fake_playlists);
    } else if (d->fakes_suspected) {
        snprintf(notes + strlen(notes), sizeof(notes) - strlen(notes), "%spossibly fake playlists",
                 *notes ? ", " : "");
        snprintf(why + strlen(why), wsize - strlen(why), "%sMany long playlists are made from the same clips in "
                 "different orders: possibly fakes of a kind not recognised (see the log).", *why ? "\n" : "");
    }
    snprintf(state, ssize, "%d title%s%s%s", d->nb_titles, d->nb_titles == 1 ? "" : "s", *notes ? " – " : "",
             notes);
    return *notes != 0;
}

void engine_title_line(const DiscTitleInfo *t, char *name, size_t nsize, char *desc, size_t dsize)
{
    char made[256], tm[48], nm[1024] = "";

    snprintf(name, nsize, "Title #%d", t->index);
    snprintf(made, sizeof(made), "%d chapter%s, %s", t->nb_chapters, t->nb_chapters == 1 ? "" : "s",
             engine_hms(t->duration, tm, sizeof(tm)));
    if (t->nb_files > 1)
        snprintf(made + strlen(made), sizeof(made) - strlen(made), ", %d segments", t->nb_files);
    if (t->angle)
        snprintf(made + strlen(made), sizeof(made) - strlen(made), ", angle %d%s", t->angle,
                 t->angle == t->main_angle ? " (main)" : "");
    if (t->nb_parts)
        snprintf(made + strlen(made), sizeof(made) - strlen(made), ", %d parts", t->nb_parts);
    if (t->name && *t->name)
        snprintf(nm, sizeof(nm), "%s", t->name);
    if (t->menu_name)               /* the name the disc's menu gives it, after the title's name */
        snprintf(nm + strlen(nm), sizeof(nm) - strlen(nm), "%s%s]", *nm ? " [" : "[", t->menu_name);
    snprintf(desc, dsize, "%s%s%s", nm, *nm ? " – " : "", made);
    if (t->source)
        snprintf(desc + strlen(desc), dsize - strlen(desc), " (%s)", t->source);
}

int engine_title_state(const DiscTitleInfo *t, int split, char *state, size_t ssize, char *why, size_t wsize)
{
    const DiscPropertyInfo *p;

    state[0] = why[0] = 0;
    if (!t->selected)
        snprintf(state, ssize, "not the main angle");
    if (split)
        snprintf(state, ssize, "written as %d parts", t->nb_parts);
    if (t->playlist_check != DISC_PLAYLIST_NOT_MARKED) {
        char rest[256];
        snprintf(rest, sizeof(rest), "%s", state);
        snprintf(state, ssize, "%s%s%s", t->playlist_check == DISC_PLAYLIST_LIKELY_FAKE ? "likely fake" :
                 "likely real", *rest ? "; " : "", rest);
        if ((p = disc_prop_find(t->props, t->nb_props, DISC_PROP_FAKE_PLAYLIST_CHECK)))
            snprintf(why, wsize, "%s", p->value);
    }
    return t->playlist_check;
}

void engine_track_line(const DiscTitleInfo *t, int i, char *name, size_t nsize, char *desc, size_t dsize)
{
    const DiscStreamInfo *k = &t->streams[i];
    char codec[96], body[512], link[96], an[32], more[1024], tmp[1200];
    const char *lang = k->language[0] ? disc_language_name(k->language) : NULL;

    snprintf(name, nsize, "%s #%d", kind_text(k), i);
    engine_track_pieces(t, i, codec, sizeof(codec), body, sizeof(body), link, sizeof(link));
    /* how it is linked to another track the queue shows by where the row
     * is: a short word instead */
    snprintf(more, sizeof(more), "%s", body);
    trimmed(more);
    if (k->forced)
        snprintf(more + strlen(more), sizeof(more) - strlen(more), "  forced only");
    else if (k->second_view)
        snprintf(more + strlen(more), sizeof(more) - strlen(more), "  second view");
    else if (k->from_track >= 0)
        snprintf(more + strlen(more), sizeof(more) - strlen(more), "  core");
    /* the audio's name ("Surround 5.1"), the video's HDR first */
    if (*audio_name(k, an, sizeof(an))) {
        snprintf(tmp, sizeof(tmp), "%s  %s", an, more);
        dr_strlcpy(more, tmp, sizeof(more));
    }
    if (k->hdr[0] && k->hdr[0] != 'S') {
        snprintf(tmp, sizeof(tmp), "%s%s  %s", k->hdr, k->dolby_vision[0] ? " + Dolby Vision" : "", more);
        dr_strlcpy(more, tmp, sizeof(more));
    }
    snprintf(desc, dsize, "%s  %s", codec, more);
    trimmed(desc);
    if (lang)
        snprintf(desc + strlen(desc), dsize - strlen(desc), "  %s", lang);
}

void engine_chapter_line(const DiscTitleInfo *t, int c, char *desc, size_t dsize)
{
    char tm[48];

    snprintf(desc, dsize, "%s%s%s", engine_hms_ms(t->chapters[c].start, tm, sizeof(tm)),
             t->chapters[c].name ? "  " : "", t->chapters[c].name ? t->chapters[c].name : "");
}

void engine_queue_line(int discs, int titles, int64_t bytes, int scanning, char *buf, size_t size)
{
    char sz[32];

    snprintf(buf, size, "Queue: %d disc%s, %d title%s checked (%s)", discs, discs == 1 ? "" : "s", titles,
             titles == 1 ? "" : "s", bytes ? engine_size(bytes, sz, sizeof(sz)) : "0 MB");
    if (scanning)
        snprintf(buf + strlen(buf), size - strlen(buf), " \u2014 scanning %d disc%s\u2026", scanning,
                 scanning == 1 ? "" : "s");
}

void engine_progress_line(double done, int64_t size, double secs, int whole, char *buf, size_t bsize)
{
    snprintf(buf, bsize, "Writing %d%%", (int)(done * 100));
    /* speed and time left from the title's size (a whole title only) */
    if (secs > 2 && done > 0.001 && whole) {
        double rate = done * size / secs;
        char left[48];
        snprintf(buf + strlen(buf), bsize - strlen(buf), " \u00b7 %.1f MB/s \u00b7 %s left", rate / (1 << 20),
                 engine_hms((int64_t)((1 - done) / done * secs * 1e6), left, sizeof(left)));
    }
}

/* ---- the queue's states and notes ---- */

const char *engine_state_text(EngineState s)
{
    switch (s) {
    case ENGINE_STATE_SCAN_WAITING:   return "Waiting to scan\u2026";
    case ENGINE_STATE_SCANNING:       return "Scanning\u2026";
    case ENGINE_STATE_UNREADABLE:     return "Cannot be read (see the log)";
    case ENGINE_STATE_RIP_WAITING:    return "Waiting";
    case ENGINE_STATE_RIPPING:        return "Ripping\u2026";
    case ENGINE_NOTE_NOTHING_CHECKED: return "No title is checked: nothing to rip";
    case ENGINE_NOTE_STOPPING:        return "Stopping: the title being written ends now";
    case ENGINE_NOTE_QUEUE_STOPPED:   return "The queue was stopped";
    case ENGINE_NOTE_QUEUE_DONE:      return "The queue is done";
    }
    return "";
}

/* " part 2" for a part of a title, "" for the whole title */
static const char *part_text(int part, char *buf, size_t size)
{
    buf[0] = 0;
    if (part >= 0)
        snprintf(buf, size, " part %d", part + 1);
    return buf;
}

void engine_title_start_state(int title, int part, int ordinal, int count, char *tstate, size_t tsize,
                              char *dstate, size_t dsize)
{
    char pt[32];

    part_text(part, pt, sizeof(pt));
    snprintf(tstate, tsize, "Writing%s\u2026", pt);
    snprintf(dstate, dsize, "Title #%d%s (%d of %d)", title, pt, ordinal, count);
}

void engine_title_result_state(int part, DrStatus status, int64_t warnings, char *buf, size_t size)
{
    char pt[32];

    part_text(part, pt, sizeof(pt));
    if (status == DR_STATUS_OK && warnings)
        snprintf(buf, size, "Done%s, %"PRId64" warning(s)", pt, warnings);
    else if (status == DR_STATUS_OK)
        snprintf(buf, size, "Done%s", pt);
    else if (status == DR_STATUS_STOPPED)
        snprintf(buf, size, "Stopped%s: not complete", pt);
    else
        snprintf(buf, size, "Failed%s (see the log)", pt);
}

const char *engine_disc_result_state(int failed, int stopped)
{
    return stopped ? "Stopped" : failed ? "Done with failures (see the log)" : "Done";
}

void engine_no_disc_note(const char *path, char *buf, size_t size)
{
    snprintf(buf, size, "No disc found in %s", path);
}

void engine_setting_kept_note(const char *what, char *buf, size_t size)
{
    snprintf(buf, size, "Setting kept: %s", what);
}
