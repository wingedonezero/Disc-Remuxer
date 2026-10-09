#include "MainWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "Console.h"
#include "DetailsPanel.h"

extern "C" {
#include <libavutil/avutil.h>
}

enum Column { C_NAME, C_VIDEO, C_AUDIO, C_SUBS, C_CHAPTERS, C_DURATION, C_STATUS, C_PROGRESS, C_COUNT };
enum Role { R_JOB = Qt::UserRole, R_TITLE, R_TRACK };

MainWindow::MainWindow()
{
    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    auto *buttons = new QHBoxLayout;
    auto *btnImages = new QPushButton("Add Image(s)…");
    auto *btnFolders = new QPushButton("Add Folder(s)…");
    auto *btnRemove = new QPushButton("Remove Selected");
    auto *btnClear = new QPushButton("Clear");

    setWindowTitle(QString("Disc Remuxer %1").arg(DISC_REMUXER_VERSION));
    setAcceptDrops(true);

    btnPrefs = new QPushButton("Preferences…");
    btnStart = new QPushButton("Start Queue");
    btnStop = new QPushButton("Stop");
    btnPrefs->setEnabled(false);
    btnPrefs->setToolTip("Comes in a later build: every setting of the engine and the GUI's own");
    btnStart->setToolTip("Ripping comes in the next build");
    for (QPushButton *b : { btnImages, btnFolders, btnRemove, btnClear, btnPrefs, btnStart, btnStop })
        buttons->addWidget(b);
    buttons->addStretch();

    queueLabel = new QLabel;
    queueLabel->setStyleSheet("font-weight:600;");

    tree = new QTreeWidget;
    tree->setColumnCount(C_COUNT);
    tree->setHeaderLabels({ "Source / Title / Track", "Video", "Audio", "Subs", "Chapters", "Duration", "Status",
                            "Progress" });
    tree->header()->setSectionResizeMode(C_NAME, QHeaderView::Stretch);
    for (int c = 1; c < C_COUNT; c++)
        tree->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    tree->setUniformRowHeights(true);

    details = new DetailsPanel;
    console = new Console;

    centerSplit = new QSplitter(Qt::Horizontal);
    centerSplit->addWidget(tree);
    centerSplit->addWidget(details);
    centerSplit->setStretchFactor(0, 3);
    centerSplit->setStretchFactor(1, 2);
    mainSplit = new QSplitter(Qt::Vertical);
    mainSplit->addWidget(centerSplit);
    mainSplit->addWidget(console);
    mainSplit->setStretchFactor(0, 3);
    mainSplit->setStretchFactor(1, 1);

    layout->addWidget(queueLabel);
    layout->addLayout(buttons);
    layout->addWidget(mainSplit);
    setCentralWidget(central);
    resize(1400, 900);

    connect(btnImages, &QPushButton::clicked, this, &MainWindow::addImages);
    connect(btnFolders, &QPushButton::clicked, this, &MainWindow::addFolders);
    connect(btnRemove, &QPushButton::clicked, this, &MainWindow::removeSelected);
    connect(btnClear, &QPushButton::clicked, this, &MainWindow::clearAll);
    connect(tree, &QTreeWidget::itemChanged, this, &MainWindow::itemChanged);
    connect(tree, &QTreeWidget::currentItemChanged, this, &MainWindow::currentChanged);

    // the engine's log into the console; scans on the engine's thread
    relay = new LogRelay(this);
    connect(relay, &LogRelay::line, console, &Console::addLine);
    worker = new EngineWorker;
    worker->moveToThread(&engineThread);
    connect(&engineThread, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &MainWindow::scanRequested, worker, &EngineWorker::scan);
    connect(worker, &EngineWorker::scanStarted, this, &MainWindow::scanStarted);
    connect(worker, &EngineWorker::scanned, this, &MainWindow::scanned);
    engineThread.start();

    updateControls();
    updateQueueLabel();
}

MainWindow::~MainWindow()
{
    engineThread.quit();
    engineThread.wait();
    for (Job &j : jobs)
        engine_disc_close(&j.disc);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    // a scan that is running is finished first (a disc cannot be left half open)
    if (pendingScans)
        console->addLine(3, 0, QString(), "Waiting for the disc being scanned…", false);
    e->accept();
}

MainWindow::Job *MainWindow::jobById(quint64 id)
{
    for (Job &j : jobs)
        if (j.id == id)
            return &j;
    return nullptr;
}

MainWindow::Job *MainWindow::jobOf(QTreeWidgetItem *item)
{
    return item ? jobById(item->data(C_NAME, R_JOB).toULongLong()) : nullptr;
}

void MainWindow::addImages()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Disc images", QDir::homePath(),
                                                      "Disc images (*.iso *.img);;All files (*)");
    addPaths(files);
}

void MainWindow::addFolders()
{
    QString dir = QFileDialog::getExistingDirectory(this, "A disc folder, or a folder holding discs", QDir::homePath());
    if (!dir.isEmpty())
        addPaths({ dir });
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    QStringList paths;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile())
            paths << u.toLocalFile();
    addPaths(paths);
}

