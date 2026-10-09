// What the selected disc, title or track is, from the engine's disc info:
// every field the disc info has (what makemkvcon's info lists, and what the
// format part tells more).
#pragma once

#include <QTreeWidget>

extern "C" {
#include "engine.h"
}

class DetailsPanel : public QTreeWidget {
    Q_OBJECT
public:
    explicit DetailsPanel(QWidget *parent = nullptr);

    void showDisc(const QString &source, const DiscInfo *d);
    void showTitle(const DiscTitleInfo *t, const uint8_t *keep);
    void showTrack(const DiscTitleInfo *t, int i, const uint8_t *keep);

private:
    void addTrack(QTreeWidgetItem *parent, const DiscTitleInfo *t, int i, const uint8_t *keep, bool open);
};

QString hms(int64_t us);
QString sizeText(int64_t bytes);
// "eng (English)", "eng", or "none"
QString languageText(const char *code);
// "Video", "Audio", "Subtitle"
QString kindText(const DiscStreamInfo *k);
