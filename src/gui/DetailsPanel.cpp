#include "DetailsPanel.h"

#include <QByteArray>

QString hms(int64_t us)
{
    char buf[48];
    return QString::fromUtf8(engine_hms(us, buf, sizeof(buf)));
}

QString hmsMs(int64_t us)
{
    char buf[48];
    return QString::fromUtf8(engine_hms_ms(us, buf, sizeof(buf)));
}

QString sizeText(int64_t bytes)
{
    char buf[48];
    return QString::fromUtf8(engine_size(bytes, buf, sizeof(buf)));
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

// The engine's details (views.h) as the panel's text.
static void showRows(QTextBrowser *b, int ret, EngineRows *r)
{
    Html h;

    for (int i = 0; ret >= 0 && i < r->nb; i++) {
        const EngineRow &row = r->rows[i];
        QString label = QString::fromUtf8(row.label);
        if (row.kind == ENGINE_ROW_HEAD)
            h.head(label);
        else if (row.kind == ENGINE_ROW_SUB)
            h.sub(label, row.level);
        else
            h.row(label, QString::fromUtf8(row.value), row.level);
    }
    engine_rows_free(r);
    b->setHtml(h.s);
}

void DetailsPanel::showDisc(const QString &source, const DiscInfo *d)
{
    EngineRows r = {};
    QByteArray src = source.toUtf8();

    showRows(this, engine_disc_rows(d, src.constData(), &r), &r);
}

void DetailsPanel::showTitle(const DiscTitleInfo *t, const uint8_t *keep)
{
    EngineRows r = {};

    showRows(this, engine_title_rows(t, keep, &r), &r);
}

void DetailsPanel::showTrack(const DiscTitleInfo *t, int i, const uint8_t *keep)
{
    EngineRows r = {};

    showRows(this, engine_track_rows(t, i, keep, &r), &r);
}

void DetailsPanel::showChapters(const DiscTitleInfo *t)
{
    EngineRows r = {};

    showRows(this, engine_chapters_rows(t, &r), &r);
}
