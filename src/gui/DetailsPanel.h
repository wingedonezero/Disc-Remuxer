// What the selected disc, title or track is, from the engine's disc info:
// every field the disc info has (what makemkvcon's info lists, and what the
// format part tells more), as text that wraps to the panel's width: a
// heading per block, then "Name: value" lines.
#pragma once

#include <QTextBrowser>

extern "C" {
#include "engine.h"
}

class DetailsPanel : public QTextBrowser {
    Q_OBJECT
public:
    explicit DetailsPanel(QWidget *parent = nullptr);

    void showDisc(const QString &source, const DiscInfo *d);
    void showTitle(const DiscTitleInfo *t, const uint8_t *keep);
    void showTrack(const DiscTitleInfo *t, int i, const uint8_t *keep);
    // a title's chapters (the tree's chapter rows)
    void showChapters(const DiscTitleInfo *t);
};

QString hms(int64_t us);
// h:mm:ss.mmm (a chapter's start: the disc's time, to the millisecond)
QString hmsMs(int64_t us);
QString sizeText(int64_t bytes);
// "eng (English)", "eng", or "none"
QString languageText(const char *code);
// "Video", "Audio", "Subtitle"
QString kindText(const DiscStreamInfo *k);
