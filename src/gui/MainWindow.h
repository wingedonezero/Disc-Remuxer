// The window: the queue of discs (each disc's titles and tracks with check
// boxes), the details of what is selected, the log.
#pragma once

#include <QList>
#include <QMainWindow>
#include <QThread>

#include "DiscFinder.h"
#include "EngineWorker.h"

class Console;
class DetailsPanel;
class QLabel;
class QPushButton;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    // Discs found in paths (images, disc folders, folders holding discs)
    // into the queue, each scanned.
    void addPaths(const QStringList &paths);
    // Tests: the window saved as a picture into file once every scan has
    // ended, then the program quits.
    void snapshotWhenScanned(const QString &file);

signals:
    void scanRequested(quint64 id, QString path);

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private slots:
    void addImages();
    void addFolders();
    void removeSelected();
    void clearAll();
    void scanStarted(quint64 id);
    void scanned(quint64 id, quintptr disc);
    void itemChanged(QTreeWidgetItem *item, int column);
    void currentChanged(QTreeWidgetItem *cur);

private:
    struct Job {
        quint64          id;
        FoundDisc        found;
        EngineDisc      *disc = nullptr;
        QTreeWidgetItem *item = nullptr;
        bool             scanning = true;
    };
    Job *jobOf(QTreeWidgetItem *item);
    Job *jobById(quint64 id);
    void fillDisc(Job &job);
    void removeJob(int row);
    void updateControls();
    void updateQueueLabel();

    QList<Job>     jobs;
    quint64        nextId = 1;
    int            pendingScans = 0;
    bool           updating = false;
    QString        snapshot;

    QLabel        *queueLabel;
    QTreeWidget   *tree;
    DetailsPanel  *details;
    Console       *console;
    QSplitter     *centerSplit, *mainSplit;
    QPushButton   *btnStart, *btnStop, *btnPrefs;

    LogRelay      *relay;
    QThread        engineThread;
    EngineWorker  *worker;
};
