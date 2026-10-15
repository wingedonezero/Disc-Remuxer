// The engine on a thread of its own: discs are opened (scanned) and ripped
// there one after another, so the window never waits for a disc. The
// engine's events (events.h: log lines, progress, a title's start and
// result, each track's result, the run's result) come back as signals
// carrying the data (LogRelay), whichever thread they came from.
#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QVector>
#include <QObject>
#include <QString>

#include <atomic>

extern "C" {
#include "engine.h"
#include "events.h"
}

// A title's rip began (DrEventTitleStart).
struct TitleStart {
    int     title = -1, part = -1;      // part -1: the whole title
    QString name;
    int     ordinal = 0, count = 0;
};

// One written track (DrEventTrackResult), its strings copied.
struct TrackResult {
    int     title = -1, part = -1, index = -1;
    QString codec, language, layout, file;
    qint64  frames = 0, warnings = 0, delayMs = 0;
    bool    hasLine21 = false;
    QString line21;
    qint64  ccBlocks = 0, ccText = 0;
    int     sub = DR_SUB_NONE;
    QString caption;
    qint64  cues = 0;
    bool    hasCcLost = false;
    qint64  ccLost = 0;
    QString picture;
    int     width = 0, height = 0;
    qint64  noStop = 0, outside = 0;
    int     detailsCheck = 0;           // 0 not checked, 1 passed, -1 failed
    QString details;
};

// A title's rip ended (DrEventTitleResult).
struct TitleResult {
    int      title = -1, part = -1;
    DrStatus status = DR_STATUS_OK;
    qint64   warnings = 0, damagedRanges = 0, damagedBytes = 0;
    QString  folder;
};

// The run ended (DrEventRunResult).
struct RunResult {
    int     written = 0, failed = 0, warnings = 0, errors = 0;
    bool    stopped = false;
    QString log;
};

Q_DECLARE_METATYPE(TitleStart)
Q_DECLARE_METATYPE(TrackResult)
Q_DECLARE_METATYPE(TitleResult)
Q_DECLARE_METATYPE(RunResult)

// Receives the engine's events (engine_set_events()) and gives them to the
// window as queued signals, with the data copied.
class LogRelay : public QObject {
    Q_OBJECT
public:
    explicit LogRelay(QObject *parent = nullptr);
    ~LogRelay() override;

    // The lines logged before the window listened (the settings file read
    // at the start) given out now; every later line as it comes.
    void goLive();

    struct Early {
        int     level, code;
        QString source, text;
        bool    heading;
    };
    QVector<Early> early;
    bool           live = false;

signals:
    void line(int level, int code, QString source, QString text, bool heading);
    void progress(int title, double done);
    void titleStarted(TitleStart s);
    void trackDone(TrackResult r);
    void titleDone(TitleResult r);
    void runResult(RunResult r);
};

// One disc of a run: what to rip of it and where.
struct RipJob {
    quint64     id;
    EngineDisc *disc;
    QByteArray  sel;       // per title: 1 rip
    QByteArray  split;     // per title: 1 as parts
    QString     folder;    // the disc's output folder before _001, _002, ... (engine_unique_folder())
};

class EngineWorker : public QObject {
    Q_OBJECT
public:
    // Stops the run: the title being written ends, no further title or disc
    // is ripped (any thread).
    void stop();

public slots:
    // Opens the disc at path; scanned() says how it went (disc 0: it cannot
    // be read, the log says why).
    void scan(quint64 id, QString path);
    // Rips the jobs' discs one after another.
    void rip(QList<RipJob> jobs);

signals:
    void scanStarted(quint64 id);
    void scanned(quint64 id, quintptr disc);   // EngineDisc *
    void ripStarted(quint64 id, QString folder);
    void ripDone(quint64 id, int result, bool stopped);
    void runDone(bool stopped);

private:
    std::atomic<bool> stopping { false };
};