void MainWindow::addPaths(const QStringList &paths)
{
    for (const QString &p : paths) {
        const QList<FoundDisc> found = findDiscs(p);
        if (found.isEmpty())
            console->addLine(1, 0, QString(), QString("No disc found in %1").arg(p), false);
        for (const FoundDisc &f : found) {
            Job job;
            job.id = nextId++;
            job.found = f;
            job.item = new QTreeWidgetItem({ f.name, "", "", "", "", "", "Waiting to scan…", "" });
            job.item->setData(C_NAME, R_JOB, job.id);
            job.item->setData(C_NAME, R_TITLE, -1);
            job.item->setData(C_NAME, R_TRACK, -1);
            job.item->setToolTip(C_NAME, f.path);
            updating = true;
            job.item->setFlags(job.item->flags() | Qt::ItemIsUserCheckable);
            job.item->setCheckState(C_NAME, Qt::Checked);
            updating = false;
            tree->addTopLevelItem(job.item);
            auto *bar = new QProgressBar;
            bar->setRange(0, 0);    // busy until the scan ends
            bar->setFixedHeight(12);
            bar->setTextVisible(false);
            tree->setItemWidget(job.item, C_PROGRESS, bar);
            jobs.append(job);
            pendingScans++;
            emit scanRequested(job.id, f.path);
        }
    }
    updateControls();
    updateQueueLabel();
}

void MainWindow::scanStarted(quint64 id)
{
    if (Job *j = jobById(id))
        j->item->setText(C_STATUS, "Scanning…");
}

void MainWindow::scanned(quint64 id, quintptr handle)
{
    EngineDisc *disc = reinterpret_cast<EngineDisc *>(handle);
    Job *j = jobById(id);

    pendingScans--;
    if (!j) {   // removed while it was scanned
        engine_disc_close(&disc);
        updateControls();
        updateQueueLabel();
        return;
    }
    j->scanning = false;
    j->disc = disc;
    if (auto *bar = qobject_cast<QProgressBar *>(tree->itemWidget(j->item, C_PROGRESS))) {
        bar->setRange(0, 100);
        bar->setValue(0);
    }
    if (disc)
        fillDisc(*j);
    else {
        j->item->setText(C_STATUS, "Cannot be read (see the log)");
        updating = true;
        j->item->setCheckState(C_NAME, Qt::Unchecked);
        j->item->setFlags(j->item->flags() & ~Qt::ItemIsUserCheckable);
        updating = false;
    }
    updateControls();
    updateQueueLabel();
    if (!snapshot.isEmpty() && !pendingScans) {
        tree->setCurrentItem(j->item->childCount() ? j->item->child(0) : j->item);
        grab().save(snapshot);
        QApplication::quit();
    }
}

void MainWindow::snapshotWhenScanned(const QString &file)
{
    snapshot = file;
}

// The disc's titles and their tracks under its row: titles shorter than
// the minimum length unchecked; track 0 (the video, with a Dolby Vision
// layer) always ripped.
void MainWindow::fillDisc(Job &job)
{
    const DiscInfo *d = engine_disc_info(job.disc);
    int checked = 0;

    updating = true;
    job.item->setText(C_STATUS, QString("%1, %2 title%3").arg(QString::fromUtf8(d->kind)).arg(d->nb_titles)
                                    .arg(d->nb_titles == 1 ? "" : "s"));
    for (int t = 0; t < d->nb_titles; t++) {
        const DiscTitleInfo *ti = &d->titles[t];
        uint8_t *keep = engine_disc_tracks(job.disc, t);
        int na = 0, ns = 0;
        for (int i = 0; i < ti->nb_streams; i++) {
            na += ti->streams[i].type == AVMEDIA_TYPE_AUDIO;
            ns += ti->streams[i].type == AVMEDIA_TYPE_SUBTITLE;
        }
        QString name = QString("#%1").arg(t);
        if (ti->name && *ti->name)
            name += "  " + QString::fromUtf8(ti->name);
        auto *title = new QTreeWidgetItem(job.item, {
            name, ti->nb_streams && ti->streams[0].type == AVMEDIA_TYPE_VIDEO ?
                      QString::fromUtf8(engine_codec_label(ti->streams[0].codec, 0)) : "-",
            QString::number(na), QString::number(ns), QString::number(ti->nb_chapters), hms(ti->duration),
            ti->selected ? "" : "shorter than the minimum length", "" });
        title->setData(C_NAME, R_JOB, job.id);
        title->setData(C_NAME, R_TITLE, t);
        title->setData(C_NAME, R_TRACK, -1);
        title->setFlags(title->flags() | Qt::ItemIsUserCheckable);
        title->setCheckState(C_NAME, ti->selected ? Qt::Checked : Qt::Unchecked);
        checked += ti->selected;
        for (int i = 0; i < ti->nb_streams; i++) {
            const DiscStreamInfo *k = &ti->streams[i];
            char text[512];
            auto *track = new QTreeWidgetItem(title, {
                QString("%1  %2  %3  %4").arg(i).arg(av_get_media_type_string(k->type), -8)
                    .arg(k->language[0] ? QString::fromUtf8(k->language) : "   ")
                    .arg(QString::fromUtf8(engine_track_text(ti, i, 1, text, sizeof(text)))) });
            track->setData(C_NAME, R_JOB, job.id);
            track->setData(C_NAME, R_TITLE, t);
            track->setData(C_NAME, R_TRACK, i);
            track->setCheckState(C_NAME, keep[i] ? Qt::Checked : Qt::Unchecked);
            if (i == 0) {
                // the video: always ripped (its Dolby Vision layer with it)
                track->setFlags(track->flags() & ~Qt::ItemIsUserCheckable);
                track->setToolTip(C_NAME, "The video is always ripped");
            } else {
                track->setFlags(track->flags() | Qt::ItemIsUserCheckable);
            }
        }
    }
    job.item->setCheckState(C_NAME, !checked ? Qt::Unchecked : checked == d->nb_titles ? Qt::Checked :
                                    Qt::PartiallyChecked);
    job.item->setExpanded(true);
    updating = false;
}

