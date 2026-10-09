#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "Console.h"
#include "DetailsPanel.h"
#include "PrefsDialog.h"
#include "QueueTree.h"

extern "C" {
#include <libavutil/avutil.h>
#include "log.h"
#include "settings.h"
}

enum Column { C_NAME, C_VIDEO, C_AUDIO, C_SUBS, C_CHAPTERS, C_DURATION, C_STATUS, C_PROGRESS, C_COUNT };
enum Role { R_JOB = Qt::UserRole, R_TITLE, R_TRACK };

static const int defaultWidths[C_COUNT] = { 460, 70, 50, 45, 65, 70, 300, 150 };

MainWindow::MainWindow()
{
    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    auto *buttons = new QHBoxLayout;
    auto *btnImages = new QPushButton("Add Image(s)…");
    auto *btnFolders = new QPushButton("Add Folder(s)…");
    auto *btnPrefs = new QPushButton("Preferences…");

    setWindowTitle(QString("Disc Remuxer %1").arg(DISC_REMUXER_VERSION));

    btnRemove = new QPushButton("Remove Selected");
    btnClear = new QPushButton("Clear");
    btnStart = new QPushButton("Start Queue");
    btnStop = new QPushButton("Stop");
    for (QPushButton *b : { btnImages, btnFolders, btnRemove, btnClear, btnPrefs, btnStart, btnStop })
        buttons->addWidget(b);
    buttons->addStretch();

    queueLabel = new QLabel;
    queueLabel->setStyleSheet("font-weight:600;");

    tree = new QueueTree;
    tree->setColumnCount(C_COUNT);
    tree->setHeaderLabels({ "Source / Title / Track", "Video", "Audio", "Subs", "Chapters", "Duration", "Status",
                            "Progress" });
    // columns keep their width (the user's) whatever rows are opened
    tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree->header()->setStretchLastSection(false);
    for (int c = 0; c < C_COUNT; c++)
        tree->setColumnWidth(c, defaultWidths[c]);
    tree->setUniformRowHeights(true);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    tree->setTextElideMode(Qt::ElideRight);

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
    resize(1500, 950);

    // menus
    QMenu *file = menuBar()->addMenu("&File");
    file->addAction("Add &Image(s)…", QKeySequence::Open, this, &MainWindow::addImages);
    file->addAction("Add &Folder(s)…", QKeySequence("Ctrl+Shift+O"), this, &MainWindow::addFolders);
    file->addSeparator();
    file->addAction("Open the &Output Folder", this, [] {
        char root[4096];
        engine_output_root(root, sizeof(root));
        QDir().mkpath(QString::fromUtf8(root));
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(root)));
    });
    file->addAction("Open the &keydb Folder", this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(program_dir()) + "/keydb"));
    });
    file->addSeparator();
    file->addAction("&Quit", QKeySequence::Quit, this, &QWidget::close);
    QMenu *queue = menuBar()->addMenu("&Queue");
    actStart = queue->addAction("&Start", QKeySequence("F5"), this, &MainWindow::startQueue);
    actStop = queue->addAction("S&top", this, &MainWindow::stopQueue);
    queue->addSeparator();
    actRescan = queue->addAction("Scan the Selected Disc &Again", this, &MainWindow::rescanSelected);
    actRemove = queue->addAction("&Remove the Selected Disc", QKeySequence::Delete, this,
                                 &MainWindow::removeSelected);
    actClear = queue->addAction("&Clear", this, &MainWindow::clearAll);
    QMenu *settings = menuBar()->addMenu("&Settings");
    settings->addAction("&Preferences…", QKeySequence::Preferences, this, &MainWindow::openPrefs);
    QMenu *help = menuBar()->addMenu("&Help");
    help->addAction("&About", this, [this] {
        QMessageBox::about(this, "Disc Remuxer", QString("Disc Remuxer %1\n\nRips DVD, HD DVD and Blu-ray titles into "
                                                         "stream files.\nSettings: %2")
                                                     .arg(DISC_REMUXER_VERSION, QString::fromUtf8(settings_path())));
    });

    connect(btnImages, &QPushButton::clicked, this, &MainWindow::addImages);
    connect(btnFolders, &QPushButton::clicked, this, &MainWindow::addFolders);
    connect(btnRemove, &QPushButton::clicked, this, &MainWindow::removeSelected);
    connect(btnClear, &QPushButton::clicked, this, &MainWindow::clearAll);
    connect(btnPrefs, &QPushButton::clicked, this, &MainWindow::openPrefs);
    connect(btnStart, &QPushButton::clicked, this, &MainWindow::startQueue);
    connect(btnStop, &QPushButton::clicked, this, &MainWindow::stopQueue);
    connect(tree, &QTreeWidget::itemChanged, this, &MainWindow::itemChanged);
    connect(tree, &QTreeWidget::currentItemChanged, this, &MainWindow::currentChanged);
    connect(tree, &QWidget::customContextMenuRequested, this, &MainWindow::contextMenu);
    connect(tree, &QueueTree::pathsDropped, this, &MainWindow::addPaths);
    connect(tree, &QueueTree::moveRequested, this, &MainWindow::moveDisc);

    // the engine's log into the console; scans and rips on the engine's thread
    relay = new LogRelay(this);
    connect(relay, &LogRelay::line, console, &Console::addLine);
    connect(relay, &LogRelay::record, this, &MainWindow::onRecord);
    connect(relay, &LogRelay::progress, this, &MainWindow::onProgress);
    worker = new EngineWorker;
    worker->moveToThread(&engineThread);
    connect(&engineThread, &QThread::finished, worker, &QObject::deleteLater);
    connect(this, &MainWindow::scanRequested, worker, &EngineWorker::scan);
    connect(worker, &EngineWorker::scanStarted, this, &MainWindow::scanStarted);
    connect(worker, &EngineWorker::scanned, this, &MainWindow::scanned);
    connect(worker, &EngineWorker::ripStarted, this, &MainWindow::ripStarted);
    connect(worker, &EngineWorker::ripDone, this, &MainWindow::ripDone);
    connect(worker, &EngineWorker::runDone, this, &MainWindow::runDone);
    engineThread.start();

    loadState();
    updateControls();
    updateQueueLabel();
}

