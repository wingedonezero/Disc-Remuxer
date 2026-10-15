#include "EngineWorker.h"

#include <QByteArray>

static LogRelay *relay;

static QString text(const char *s)
{
    return s ? QString::fromUtf8(s) : QString();
}

static void line_event(const DrEventLine &e)
{
    QString src = text(e.source), t = text(e.text);

    if (!relay->live)       /* before the window listens: only the GUI's own thread logs */
        relay->early.append({ e.level, e.code, src, t, e.heading != 0 });
    else
        emit relay->line(e.level, e.code, src, t, e.heading != 0);
}

static TrackResult track_result(const DrEventTrackResult &e)
{
    TrackResult r;

    r.title        = e.title;
    r.part         = e.part;
    r.index        = e.index;
    r.codec        = text(e.codec);
    r.language     = text(e.language);
    r.layout       = text(e.layout);
    r.file         = text(e.file);
    r.frames       = e.frames;
    r.warnings     = e.warnings;
    r.delayMs      = e.delay_ms;
    r.hasLine21    = e.has_line21;
    r.line21       = text(e.line21);
    r.ccBlocks     = e.cc_blocks;
    r.ccText       = e.cc_text;
    r.sub          = e.sub;
    r.caption      = text(e.caption);
    r.cues         = e.cues;
    r.hasCcLost    = e.has_cc_lost;
    r.ccLost       = e.cc_lost;
    r.picture      = text(e.picture);
    r.width        = e.width;
    r.height       = e.height;
    r.noStop       = e.no_stop;
    r.outside      = e.outside;
    r.detailsCheck = e.details_check;
    r.details      = text(e.details);
    return r;
}

extern "C" {
static void sink_event(void *, const DrEvent *ev)
{
    if (!relay)
        return;
    switch (ev->kind) {
    case DR_EVENT_LINE:
        line_event(ev->u.line);
        break;
    case DR_EVENT_PROGRESS:
        emit relay->progress(ev->u.progress.title, ev->u.progress.done);
        break;
    case DR_EVENT_TITLE_START: {
        const DrEventTitleStart &e = ev->u.title_start;
        emit relay->titleStarted({ e.title, e.part, text(e.name), e.ordinal, e.count });
        break;
    }
    case DR_EVENT_TRACK_RESULT:
        emit relay->trackDone(track_result(ev->u.track));
        break;
    case DR_EVENT_TITLE_RESULT: {
        const DrEventTitleResult &e = ev->u.title_result;
        emit relay->titleDone({ e.title, e.part, e.status, e.warnings, e.damaged_ranges, e.damaged_bytes,
                                text(e.folder) });
        break;
    }
    case DR_EVENT_RUN_RESULT: {
        const DrEventRunResult &e = ev->u.run;
        emit relay->runResult({ e.written, e.failed, e.warnings, e.errors, e.stopped != 0, text(e.log) });
        break;
    }
    default:                /* the disc and its titles: the window reads the disc model itself */
        break;
    }
}
}

LogRelay::LogRelay(QObject *parent) : QObject(parent)
{
    DrSink sink = { sink_event, nullptr };

    qRegisterMetaType<TitleStart>();
    qRegisterMetaType<TrackResult>();
    qRegisterMetaType<TitleResult>();
    qRegisterMetaType<RunResult>();
    relay = this;
    engine_set_events(&sink);
}

void LogRelay::goLive()
{
    live = true;
    for (const Early &e : early)
        emit line(e.level, e.code, e.source, e.text, e.heading);
    early.clear();
}

LogRelay::~LogRelay()
{
    engine_set_events(nullptr);
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
        ret = engine_disc_rip(j.disc, folder);
        emit ripDone(j.id, ret, stopping);
    }
    emit runDone(stopping);
}