void MainWindow::itemChanged(QTreeWidgetItem *item, int column)
{
    Job *j;

    if (updating || column != C_NAME || !(j = jobOf(item)) || !j->disc)
        return;
    int t = item->data(C_NAME, R_TITLE).toInt(), i = item->data(C_NAME, R_TRACK).toInt();
    bool on = item->checkState(C_NAME) == Qt::Checked;

    updating = true;
    if (i >= 0) {
        // a track: what the engine rips of its title
        engine_disc_tracks(j->disc, t)[i] = on;
    } else if (t >= 0) {
        // a title: the disc's box shows how many are checked
        int n = j->item->childCount(), c = 0;
        for (int k = 0; k < n; k++)
            c += j->item->child(k)->checkState(C_NAME) == Qt::Checked;
        j->item->setCheckState(C_NAME, !c ? Qt::Unchecked : c == n ? Qt::Checked : Qt::PartiallyChecked);
    } else {
        // the disc: all its titles on or off
        if (item->checkState(C_NAME) == Qt::PartiallyChecked)
            item->setCheckState(C_NAME, Qt::Checked);
        for (int k = 0; k < item->childCount(); k++)
            item->child(k)->setCheckState(C_NAME, item->checkState(C_NAME));
    }
    updating = false;
    if (tree->currentItem())
        currentChanged(tree->currentItem());
    updateQueueLabel();
}

void MainWindow::currentChanged(QTreeWidgetItem *cur)
{
    Job *j = jobOf(cur);

    if (!j || !j->disc) {
        details->clear();
        return;
    }
    const DiscInfo *d = engine_disc_info(j->disc);
    int t = cur->data(C_NAME, R_TITLE).toInt(), i = cur->data(C_NAME, R_TRACK).toInt();
    if (t < 0)
        details->showDisc(j->found.path, d);
    else if (i < 0)
        details->showTitle(&d->titles[t], engine_disc_tracks(j->disc, t));
    else
        details->showTrack(&d->titles[t], i, engine_disc_tracks(j->disc, t));
}

void MainWindow::removeJob(int row)
{
    QTreeWidgetItem *item = tree->takeTopLevelItem(row);
    Job *j = jobOf(item);

    if (j) {
        // a disc still being scanned is closed when its scan ends
        quint64 id = j->id;
        engine_disc_close(&j->disc);
        jobs.removeIf([id](const Job &x) { return x.id == id; });
    }
    delete item;
}

void MainWindow::removeSelected()
{
    QTreeWidgetItem *item = tree->currentItem();
    while (item && item->parent())
        item = item->parent();
    if (item)
        removeJob(tree->indexOfTopLevelItem(item));
    details->clear();
    updateControls();
    updateQueueLabel();
}

void MainWindow::clearAll()
{
    while (tree->topLevelItemCount())
        removeJob(0);
    details->clear();
    console->clear();
    updateControls();
    updateQueueLabel();
}

void MainWindow::updateControls()
{
    btnStart->setEnabled(false);    // ripping: the next build
    btnStop->setEnabled(false);
}

void MainWindow::updateQueueLabel()
{
    int64_t bytes = 0;

    for (const Job &j : jobs) {
        if (!j.disc)
            continue;
        const DiscInfo *d = engine_disc_info(j.disc);
        for (int t = 0; t < j.item->childCount() && t < d->nb_titles; t++)
            if (j.item->child(t)->checkState(C_NAME) == Qt::Checked)
                bytes += d->titles[t].size;
    }
    QString text = QString("Queue: %1 disc%2, checked titles %3").arg(jobs.size()).arg(jobs.size() == 1 ? "" : "s")
                       .arg(bytes ? sizeText(bytes) : "0 MB");
    if (pendingScans)
        text += QString(" — scanning %1 disc%2…").arg(pendingScans).arg(pendingScans == 1 ? "" : "s");
    queueLabel->setText(text);
}
