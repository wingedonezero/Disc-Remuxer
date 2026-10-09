#include "DetailsPanel.h"

#include <QHeaderView>

extern "C" {
#include <libavutil/avutil.h>
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

DetailsPanel::DetailsPanel(QWidget *parent) : QTreeWidget(parent)
{
    setHeaderLabels({ "Property", "Value" });
    header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(1, QHeaderView::Stretch);
}

static QTreeWidgetItem *row(QTreeWidgetItem *parent, const QString &name, const QString &value)
{
    return new QTreeWidgetItem(parent, { name, value });
}

void DetailsPanel::showDisc(const QString &source, const DiscInfo *d)
{
    clear();
    auto *top = new QTreeWidgetItem(this, { "Disc", d->label ? QString::fromUtf8(d->label) : QString() });
    row(top, "Source", source);
    row(top, "Kind", QString::fromUtf8(d->kind));
    if (d->name)
        row(top, "Name", QString::fromUtf8(d->name));
    QString fs = d->filesystem ? QString::fromUtf8(d->filesystem) : "?";
    if (d->udf_revision)
        fs += QString(" %1.%2").arg(d->udf_revision >> 8, 0, 16).arg(d->udf_revision & 0xff, 2, 16, QChar('0'));
    row(top, "File system", fs);
    row(top, "Encrypted", d->encrypted ? "yes" : "no");
    row(top, "Titles", QString::number(d->nb_titles));
    expandAll();
}

void DetailsPanel::showTitle(const DiscTitleInfo *t, const uint8_t *keep)
{
    clear();
    auto *top = new QTreeWidgetItem(this, { "Title", QString("#%1").arg(t->index) });
    if (t->name)
        row(top, "Name", QString::fromUtf8(t->name));
    row(top, "Duration", hms(t->duration));
    row(top, "Size", sizeText(t->size));
    if (t->source)
        row(top, "Made from", QString::fromUtf8(t->source));
    if (t->angle)
        row(top, "Angle", QString::number(t->angle));
    if (t->language[0])
        row(top, "Language", QString::fromUtf8(t->language));
    row(top, "Minimum length", t->selected ? "long enough" : "shorter: not ripped with all");
    if (t->nb_files) {
        auto *files = row(top, "Files", QString::number(t->nb_files));
        for (int i = 0; i < t->nb_files; i++)
            row(files, QString::number(i + 1), QString::fromUtf8(t->files[i]));
    }
    auto *tracks = row(top, "Tracks", QString::number(t->nb_streams));
    for (int i = 0; i < t->nb_streams; i++) {
        char text[512];
        row(tracks, QString("%1 %2 %3").arg(i).arg(av_get_media_type_string(t->streams[i].type))
                        .arg(QString::fromUtf8(t->streams[i].language)),
            QString::fromUtf8(engine_track_text(t, i, keep[i], text, sizeof(text))));
    }
    auto *chapters = row(top, "Chapters", QString::number(t->nb_chapters));
    for (int i = 0; i < t->nb_chapters; i++)
        row(chapters, QString::number(i + 1),
            hms(t->chapters[i].start) + (t->chapters[i].name ? "  " + QString::fromUtf8(t->chapters[i].name) : ""));
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
    top->setExpanded(true);
    tracks->setExpanded(true);
}

void DetailsPanel::showTrack(const DiscTitleInfo *t, int i, const uint8_t *keep)
{
    const DiscStreamInfo *k = &t->streams[i];
    char text[512], buf[64];

    clear();
    auto *top = new QTreeWidgetItem(this, { "Track", QString("%1 of title #%2").arg(i).arg(t->index) });
    row(top, "Kind", av_get_media_type_string(k->type));
    row(top, "Codec", QString::fromUtf8(engine_track_text(t, i, 1, text, sizeof(text))));
    row(top, "Language", k->language[0] ? QString::fromUtf8(k->language) : "none");
    row(top, "Ripped", keep[i] ? "yes" : "no");
    if (k->width)
        row(top, k->type == AVMEDIA_TYPE_SUBTITLE ? "Canvas" : "Size", QString("%1x%2").arg(k->width).arg(k->height));
    if (k->frame_rate.num > 0)
        row(top, "Frame rate", QString("%1/%2").arg(k->frame_rate.num).arg(k->frame_rate.den));
    if (k->aspect.num > 0)
        row(top, "Aspect", QString("%1:%2").arg(k->aspect.num).arg(k->aspect.den));
    if (k->dolby_vision[0])
        row(top, "Dolby Vision", QString::fromUtf8(k->dolby_vision));
    if (k->channels)
        row(top, "Channels", QString::number(k->channels));
    if (k->sample_rate)
        row(top, "Sample rate", QString("%1 Hz").arg(k->sample_rate));
    if (k->bits)
        row(top, "Bits", QString::number(k->bits));
    if (k->bit_rate) {
        snprintf(buf, sizeof(buf), "%lld bit/s", (long long)k->bit_rate);
        row(top, "Bit rate", buf);
    }
    if (k->from_track >= 0)
        row(top, k->second_view ? "Second view of" : k->forced ? "Forced subtitles of" : "Taken out of",
            QString("track %1").arg(k->from_track));
    if (k->inner_track >= 0)
        row(top, "Has taken out of it", QString("track %1").arg(k->inner_track));
    if (k->second_layer)
        row(top, "Second layer", "read with it (ripped as the settings say)");
    expandAll();
}
