// The engine on a thread of its own: discs are opened (scanned) there one
// after another, so the window never waits for a disc. The engine's log
// comes back as signals (LogRelay), whichever thread wrote it.
#pragma once

#include <QObject>
#include <QString>

extern "C" {
#include "engine.h"
}

// Receives the engine's log lines and progress (engine_set_sink) and gives
// them to the window as queued signals.
class LogRelay : public QObject {
    Q_OBJECT
public:
    explicit LogRelay(QObject *parent = nullptr);
    ~LogRelay() override;

signals:
    void line(int level, int code, QString source, QString text, bool heading);
    void progress(int title, double done);
};

class EngineWorker : public QObject {
    Q_OBJECT
public slots:
    // Opens the disc at path; scanned() says how it went (disc null: it
    // cannot be read, the log says why).
    void scan(quint64 id, QString path);

signals:
    void scanStarted(quint64 id);
    void scanned(quint64 id, quintptr disc);   // EngineDisc *
};
