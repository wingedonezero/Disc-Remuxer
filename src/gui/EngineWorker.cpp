#include "EngineWorker.h"

#include <QByteArray>

static LogRelay *relay;

extern "C" {
static void sink_line(void *, int level, int code, const char *source, const char *text, int heading)
{
    if (relay)
        emit relay->line(level, code, source ? QString::fromUtf8(source) : QString(), QString::fromUtf8(text),
                         heading != 0);
}

static void sink_progress(void *, int title, double done)
{
    if (relay)
        emit relay->progress(title, done);
}
}

LogRelay::LogRelay(QObject *parent) : QObject(parent)
{
    EngineSink sink = { sink_line, sink_progress, nullptr, nullptr };
    relay = this;
    engine_set_sink(&sink);
}

LogRelay::~LogRelay()
{
    engine_set_sink(nullptr);
    relay = nullptr;
}

void EngineWorker::scan(quint64 id, QString path)
{
    EngineChoice choice = { nullptr, 0, nullptr };
    EngineDisc *disc = nullptr;
    QByteArray p = path.toUtf8();

    emit scanStarted(id);
    if (engine_disc_open(p.constData(), &choice, &disc) < 0)
        disc = nullptr;
    emit scanned(id, reinterpret_cast<quintptr>(disc));
}