MainWindow::~MainWindow()
{
    worker->stop();
    engineThread.quit();
    engineThread.wait();
    for (Job &j : jobs)
        engine_disc_close(&j.disc);
}

// ---- the window's sizes, kept in the settings file ([gui]) ----

void MainWindow::loadState()
{
    QByteArray geo = QByteArray::fromBase64(QByteArray(setting_text("gui.window")));
    const QStringList cols = QString::fromUtf8(setting_text("gui.columns")).split(',', Qt::SkipEmptyParts);
    const QStringList panes = QString::fromUtf8(setting_text("gui.panes")).split(',', Qt::SkipEmptyParts);

    if (!geo.isEmpty())
        restoreGeometry(geo);
    if (cols.size() == C_COUNT)
        for (int c = 0; c < C_COUNT; c++)
            tree->setColumnWidth(c, qMax(20, cols[c].toInt()));
    if (panes.size() == 4) {
        centerSplit->setSizes({ panes[0].toInt(), panes[1].toInt() });
        mainSplit->setSizes({ panes[2].toInt(), panes[3].toInt() });
    }
}

void MainWindow::saveState()
{
    QStringList cols, panes;
    char err[256];

    for (int c = 0; c < C_COUNT; c++)
        cols << QString::number(tree->columnWidth(c));
    for (int s : centerSplit->sizes())
        panes << QString::number(s);
    for (int s : mainSplit->sizes())
        panes << QString::number(s);
    settings_store("gui.window", saveGeometry().toBase64().constData(), err, sizeof(err));
    settings_store("gui.columns", cols.join(',').toUtf8().constData(), err, sizeof(err));
    settings_store("gui.panes", panes.join(',').toUtf8().constData(), err, sizeof(err));
    settings_save();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (running &&
        QMessageBox::question(this, "Disc Remuxer", "A rip is running. Stop it and quit? The title being written "
                                                    "is left incomplete.") != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    saveState();
    e->accept();
}

// ---- the queue ----

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

static QString lastFolder()
{
    QString f = QString::fromUtf8(setting_text("gui.last_folder"));
    return f.isEmpty() ? QDir::homePath() : f;
}

static void keepLastFolder(const QString &dir)
{
    char err[256];
    settings_store("gui.last_folder", dir.toUtf8().constData(), err, sizeof(err));
}

void MainWindow::addImages()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Disc images", lastFolder(),
                                                      "Disc images (*.iso *.img);;All files (*)");
    if (!files.isEmpty())
        keepLastFolder(QFileInfo(files[0]).absolutePath());
    addPaths(files);
}

