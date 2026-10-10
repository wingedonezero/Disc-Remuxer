#include "DetailsPanel.h"

#include <QLocale>

extern "C" {
#include <libavutil/avutil.h>
#include "codecs.h"
}

QString hms(int64_t us)
{
    int64_t t = (us + 500000) / 1000000;
    return QString("%1:%2:%3").arg(t / 3600).arg(t / 60 % 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'));
}

QString hmsMs(int64_t us)
{
    int64_t ms = (us + 500) / 1000;
    return QString("%1:%2:%3.%4").arg(ms / 3600000).arg(ms / 60000 % 60, 2, 10, QChar('0'))
        .arg(ms / 1000 % 60, 2, 10, QChar('0')).arg(ms % 1000, 3, 10, QChar('0'));
}

// GB / MB of 1024^3 / 1024^2 bytes, one decimal, as info writes it
QString sizeText(int64_t bytes)
{
    bool gb = bytes >= (1LL << 30);
    int64_t tenths = bytes * 10 / (gb ? 1LL << 30 : 1LL << 20);
    return QString("%1.%2 %3").arg(tenths / 10).arg(tenths % 10).arg(gb ? "GB" : "MB");
}

QString languageText(const char *code)
{
    if (!code || !*code)
        return "none";
    const char *name = disc_language_name(code);
    return name ? QString("%1 (%2)").arg(QString::fromUtf8(code), QString::fromUtf8(name)) : QString::fromUtf8(code);
}

QString kindText(const DiscStreamInfo *k)
{
    switch (k->type) {
    case AVMEDIA_TYPE_VIDEO:    return "Video";
    case AVMEDIA_TYPE_AUDIO:    return "Audio";
    case AVMEDIA_TYPE_SUBTITLE: return "Subtitle";
    default:                    return "Other";
    }
}

DetailsPanel::DetailsPanel(QWidget *parent) : QTextBrowser(parent)
{
    setLineWrapMode(QTextEdit::WidgetWidth);
    setOpenLinks(false);
}

// The panel's text: a heading per block, "Name: value" lines that wrap at
// the panel's width (a deeper level indented).
struct Html {
    QString s;
    void head(const QString &h)
    {
        s += QString("<p style=\"margin-top:10px; margin-bottom:3px\"><b><u>%1</u></b></p>").arg(h.toHtmlEscaped());
    }
    void sub(const QString &h, int level = 1)
    {
        s += QString("<p style=\"margin-top:6px; margin-bottom:1px; margin-left:%1px\"><b>%2</b></p>")
                 .arg(level * 14).arg(h.toHtmlEscaped());
    }
    void row(const QString &name, const QString &value, int level = 0)
    {
        s += QString("<p style=\"margin:0px; margin-left:%1px\"><span style=\"color:#808080\">%2:</span> "
                     "%3</p>").arg(level * 14).arg(name.toHtmlEscaped(), value.toHtmlEscaped());
    }
};

static QString bytesText(int64_t bytes)
{
    return QString("%1 (%2 MB, %3 bytes)").arg(sizeText(bytes))
        .arg(QLocale().toString(bytes / 1048576.0, 'f', 2)).arg(QLocale().toString((qlonglong)bytes));
}

void DetailsPanel::showDisc(const QString &source, const DiscInfo *d)
{
    Html h;

    h.head("Disc information");
    h.row("Source", source);
    h.row("Type", QString::fromUtf8(d->kind));
    if (d->label)
        h.row("Volume name", QString::fromUtf8(d->label));
    if (d->name)
        h.row("Name", QString::fromUtf8(d->name));
    QString fs = d->filesystem ? QString::fromUtf8(d->filesystem) : "?";
    if (d->udf_revision)
        fs += QString(" %1.%2").arg(d->udf_revision >> 8, 0, 16).arg(d->udf_revision & 0xff, 2, 16, QChar('0'));
    h.row("File system", fs);
    h.row("Encrypted", d->encrypted ? "yes" : "no");
    h.row("Titles found", QString::number(d->nb_titles));
    // the format part's own details; "Protection: X" ones in a block of their own
    bool protection = false;
    for (int p = 0; p < d->nb_props; p++) {
        QString name = QString::fromUtf8(d->props[p].name), v = QString::fromUtf8(d->props[p].value);
        if (name.contains("language"))
            v = languageText(d->props[p].value);
        if (!name.startsWith("Protection: "))
            h.row(name, v);
    }
    for (int p = 0; p < d->nb_props; p++) {
        QString name = QString::fromUtf8(d->props[p].name);
        if (!name.startsWith("Protection: "))
            continue;
        if (!protection)
            h.head("Protection");
        protection = true;
        h.row(name.mid(12), QString::fromUtf8(d->props[p].value));
    }
    setHtml(h.s);
}

// what a track's file is made of
static QString conversionText(const DiscStreamInfo *k)
{
    if (k->second_view)
        return "the 3-D second view (MVC), as on the disc";
    switch (k->codec) {
    case AV_CODEC_ID_PCM_DVD:
    case AV_CODEC_ID_PCM_BLURAY: return "LPCM written as PCM in a WAVE file (lossless)";
    case AV_CODEC_ID_EIA_608:    return "closed captions made into SRT text";
    case AV_CODEC_ID_DVD_SUBTITLE: return "VobSub (.sub with its .idx), as on the disc";
    default:                     return "as on the disc (copied, lossless)";
    }
}

static void addTrack(Html &h, const DiscTitleInfo *t, int i, const uint8_t *keep, int level)
{
    const DiscStreamInfo *k = &t->streams[i];
    char codec[96], rest[512], buf[96];
    const char *mkv = disc_mkv_codec_id(k);

    engine_track_parts(t, i, codec, sizeof(codec), rest, sizeof(rest));
    h.sub(QString("Track #%1: %2 %3%4").arg(i).arg(kindText(k), QString::fromUtf8(codec), QString::fromUtf8(rest))
              .trimmed(), level);
    level++;
    h.row("Kind", kindText(k), level);
    h.row("Codec (full)", QString::fromUtf8(codec), level);
    h.row("Codec (short)", QString::fromUtf8(codec_short(k)), level);
    if (mkv)
        h.row("Codec ID", QString::fromUtf8(mkv), level);
    if (*codec_profile_level(k, buf, sizeof(buf)))
        h.row("Profile", QString::fromUtf8(buf), level);
    h.row("Language", languageText(k->language), level);
    if (*audio_name(k, buf, sizeof(buf)))
        h.row("Name", QString::fromUtf8(buf), level);
    // TrueHD / MLP: variable; the peak rate their header states reads far too low on real discs (LOOK INTO
    // AGAIN LATER), so none is shown. DTS-HD: the rate is its DTS core's.
    if (k->codec == AV_CODEC_ID_TRUEHD || k->codec == AV_CODEC_ID_MLP)
        h.row("Bit rate", "variable (lossless)", level);
    else if (k->bit_rate && *bit_rate_text(k->bit_rate, buf, sizeof(buf)))
        h.row("Bit rate", QString::fromUtf8(buf) + (k->codec == AV_CODEC_ID_DTS && k->inner_track >= 0 ?
                                                    " (its DTS core's)" : ""), level);
    if (k->type == AVMEDIA_TYPE_VIDEO) {
        if (k->width)
            h.row("Video size", QString("%1x%2").arg(k->width).arg(k->height), level);
        if (k->aspect.num > 0 && *aspect_text(k, buf, sizeof(buf)))
            h.row("Aspect ratio", QString::fromUtf8(buf), level);
        if (k->frame_rate.num > 0 && *frame_rate_text(k, buf, sizeof(buf)))
            h.row("Frame rate", QString::fromUtf8(buf), level);
        if (k->hdr[0])
            h.row("HDR", QString::fromUtf8(k->hdr) + (k->dolby_vision[0] ? " (the base layer), with Dolby Vision" : ""),
                  level);
        if (k->dolby_vision[0])
            h.row("Dolby Vision", QString::fromUtf8(k->dolby_vision), level);
        if (k->colour[0])
            h.row("Colour", QString::fromUtf8(k->colour), level);
        if (k->mastering[0])
            h.row("Mastering display", QString::fromUtf8(k->mastering), level);
        if (k->max_cll || k->max_fall)
            h.row("Content light level", QString("MaxCLL %1, MaxFALL %2 cd/m2").arg(k->max_cll).arg(k->max_fall),
                  level);
    }
    if (k->type == AVMEDIA_TYPE_AUDIO) {
        if (k->channels)
            h.row("Channels", QString::number(k->channels), level);
        if (k->layout && *layout_text(k, buf, sizeof(buf)))
            h.row("Channel layout", QString::fromUtf8(buf), level);
        if (k->sample_rate)
            h.row("Sample rate", QString("%1 Hz").arg(k->sample_rate), level);
        if (k->bits)
            h.row("Sample size", QString("%1 bits").arg(k->bits), level);
    }
    if (k->type == AVMEDIA_TYPE_SUBTITLE && k->width)
        h.row("Canvas", QString("%1x%2").arg(k->width).arg(k->height), level);
    // what the disc says of it and how it is linked to other tracks
    QStringList flags;
    static const struct { int bit; const char *word; } roles[] = {
        { DISC_ROLE_COMMENTARY, "commentary" }, { DISC_ROLE_ALT_COMMENTARY, "alternate commentary" },
        { DISC_ROLE_VISUALLY_IMPAIRED, "for the visually impaired" }, { DISC_ROLE_LARGE, "large" },
        { DISC_ROLE_CHILDREN, "for children" }, { DISC_ROLE_FORCED, "forced subtitles" },
    };
    for (const auto &r : roles)
        if (k->roles & r.bit)
            flags << r.word;
    if (k->forced)
        flags << QString("the forced subtitles of track %1").arg(k->from_track);
    else if (k->second_view)
        flags << QString("the 3-D second view of track %1").arg(k->from_track);
    else if (k->from_track >= 0)
        flags << QString("taken out of track %1 (its core)").arg(k->from_track);
    if (k->inner_track >= 0)
        flags << QString("has a core: track %1 is taken out of it").arg(k->inner_track);
    if (k->second_layer)
        flags << "a second layer is read with it (Dolby Vision enhancement layer)";
    if (k->offset_sequence >= 0)
        flags << QString("3-D offset sequence %1").arg(k->offset_sequence);
    if (!flags.isEmpty())
        h.row("Flags", flags.join("; "), level);
    h.row("Output", QString(".%1: %2").arg(QString::fromUtf8(engine_track_extension(k)), conversionText(k)), level);
    h.row("Ripped", i == 0 ? "yes (the video is always ripped)" : keep[i] ? "yes" : "no (unchecked)", level);
}

static void chapterList(Html &h, const DiscTitleInfo *t)
{
    // the disc's chapter marks; a rip starts each chapter at the first video frame from its mark (the
    // chapters file written then has those times)
    h.head(QString("Chapters (%1, as the disc marks them)").arg(t->nb_chapters));
    for (int i = 0; i < t->nb_chapters; i++)
        h.row(QString("Chapter %1").arg(i + 1),
              hmsMs(t->chapters[i].start) + (t->chapters[i].name ? "  " + QString::fromUtf8(t->chapters[i].name) : ""));
}

void DetailsPanel::showTitle(const DiscTitleInfo *t, const uint8_t *keep)
{
    char name[512];
    Html h;

    engine_title_file_name(t, name, sizeof(name));
    h.head(QString("Title information: #%1").arg(t->index));
    if (t->name)
        h.row("Name", QString::fromUtf8(t->name));
    h.row("Duration", hms(t->duration));
    h.row("Size", bytesText(t->size));
    h.row("Chapters", QString::number(t->nb_chapters));
    if (t->source)
        h.row("Source", QString::fromUtf8(t->source));
    if (t->nb_files) {
        QStringList files;
        for (int i = 0; i < t->nb_files; i++)
            files << QString::fromUtf8(t->files[i]);
        h.row("Segments", QString::number(t->nb_files));
        h.row("Segment map", files.join(", "));
    }
    if (t->angle)
        h.row("Angle", QString::number(t->angle));
    h.row("Language", languageText(t->language));
    h.row("Output", QString("%1/%1_<track>_…  (as parts: %1_p1/%1_p1_<track>_…, one folder per part)")
                        .arg(QString::fromUtf8(name)));
    h.row("Minimum length", t->selected ? "long enough" : "shorter: not ripped with all");
    for (int p = 0; p < t->nb_props; p++)
        h.row(QString::fromUtf8(t->props[p].name), QString::fromUtf8(t->props[p].value));
    if (t->nb_parts) {
        h.head(QString("Parts (%1)").arg(t->nb_parts));
        for (int p = 0; p < t->nb_parts; p++) {
            const DiscPartInfo *q = &t->parts[p];
            QString files;
            for (int f = 0; f < q->nb_files; f++)
                files += (f ? ", " : "") + QString::fromUtf8(t->files[q->first_file + f]);
            h.row(QString("Part %1").arg(p + 1), QString("at %1, %2, %3, %4 chapter(s)").arg(hmsMs(q->start))
                                                     .arg(hms(q->duration)).arg(files).arg(q->nb_chapters));
        }
    }
    // the tracks by kind, as makemkvcon groups them
    for (enum AVMediaType type : { AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_SUBTITLE }) {
        bool any = false;
        for (int i = 0; i < t->nb_streams; i++) {
            if (t->streams[i].type != type)
                continue;
            if (!any)
                h.head(type == AVMEDIA_TYPE_VIDEO ? "Video" : type == AVMEDIA_TYPE_AUDIO ? "Audio" : "Subtitles");
            any = true;
            addTrack(h, t, i, keep, 0);
        }
    }
    chapterList(h, t);
    setHtml(h.s);
}

void DetailsPanel::showTrack(const DiscTitleInfo *t, int i, const uint8_t *keep)
{
    Html h;

    h.head(QString("Track information (title #%1)").arg(t->index));
    addTrack(h, t, i, keep, 0);
    setHtml(h.s);
}

void DetailsPanel::showChapters(const DiscTitleInfo *t)
{
    Html h;

    h.head(QString("Title #%1%2").arg(t->index).arg(t->name ? ": " + QString::fromUtf8(t->name) : QString()));
    chapterList(h, t);
    setHtml(h.s);
}
