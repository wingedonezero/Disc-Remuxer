// The engine on a thread of its own: discs are opened (scanned) and ripped
// there one after another, so the window never waits for a disc. The
// engine's log, progress and records come back as signals (LogRelay),
// whichever thread wrote them.
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>

#include <atomic>

extern "C" {
#include "engine.h"
}

// Receives the engine's log lines, progress and JSON records
// (engine_set_sink) and gives them to the window as queued signals.
class LogRelay : public QObject {
    Q_OBJECT
public:
    explicit LogRelay(QObject *parent = nullptr);
    ~LogRelay() override;

signals:
    void line(int level, int code, QString source, QString text, bool heading);
    void progress(int title, double done);
    void record(QString json);
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