void MainWindow::addFolders()
{
    QString dir = QFileDialog::getExistingDirectory(this, "A disc folder, or a folder holding discs", lastFolder());
    if (!dir.isEmpty()) {
        keepLastFolder(QFileInfo(dir).absolutePath());
        addPaths({ dir });
    }
}

QProgressBar *MainWindow::barOf(QTreeWidgetItem *item)
{
    return qobject_cast<QProgressBar *>(tree->itemWidget(item, C_PROGRESS));
}

void MainWindow::newBar(QTreeWidgetItem *item, int value)
{
    auto *bar = new QProgressBar;
    bar->setFixedHeight(14);
    bar->setRange(0, value < 0 ? 0 : 100);    // < 0: busy
    bar->setValue(qMax(0, value));
    tree->setItemWidget(item, C_PROGRESS, bar);
}

void MainWindow::queueScan(Job &job)
{
    job.scanning = true;
    job.item->setText(C_STATUS, "Waiting to scan…");
    newBar(job.item, -1);
    pendingScans++;
    emit scanRequested(job.id, job.found.path);
}

void MainWindow::addPaths(const QStringList &paths)
{
    for (const QString &p : paths) {
        const QList<FoundDisc> found = findDiscs(p);
        if (found.isEmpty())
            console->addLine(LOG_WARNING, 0, QString(), QString("No disc found in %1").arg(p), false);
        for (const FoundDisc &f : found) {
            Job job;
            job.id = nextId++;
            job.found = f;
            job.item = new QTreeWidgetItem({ f.name, "", "", "", "", "", "", "" });
            job.item->setData(C_NAME, R_JOB, job.id);
            job.item->setData(C_NAME, R_TITLE, -1);
            job.item->setData(C_NAME, R_TRACK, -1);
            job.item->setToolTip(C_NAME, f.path);
            job.item->setFlags((job.item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
            tree->addTopLevelItem(job.item);
            jobs.append(job);
            queueScan(jobs.last());
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
    } else {
        j->scanning = false;
        j->disc = disc;
        newBar(j->item, 0);
        if (disc) {
            fillDisc(*j);
        } else {
            j->item->setText(C_STATUS, "Cannot be read (see the log)");
            updating = true;
            j->item->setData(C_NAME, Qt::CheckStateRole, QVariant());
            updating = false;
        }
    }
    updateControls();
    updateQueueLabel();
    if (!snapshot.isEmpty() && !pendingScans) {
        if (j && j->item->childCount())
            tree->setCurrentItem(j->item->child(0));
        if (test == "prefs") {
            PrefsDialog dlg(this);
            dlg.show();
            dlg.grab().save(snapshot);
            QApplication::quit();
            return;
        }
        if (test == "uncheck" && j) {
            // the disc unchecked: its titles' tracks greyed, unchecked
            j->item->setCheckState(C_NAME, Qt::Unchecked);
            if (j->item->childCount())
                j->item->child(0)->setExpanded(true);
        }
        if (test == "rip" || test == "stop") {
            startQueue();
            if (test == "stop")
                QTimer::singleShot(1000, this, &MainWindow::stopQueue);
            return;
        }
        grab().save(snapshot);
        QApplication::quit();
    }
}

void MainWindow::snapshotWhenScanned(const QString &file, const QString &t)
{
    snapshot = file;
    test = t;
}

// A title's tracks as its choice stands: an unchecked title's tracks are
// unchecked and cannot be changed (their own choice comes back with the
// title); the video is always ripped.
void MainWindow::showTitleChecks(Job &job, int t)
{
    QTreeWidgetItem *title = job.item->child(t);
    const uint8_t *keep = engine_disc_tracks(job.disc, t);
    const DiscTitleInfo *ti = &engine_disc_info(job.disc)->titles[t];
    bool on = job.sel[t];
    QBrush grey = tree->palette().brush(QPalette::Disabled, QPalette::Text);

    updating = true;
    title->setCheckState(C_NAME, on ? Qt::Checked : Qt::Unchecked);
    QString state = ti->selected ? "" : "shorter than the minimum length";
    if (job.split[t])
        state = QString("written as %1 parts").arg(ti->nb_parts);
    title->setText(C_STATUS, state);
    for (int i = 0; i < title->childCount(); i++) {
        QTreeWidgetItem *track = title->child(i);
        bool changeable = on && i != 0 && !running;
        track->setCheckState(C_NAME, on && keep[i] ? Qt::Checked : Qt::Unchecked);
        track->setFlags(changeable ? track->flags() | Qt::ItemIsUserCheckable : track->flags() & ~Qt::ItemIsUserCheckable);
        track->setForeground(C_NAME, on ? tree->palette().brush(QPalette::Text) : grey);
    }
    updating = false;
}

void MainWindow::showDiscCheck(Job &job)
{
    int n = job.sel.size(), c = 0;
    for (char s : job.sel)
        c += s != 0;
    updating = true;
    job.item->setCheckState(C_NAME, !c ? Qt::Unchecked : c == n ? Qt::Checked : Qt::PartiallyChecked);
    updating = false;
}

// The disc's titles and their tracks under its row: titles shorter than
// the minimum length unchecked; track 0 (the video, with a Dolby Vision
// layer) always ripped.
void MainWindow::fillDisc(Job &job)
{
    const DiscInfo *d = engine_disc_info(job.disc);

    updating = true;
    job.item->setFlags(job.item->flags() | Qt::ItemIsUserCheckable);
    job.item->setText(C_STATUS, QString("%1, %2 title%3").arg(QString::fromUtf8(d->kind)).arg(d->nb_titles)
                                    .arg(d->nb_titles == 1 ? "" : "s"));
    job.sel.fill(0, d->nb_titles);
    job.split.fill(0, d->nb_titles);
    for (int t = 0; t < d->nb_titles; t++) {
        const DiscTitleInfo *ti = &d->titles[t];
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
            QString::number(na), QString::number(ns), QString::number(ti->nb_chapters), hms(ti->duration), "", "" });
        title->setData(C_NAME, R_JOB, job.id);
        title->setData(C_NAME, R_TITLE, t);
        title->setData(C_NAME, R_TRACK, -1);
        title->setFlags((title->flags() | Qt::ItemIsUserCheckable) & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
        job.sel[t] = ti->selected;
        for (int i = 0; i < ti->nb_streams; i++) {
            const DiscStreamInfo *k = &ti->streams[i];
            char text[512];
            QString line = QString("%1  %2  %3  %4").arg(i).arg(av_get_media_type_string(k->type))
                               .arg(k->language[0] ? QString::fromUtf8(k->language) : "---")
                               .arg(QString::fromUtf8(engine_track_text(ti, i, 1, text, sizeof(text))));
            auto *track = new QTreeWidgetItem(title, { line });
            track->setData(C_NAME, R_JOB, job.id);
            track->setData(C_NAME, R_TITLE, t);
            track->setData(C_NAME, R_TRACK, i);
            track->setToolTip(C_NAME, i == 0 ? line + "\nThe video is always ripped" : line);
            track->setFlags(track->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
        }
    }
    updating = false;
    for (int t = 0; t < d->nb_titles; t++)
        showTitleChecks(job, t);
    showDiscCheck(job);
    job.item->setExpanded(true);
}

void MainWindow::itemChanged(QTreeWidgetItem *item, int column)
{
    Job *j;

    if (updating || column != C_NAME || !(j = jobOf(item)) || !j->disc)
        return;
    int t = item->data(C_NAME, R_TITLE).toInt(), i = item->data(C_NAME, R_TRACK).toInt();
    bool on = item->checkState(C_NAME) != Qt::Unchecked;

    if (running) {
        // the choice of a disc in the run stays as it was
        for (int k = 0; k < j->sel.size(); k++)
            showTitleChecks(*j, k);
        showDiscCheck(*j);
        return;
    }
    if (i >= 0) {
        // a track: what the engine rips of its title
        engine_disc_tracks(j->disc, t)[i] = on;
    } else if (t >= 0) {
        j->sel[t] = on;
        showTitleChecks(*j, t);
    } else {
        // the disc: all its titles on or off
        for (int k = 0; k < j->sel.size(); k++) {
            j->sel[k] = on;
            showTitleChecks(*j, k);
        }
    }
    showDiscCheck(*j);
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

void MainWindow::moveDisc(int from, int to)
{
    if (running)
        return;
    QTreeWidgetItem *item = tree->topLevelItem(from);
    QProgressBar *bar = barOf(item);
    int value = bar ? (bar->maximum() ? bar->value() : -1) : 0;
    bool open = item->isExpanded();

    tree->takeTopLevelItem(from);
    tree->insertTopLevelItem(to > from ? to - 1 : to, item);
    newBar(item, value);
    item->setExpanded(open);
    tree->setCurrentItem(item);
}

void MainWindow::contextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = tree->itemAt(pos);
    Job *j = jobOf(item);
    QMenu menu(this);

    if (!j)
        return;
    int t = item->data(C_NAME, R_TITLE).toInt();
    const QString out = j->outDir, log = j->logFile;
    if (t >= 0 && j->disc && engine_disc_info(j->disc)->titles[t].nb_parts) {
        int parts = engine_disc_info(j->disc)->titles[t].nb_parts;
        QAction *a = menu.addAction(QString("Write as %1 Parts (One per Stream File)").arg(parts));
        a->setCheckable(true);
        a->setChecked(j->split[t]);
        a->setEnabled(!running);
        connect(a, &QAction::toggled, this, [this, id = j->id, t](bool on) {
            if (Job *x = jobById(id)) {
                x->split[t] = on;
                showTitleChecks(*x, t);
            }
        });
        menu.addSeparator();
    }
    menu.addAction("Open the Output Folder", this, [out] { QDesktopServices::openUrl(QUrl::fromLocalFile(out)); })
        ->setEnabled(!out.isEmpty() && QFileInfo(out).isDir());
    menu.addAction("Open the Log", this, [log] { QDesktopServices::openUrl(QUrl::fromLocalFile(log)); })
        ->setEnabled(!log.isEmpty() && QFileInfo(log).isFile());
    menu.addAction("Show the Source", this, [src = j->found.path] {
        QFileInfo fi(src);
        QDesktopServices::openUrl(QUrl::fromLocalFile(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath()));
    });
    menu.addSeparator();
    menu.addAction("Scan Again", this, [this, item] { tree->setCurrentItem(item); rescanSelected(); })
        ->setEnabled(!running && !j->scanning);
    menu.addAction("Remove from the Queue", this, [this, item] { tree->setCurrentItem(item); removeSelected(); })
        ->setEnabled(!running);
    menu.exec(tree->viewport()->mapToGlobal(pos));
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
    if (running)
        return;
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
    if (running)
        return;
    while (tree->topLevelItemCount())
        removeJob(0);
    details->clear();
    console->clear();
    updateControls();
    updateQueueLabel();
}

// The disc read again (after settings that change how discs are read).
void MainWindow::rescanSelected()
{
    QTreeWidgetItem *item = tree->currentItem();
    Job *j;

    while (item && item->parent())
        item = item->parent();
    if (running || !(j = jobOf(item)) || j->scanning)
        return;
    engine_disc_close(&j->disc);
    updating = true;
    qDeleteAll(item->takeChildren());
    item->setData(C_NAME, Qt::CheckStateRole, QVariant());
    updating = false;
    j->sel.clear();
    j->split.clear();
    details->clear();
    queueScan(*j);
    updateControls();
    updateQueueLabel();
}

void MainWindow::openPrefs()
{
    PrefsDialog dlg(this);
    QStringList changed;

    while (dlg.exec() == QDialog::Accepted) {
        if (dlg.apply(&changed)) {
            for (const QString &c : changed)
                console->addLine(LOG_INFO, 0, QString(), "Setting kept: " + c, false);
            break;
        }
        changed.clear();
    }
}

// ---- the run ----

// <main output folder>/<the disc's name>; with gui.keep_folder_structure a
// disc found below an added folder: <main>/<added folder>/<folders>/<name>.
QString MainWindow::outputFolder(const Job &job) const
{
    char root[4096], clean[1024];
    QString path;

    engine_output_root(root, sizeof(root));
    path = QString::fromUtf8(root);
    if (setting_bool("gui.keep_folder_structure") && job.found.relative != ".") {
        QStringList parts = job.found.relative.split('/', Qt::SkipEmptyParts);
        parts.removeLast();     // the disc itself: its name below
        parts.prepend(QFileInfo(job.found.dropRoot).fileName());
        for (const QString &p : parts) {
            engine_clean_name(p.toUtf8().constData(), clean, sizeof(clean));
            path += "/" + QString::fromUtf8(clean);
        }
    }
    engine_clean_name(job.found.name.toUtf8().constData(), clean, sizeof(clean));
    return path + "/" + QString::fromUtf8(clean);
}

void MainWindow::startQueue()
{
    QList<RipJob> run;

    if (running || pendingScans)
        return;
    for (int r = 0; r < tree->topLevelItemCount(); r++) {
        Job *j = jobOf(tree->topLevelItem(r));
        if (!j || !j->disc || !j->sel.contains(1))
            continue;
        run.append({ j->id, j->disc, QByteArray(j->sel.constData(), j->sel.size()),
                     QByteArray(j->split.constData(), j->split.size()), outputFolder(*j) });
        j->item->setText(C_STATUS, "Waiting");
        newBar(j->item, 0);
    }
    if (run.isEmpty()) {
        console->addLine(LOG_WARNING, 0, QString(), "No title is checked: nothing to rip", false);
        return;
    }
    running = true;
    stopping = false;
    tree->reorderable = false;
    for (Job &j : jobs)
        for (int t = 0; j.disc && t < j.sel.size(); t++)
            showTitleChecks(j, t);
    updateControls();
    QMetaObject::invokeMethod(worker, [w = worker, run] { w->rip(run); }, Qt::QueuedConnection);
}

void MainWindow::stopQueue()
{
    if (!running || stopping)
        return;
    stopping = true;
    worker->stop();
    console->addLine(LOG_WARNING, 0, QString(), "Stopping: the title being written ends now", false);
    updateControls();
}

void MainWindow::ripStarted(quint64 id, QString folder)
{
    Job *j = jobById(id);

    ripId = id;
    ripTitle = -1;
    if (!j)
        return;
    j->outDir = folder;
    j->item->setText(C_STATUS, "Ripping…");
}

void MainWindow::onRecord(QString json)
{
    QJsonObject r = QJsonDocument::fromJson(json.toUtf8()).object();
    QString type = r.value("type").toString();
    Job *j = jobById(ripId);

    if (!j || !running || !j->disc)
        return;
    int t = r.value("title").toInt(-1);
    QTreeWidgetItem *title = t >= 0 && t < j->item->childCount() ? j->item->child(t) : nullptr;
    QString part = r.contains("part") ? QString(" part %1").arg(r.value("part").toInt()) : QString();

    if (type == "result") {
        j->logFile = r.value("log").toString();
    } else if (type == "title_start" && title) {
        ripTitle = t;
        ripOrdinal = r.value("ordinal").toInt();
        ripCount = r.value("count").toInt();
        titleClock.start();
        title->setText(C_STATUS, "Writing" + part + "…");
        j->item->setText(C_STATUS, QString("Title #%1%2 (%3 of %4)").arg(t).arg(part).arg(ripOrdinal).arg(ripCount));
    } else if (type == "title_result" && title) {
        QString st = r.value("status").toString();
        int warnings = r.value("warnings").toInt();
        if (st == "ok")
            title->setText(C_STATUS, "Done" + part + (warnings ? QString(", %1 warning(s)").arg(warnings) : ""));
        else if (st == "stopped")
            title->setText(C_STATUS, "Stopped" + part + ": not complete");
        else
            title->setText(C_STATUS, "Failed" + part + " (see the log)");
    }
}

void MainWindow::onProgress(int t, double done)
{
    Job *j = jobById(ripId);

    if (!j || !running || !j->disc || t != ripTitle || t >= j->item->childCount())
        return;
    const DiscTitleInfo *ti = &engine_disc_info(j->disc)->titles[t];
    QString text = QString("Writing %1%").arg(int(done * 100));
    double secs = titleClock.elapsed() / 1000.0;
    // speed and time left from the title's size (a whole title only)
    if (secs > 2 && done > 0.001 && !j->split[t]) {
        double rate = done * ti->size / secs;
        text += QString(" · %1 MB/s · %2 left").arg(rate / (1 << 20), 0, 'f', 1)
                    .arg(hms(int64_t((1 - done) / done * secs * 1e6)));
    }
    j->item->child(t)->setText(C_STATUS, text);
    if (QProgressBar *bar = barOf(j->item))
        bar->setValue(int(100 * ((ripOrdinal - 1) + done) / qMax(1, ripCount)));
}

void MainWindow::ripDone(quint64 id, int result, bool stopped)
{
    Job *j = jobById(id);

    if (!j)
        return;
    j->item->setText(C_STATUS, stopped ? "Stopped" : result ? "Done with failures (see the log)" : "Done");
    if (QProgressBar *bar = barOf(j->item))
        bar->setValue(stopped ? bar->value() : 100);
    ripId = 0;
}

void MainWindow::runDone(bool stopped)
{
    running = false;
    stopping = false;
    tree->reorderable = true;
    for (Job &j : jobs)
        for (int t = 0; j.disc && t < j.sel.size(); t++) {
            QString keep = j.item->child(t)->text(C_STATUS);
            showTitleChecks(j, t);
            j.item->child(t)->setText(C_STATUS, keep);     // the run's result stays shown
        }
    console->addLine(stopped ? LOG_WARNING : LOG_OK, 0, QString(), stopped ? "The queue was stopped" :
                     "The queue is done", true);
    updateControls();
    if (!snapshot.isEmpty() && !test.isEmpty()) {
        grab().save(snapshot);
        QApplication::quit();
    }
}

void MainWindow::updateControls()
{
    bool canStart = !running && !pendingScans;
    btnStart->setEnabled(canStart);
    actStart->setEnabled(canStart);
    btnStop->setEnabled(running && !stopping);
    actStop->setEnabled(running && !stopping);
    btnRemove->setEnabled(!running);
    actRemove->setEnabled(!running);
    btnClear->setEnabled(!running);
    actClear->setEnabled(!running);
    actRescan->setEnabled(!running);
    btnStart->setToolTip(pendingScans ? "Discs are still being scanned" : "Rip the checked titles of every disc");
}

void MainWindow::updateQueueLabel()
{
    int64_t bytes = 0;
    int titles = 0;

    for (const Job &j : jobs) {
        if (!j.disc)
            continue;
        const DiscInfo *d = engine_disc_info(j.disc);
        for (int t = 0; t < j.sel.size() && t < d->nb_titles; t++)
            if (j.sel[t]) {
                bytes += d->titles[t].size;
                titles++;
            }
    }
    QString text = QString("Queue: %1 disc%2, %3 title%4 checked (%5)").arg(jobs.size())
                       .arg(jobs.size() == 1 ? "" : "s").arg(titles).arg(titles == 1 ? "" : "s")
                       .arg(bytes ? sizeText(bytes) : "0 MB");
    if (pendingScans)
        text += QString(" — scanning %1 disc%2…").arg(pendingScans).arg(pendingScans == 1 ? "" : "s");
    queueLabel->setText(text);
}
