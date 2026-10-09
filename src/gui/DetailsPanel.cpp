#include "DetailsPanel.h"

#include <QHeaderView>
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

DetailsPanel::DetailsPanel(QWidget *parent) : QTreeWidget(parent)
{
    setHeaderLabels({ "Property", "Value" });
    header()->setSectionResizeMode(0, QHeaderView::Interactive);
    header()->setSectionResizeMode(1, QHeaderView::Interactive);
    header()->setStretchLastSection(true);
    setColumnWidth(0, 210);
}

static QTreeWidgetItem *row(QTreeWidgetItem *parent, const QString &name, const QString &value)
{
    auto *it = new QTreeWidgetItem(parent, { name, value });
    it->setToolTip(1, value);
    return it;
}

static QString bytesText(int64_t bytes)
{
    return QString("%1 (%2 MB, %3 bytes)").arg(sizeText(bytes))
        .arg(QLocale().toString(bytes / 1048576.0, 'f', 2)).arg(QLocale().toString((qlonglong)bytes));
}

void DetailsPanel::showDisc(const QString &source, const DiscInfo *d)
{
    clear();
    auto *top = new QTreeWidgetItem(this, { "Disc", d->label ? QString::fromUtf8(d->label) : QString() });
    row(top, "Source", source);
    row(top, "Type", QString::fromUtf8(d->kind));
    if (d->label)
        row(top, "Volume name", QString::fromUtf8(d->label));
    if (d->name)
        row(top, "Name", QString::fromUtf8(d->name));
    QString fs = d->filesystem ? QString::fromUtf8(d->filesystem) : "?";
    if (d->udf_revision)
        fs += QString(" %1.%2").arg(d->udf_revision >> 8, 0, 16).arg(d->udf_revision & 0xff, 2, 16, QChar('0'));
    row(top, "File system", fs);
    row(top, "Encrypted", d->encrypted ? "yes" : "no");
    row(top, "Titles found", QString::number(d->nb_titles));
    // the format part's own details; "Protection: X" ones in a group of their own
    QTreeWidgetItem *protection = nullptr;
    for (int p = 0; p < d->nb_props; p++) {
        QString name = QString::fromUtf8(d->props[p].name), v = QString::fromUtf8(d->props[p].value);
        if (name.contains("language"))
            v = languageText(d->props[p].value);
        if (name.startsWith("Protection: ")) {
            if (!protection)
                protection = row(top, "Protection", "");
            row(protection, name.mid(12), v);
        } else {
            row(top, name, v);
        }
    }
    expandAll();
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

void DetailsPanel::addTrack(QTreeWidgetItem *parent, const DiscTitleInfo *t, int i, const uint8_t *keep, bool open)
{
    const DiscStreamInfo *k = &t->streams[i];
    char codec[96], rest[512], buf[96];
    const char *mkv = disc_mkv_codec_id(k);

    engine_track_parts(t, i, codec, sizeof(codec), rest, sizeof(rest));
    QString summary = QString("%1 %2%3").arg(QString::fromUtf8(k->language[0] ? k->language : "")).arg(codec)
                          .arg(QString::fromUtf8(rest));
    auto *tr = row(parent, QString("Track #%1").arg(i), summary.trimmed());
    row(tr, "Kind", kindText(k));
    row(tr, "Codec (full)", QString::fromUtf8(codec));
    row(tr, "Codec (short)", QString::fromUtf8(codec_short(k)));
    if (mkv)
        row(tr, "Codec ID", QString::fromUtf8(mkv));
    if (*codec_profile_level(k, buf, sizeof(buf)))
        row(tr, "Profile", QString::fromUtf8(buf));
    row(tr, "Language", languageText(k->language));
    if (*audio_name(k, buf, sizeof(buf)))
        row(tr, "Name", QString::fromUtf8(buf));
    // TrueHD / MLP: variable; the peak rate their header states reads far too low on real discs (LOOK INTO
    // AGAIN LATER), so none is shown. DTS-HD: the rate is its DTS core's.
    if (k->codec == AV_CODEC_ID_TRUEHD || k->codec == AV_CODEC_ID_MLP)
        row(tr, "Bit rate", "variable (lossless)");
    else if (k->bit_rate && *bit_rate_text(k->bit_rate, buf, sizeof(buf)))
        row(tr, "Bit rate", QString::fromUtf8(buf) + (k->codec == AV_CODEC_ID_DTS && k->inner_track >= 0 ?
                                                      " (its DTS core's)" : ""));
    if (k->type == AVMEDIA_TYPE_VIDEO) {
        if (k->width)
            row(tr, "Video size", QString("%1x%2").arg(k->width).arg(k->height));
        if (k->aspect.num > 0 && *aspect_text(k, buf, sizeof(buf)))
            row(tr, "Aspect ratio", QString::fromUtf8(buf));
        if (k->frame_rate.num > 0 && *frame_rate_text(k, buf, sizeof(buf)))
            row(tr, "Frame rate", QString::fromUtf8(buf));
        if (k->hdr[0])
            row(tr, "HDR", QString::fromUtf8(k->hdr) + (k->dolby_vision[0] ? " (the base layer), with Dolby Vision" : ""));
        if (k->dolby_vision[0])
            row(tr, "Dolby Vision", QString::fromUtf8(k->dolby_vision));
        if (k->colour[0])
            row(tr, "Colour", QString::fromUtf8(k->colour));
        if (k->mastering[0])
            row(tr, "Mastering display", QString::fromUtf8(k->mastering));
        if (k->max_cll || k->max_fall)
            row(tr, "Content light level", QString("MaxCLL %1, MaxFALL %2 cd/m2").arg(k->max_cll).arg(k->max_fall));
    }
    if (k->type == AVMEDIA_TYPE_AUDIO) {
        if (k->channels)
            row(tr, "Channels", QString::number(k->channels));
        if (k->layout && *layout_text(k, buf, sizeof(buf)))
            row(tr, "Channel layout", QString::fromUtf8(buf));
        if (k->sample_rate)
            row(tr, "Sample rate", QString("%1 Hz").arg(k->sample_rate));
        if (k->bits)
            row(tr, "Sample size", QString("%1 bits").arg(k->bits));
    }
    if (k->type == AVMEDIA_TYPE_SUBTITLE && k->width)
        row(tr, "Canvas", QString("%1x%2").arg(k->width).arg(k->height));
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
        row(tr, "Flags", flags.join("; "));
    row(tr, "Output", QString(".%1: %2").arg(QString::fromUtf8(engine_track_extension(k)), conversionText(k)));
    row(tr, "Ripped", i == 0 ? "yes (the video is always ripped)" : keep[i] ? "yes" : "no (unchecked)");
    tr->setExpanded(open);
}

void DetailsPanel::showTitle(const DiscTitleInfo *t, const uint8_t *keep)
{
    char name[512];

    clear();
    engine_title_file_name(t, name, sizeof(name));
    auto *top = new QTreeWidgetItem(this, { "Title", QString("#%1").arg(t->index) });
    if (t->name)
        row(top, "Name", QString::fromUtf8(t->name));
    row(top, "Duration", hms(t->duration));
    row(top, "Size", bytesText(t->size));
    row(top, "Chapters", QString::number(t->nb_chapters));
    if (t->source)
        row(top, "Source", QString::fromUtf8(t->source));
    if (t->nb_files) {
        QStringList files;
        for (int i = 0; i < t->nb_files; i++)
            files << QString::fromUtf8(t->files[i]);
        auto *seg = row(top, "Segments", QString::number(t->nb_files));
        row(seg, "Map", files.join(","));
        seg->setExpanded(true);
    }
    if (t->angle)
        row(top, "Angle", QString::number(t->angle));
    row(top, "Language", languageText(t->language));
    row(top, "Output file name", QString("%1_<track>_…  (in a folder %1 when more than one title is ripped)")
                                     .arg(QString::fromUtf8(name)));
    row(top, "Minimum length", t->selected ? "long enough" : "shorter: not ripped with all");
    for (int p = 0; p < t->nb_props; p++)
        row(top, QString::fromUtf8(t->props[p].name), QString::fromUtf8(t->props[p].value));
    if (t->nb_parts) {
        auto *parts = row(top, "Parts", QString::number(t->nb_parts));
        for (int p = 0; p < t->nb_parts; p++) {
            const DiscPartInfo *q = &t->parts[p];
            QString files;
            for (int f = 0; f < q->nb_files; f++)
                files += (f ? ", " : "") + QString::fromUtf8(t->files[q->first_file + f]);
            row(parts, QString::number(p + 1), QString("at %1, %2, %3, %4 chapter(s)").arg(hms(q->start))
                                                   .arg(hms(q->duration)).arg(files).arg(q->nb_chapters));
        }
    }
    auto *chapters = row(top, "Chapter list", QString::number(t->nb_chapters));
    for (int i = 0; i < t->nb_chapters; i++)
        row(chapters, QString::number(i + 1),
            hms(t->chapters[i].start) + (t->chapters[i].name ? "  " + QString::fromUtf8(t->chapters[i].name) : ""));
    top->setExpanded(true);

    // the tracks by kind, as makemkvcon groups them
    for (enum AVMediaType type : { AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO, AVMEDIA_TYPE_SUBTITLE }) {
        QTreeWidgetItem *group = nullptr;
        for (int i = 0; i < t->nb_streams; i++) {
            if (t->streams[i].type != type)
                continue;
            if (!group) {
                group = new QTreeWidgetItem(this, { type == AVMEDIA_TYPE_VIDEO ? "Video" :
                                                    type == AVMEDIA_TYPE_AUDIO ? "Audio" : "Subtitles", "" });
                group->setExpanded(true);
            }
            addTrack(group, t, i, keep, false);
        }
    }
}

void DetailsPanel::showTrack(const DiscTitleInfo *t, int i, const uint8_t *keep)
{
    clear();
    auto *top = new QTreeWidgetItem(this, { "Title", QString("#%1").arg(t->index) });
    addTrack(top, t, i, keep, true);
    top->setExpanded(true);
}
