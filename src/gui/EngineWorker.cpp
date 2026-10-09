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

static void sink_record(void *, const char *json)
{
    if (relay)
        emit relay->record(QString::fromUtf8(json));
}
}

LogRelay::LogRelay(QObject *parent) : QObject(parent)
{
    EngineSink sink = { sink_line, sink_progress, sink_record, nullptr };
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

void EngineWorker::stop()
{
    stopping = true;
    engine_cancel();
}

void EngineWorker::rip(QList<RipJob> jobs)
{
    stopping = false;
    for (const RipJob &j : jobs) {
        char folder[4096];
        int ret;
        if (stopping)
            break;
        engine_unique_folder(j.folder.toUtf8().constData(), folder, sizeof(folder));
        emit ripStarted(j.id, QString::fromUtf8(folder));
        ret = engine_disc_rip(j.disc, j.sel.constData(), j.split.constData(), folder);
        emit ripDone(j.id, ret, stopping);
    }
    emit runDone(stopping);
}
