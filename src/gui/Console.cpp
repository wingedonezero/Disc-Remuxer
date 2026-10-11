#include "Console.h"

#include <QCheckBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

extern "C" {
#include "log.h"
#include "settings.h"
}

Console::Console(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    auto *clearButton = new QPushButton("Clear log");

    layout->setContentsMargins(0, 0, 0, 0);
    view = new QPlainTextEdit;
    view->setReadOnly(true);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    view->setMaximumBlockCount(200000);
    details = new QCheckBox("Show detail lines");
    bar->addWidget(new QLabel("Log"));
    bar->addStretch();
    bar->addWidget(details);
    bar->addWidget(clearButton);
    layout->addLayout(bar);
    layout->addWidget(view);
    // kept in the settings file (saved with the window's sizes)
    details->setChecked(setting_bool("gui.show_detail_lines"));
    connect(details, &QCheckBox::toggled, this, [this](bool on) {
        char err[256];
        settings_store("gui.show_detail_lines", on ? "true" : "false", err, sizeof(err));
        refill();
    });
    connect(clearButton, &QPushButton::clicked, this, &Console::clear);
}

void Console::show(const Line &l)
{
    static const char *const colour[] = { "#e05050", "#e0a030", "#40b040", nullptr, "#888888" };
    const char *c = l.level >= 0 && l.level <= LOG_DETAIL ? colour[l.level] : nullptr;
    QString text = l.text.toHtmlEscaped();

    if (l.level == LOG_DETAIL && !details->isChecked())
        return;
    if (l.level == LOG_ERROR)
        text = "ERROR   " + text;
    else if (l.level == LOG_WARNING)
        text = "WARNING " + text;
    if (l.heading)
        text = "<b>" + text + "</b>";
    if (c)
        text = QString("<span style=\"color:%1\">%2</span>").arg(c, text);
    view->appendHtml("<pre style=\"margin:0\">" + text + "</pre>");
}

void Console::addLine(int level, int, QString, QString text, bool heading)
{
    lines.append({ level, heading, text });
    show(lines.last());
}

void Console::refill()
{
    view->clear();
    for (const Line &l : lines)
        show(l);
}

void Console::clear()
{
    lines.clear();
    view->clear();
}
