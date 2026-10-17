// The window: the queue of discs (each disc's titles and tracks with check
// boxes), the details of what is selected, the log; menus, ripping.
#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QMainWindow>
#include <QThread>
#include <QVector>

#include "DiscFinder.h"
#include "EngineWorker.h"

class Console;
class DetailsPanel;
class QAction;
class QLabel;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTreeWidgetItem;
class QueueTree;

class QComboBox;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // relay: the engine's log, set up before the settings were read
    explicit MainWindow(LogRelay *relay);
    ~MainWindow() override;

    // Discs found in paths (images, disc folders, folders holding discs)
    // into the queue, each scanned.
    void addPaths(const QStringList &paths);
    // Tests: the window saved as a picture into file once every scan has
    // ended (test "rip": once the queue, started then, has run; "stop": the
    // same, Stop pressed a second after the start), then the program quits.
    void snapshotWhenScanned(const QString &file, const QString &test);

signals:
    void scanRequested(quint64 id, QString path);

protected:
    void closeEvent(QCloseEvent *e) override;

private slots:
    void addImages();
    void addFolders();
    void removeSelected();
    void clearAll();
    void rescanSelected();
    void openPrefs();
    void syncFormatBox();
    void startQueue();
    void stopQueue();
    void scanStarted(quint64 id);
    void scanned(quint64 id, quintptr disc);
    void itemChanged(QTreeWidgetItem *item, int column);
    void currentChanged(QTreeWidgetItem *cur);
    void contextMenu(const QPoint &pos);
    void moveDisc(int from, int to);
    void ripStarted(quint64 id, QString folder);
    void ripDone(quint64 id, int result, bool stopped);
    void runDone(bool stopped);
    void onTitleStarted(TitleStart s);
    void onTitleDone(TitleResult r);
    void onRunResult(RunResult r);
    void onProgress(int title, double done);

private:
    struct Job {
        quint64          id;
        FoundDisc        found;
        EngineDisc      *disc = nullptr;
        QTreeWidgetItem *item = nullptr;
        bool             scanning = true;
        QVector<QVector<QTreeWidgetItem *>> tracks;  // per title: its tracks' rows
        QString          outDir;     // where its last run wrote
        QString          logFile;
    };
    Job *jobOf(QTreeWidgetItem *item);
    Job *ripping();
    // the open disc's titles (0 while it is scanned or cannot be read)
    static int titles(const Job &j);
    Job *jobById(quint64 id);
    void queueScan(Job &job);
    void fillDisc(Job &job);
    void showTitleChecks(Job &job, int t);
    void showDiscCheck(Job &job);
    void removeJob(int row);
    QProgressBar *barOf(QTreeWidgetItem *item);
    void newBar(QTreeWidgetItem *item, int value);
    QString outputFolder(const Job &job) const;
    void updateControls();
    void updateQueueLabel();
    void loadState();
    void saveState();

    QList<Job>     jobs;
    quint64        nextId = 1;
    int            pendingScans = 0;
    bool           updating = false;
    bool           running = false;
    bool           stopping = false;
    QString        snapshot, test;

    // the run: the disc being ripped, its title being written
    quint64        ripId = 0;
    int            ripTitle = -1, ripOrdinal = 0, ripCount = 0;
    QElapsedTimer  titleClock;

    QLabel        *queueLabel;
    QueueTree     *tree;
    DetailsPanel  *details;
    Console       *console;
    QSplitter     *centerSplit, *mainSplit;
    QPushButton   *btnStart, *btnStop, *btnRemove, *btnClear;
    QComboBox     *formatBox;
    QAction       *actStart, *actStop, *actRemove, *actClear, *actRescan;

    LogRelay      *relay;
    QThread        engineThread;
    EngineWorker  *worker;
};
