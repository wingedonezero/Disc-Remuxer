#include "MainWindow.h"

#include <cstring>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QComboBox>
#include <QSignalBlocker>
#include <QPushButton>
#include <QSplitter>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "Console.h"
#include "DetailsPanel.h"
#include "PrefsDialog.h"
#include "QueueTree.h"

extern "C" {
#include "base/log.h"
#include "base/settings.h"
}

enum Column { C_NAME, C_DESC, C_SIZE, C_STATUS, C_PROGRESS, C_COUNT };
// R_TRACK: a track's number; -1 the disc or a title; R_CHAPTERS on a title's
// chapter rows
enum Role { R_JOB = Qt::UserRole, R_TITLE, R_TRACK };
enum { R_CHAPTERS = -2 };

static const int defaultWidths[C_COUNT] = { 190, 520, 75, 170, 110 };

MainWindow::MainWindow(LogRelay *r) : relay(r)
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
    // what the titles are written as: the setting output.format (also in Preferences)
    formatBox = new QComboBox;
    formatBox->addItem("Streams", "streams");
    formatBox->addItem("MKV", "mkv");
    formatBox->addItem("MKV (mkvmerge)", "mkvmerge");
    formatBox->setToolTip("What each title is written as: a file per track (Streams), one Matroska file (MKV), or "
                          "the stream files muxed by mkvmerge into one Matroska file (MKV (mkvmerge))");
    buttons->addWidget(new QLabel("Output:"));
    buttons->addWidget(formatBox);
    syncFormatBox();
    connect(formatBox, &QComboBox::currentIndexChanged, this, [this](int i) {
        char err[256];
        if (settings_store("output.format", formatBox->itemData(i).toString().toUtf8().constData(), err,
                           sizeof(err)) == 0)
            settings_save();
    });

    queueLabel = new QLabel;
    queueLabel->setStyleSheet("font-weight:600;");

    tree = new QueueTree;
    tree->setColumnCount(C_COUNT);
    tree->setHeaderLabels({ "Type", "Description", "Size", "Status", "Progress" });
    // columns keep their width (the user's) whatever rows are opened; the
    // description takes what is left, so size, status and progress stay in view
    tree->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree->header()->setSectionResizeMode(C_DESC, QHeaderView::Stretch);
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
    centerSplit->setStretchFactor(0, 2);
    centerSplit->setStretchFactor(1, 1);
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
    connect(relay, &LogRelay::line, console, &Console::addLine);
    connect(relay, &LogRelay::titleStarted, this, &MainWindow::onTitleStarted);
    connect(relay, &LogRelay::titleDone, this, &MainWindow::onTitleDone);
    connect(relay, &LogRelay::runResult, this, &MainWindow::onRunResult);
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
    relay->goLive();
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
    // a test run (snapshot) leaves the user's window sizes as they are
    if (snapshot.isEmpty())
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
    job.item->setText(C_STATUS, QString::fromUtf8(engine_state_text(ENGINE_STATE_SCAN_WAITING)));
    newBar(job.item, -1);
    pendingScans++;
    emit scanRequested(job.id, job.found.path);
}

void MainWindow::addPaths(const QStringList &paths)
{
    for (const QString &p : paths) {
        const QList<FoundDisc> found = findDiscs(p);
        if (found.isEmpty()) {
            QByteArray path = p.toUtf8(), note(path.size() + 64, 0);
            engine_no_disc_note(path.constData(), note.data(), note.size());
            console->addLine(LOG_WARNING, 0, QString(), QString::fromUtf8(note.constData()), false);
        }
        for (const FoundDisc &f : found) {
            Job job;
            job.id = nextId++;
            job.found = f;
            job.item = new QTreeWidgetItem(QStringList{ f.name });
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
        j->item->setText(C_STATUS, QString::fromUtf8(engine_state_text(ENGINE_STATE_SCANNING)));
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
            j->item->setText(C_STATUS, QString::fromUtf8(engine_state_text(ENGINE_STATE_UNREADABLE)));
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
        if (test == "expand" && j && j->item->childCount()) {
            // the first title opened: its chapters and tracks
            j->item->child(0)->setExpanded(true);
            if (j->item->child(0)->childCount())
                j->item->child(0)->child(0)->setExpanded(true);
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
    bool on = engine_title_rip(job.disc, t);
    QBrush grey = tree->palette().brush(QPalette::Disabled, QPalette::Text);

    updating = true;
    title->setCheckState(C_NAME, on ? Qt::Checked : Qt::Unchecked);
    // its state (the engine's words) and the fake playlist check's mark
    char state[512], why[1024];
    int mark = engine_title_state(ti, engine_title_split(job.disc, t), state, sizeof(state), why, sizeof(why));
    title->setIcon(C_STATUS, QIcon());
    if (mark != DISC_PLAYLIST_NOT_MARKED)
        title->setIcon(C_STATUS, style()->standardIcon(mark == DISC_PLAYLIST_LIKELY_FAKE ? QStyle::SP_MessageBoxWarning
                                                                                       : QStyle::SP_DialogApplyButton));
    title->setToolTip(C_STATUS, QString::fromUtf8(why));
    title->setText(C_STATUS, QString::fromUtf8(state));
    for (int i = 0; i < job.tracks[t].size(); i++) {
        QTreeWidgetItem *track = job.tracks[t][i];
        bool changeable = on && i != 0 && !running;
        track->setCheckState(C_NAME, on && keep[i] ? Qt::Checked : Qt::Unchecked);
        track->setFlags(changeable ? track->flags() | Qt::ItemIsUserCheckable : track->flags() & ~Qt::ItemIsUserCheckable);
        track->setForeground(C_NAME, on ? tree->palette().brush(QPalette::Text) : grey);
    }
    updating = false;
}

void MainWindow::showDiscCheck(Job &job)
{
    const DiscInfo *d = engine_disc_info(job.disc);
    int n = titles(job), c = 0;
    int64_t bytes = 0;
    for (int t = 0; t < n; t++)
        if (engine_title_rip(job.disc, t)) {
            c++;
            bytes += d->titles[t].size;
        }
    updating = true;
    job.item->setCheckState(C_NAME, !c ? Qt::Unchecked : c == n ? Qt::Checked : Qt::PartiallyChecked);
    // what the checked titles hold
    job.item->setText(C_SIZE, bytes ? sizeText(bytes) : QString());
    updating = false;
}

// A row of a title's chapters: no check box, not dragged.
static void chapterRow(QTreeWidgetItem *it, quint64 job, int t)
{
    it->setData(C_NAME, R_JOB, job);
    it->setData(C_NAME, R_TITLE, t);
    it->setData(C_NAME, R_TRACK, R_CHAPTERS);
    it->setFlags(it->flags() & ~(Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
}

// The disc's titles under its row, each with its chapters and its tracks (a
// track taken out of another one under it: a core, forced subtitles, a 3-D
// second view): angles other than the main one unchecked; track 0
// (the video, with a Dolby Vision layer) always ripped.
void MainWindow::fillDisc(Job &job)
{
    const DiscInfo *d = engine_disc_info(job.disc);

    updating = true;
    job.item->setFlags(job.item->flags() | Qt::ItemIsUserCheckable);
    // what it is, and what the disc's checks found (the engine's words)
    char what[1024], state[512], why[2048];
    bool notes = engine_disc_line(d, what, sizeof(what), state, sizeof(state), why, sizeof(why));
    job.item->setText(C_DESC, QString::fromUtf8(what));
    job.item->setToolTip(C_DESC, QString::fromUtf8(what));
    job.item->setText(C_STATUS, QString::fromUtf8(state));
    job.item->setIcon(C_STATUS, notes ? style()->standardIcon(QStyle::SP_MessageBoxWarning) : QIcon());
    job.item->setToolTip(C_STATUS, QString::fromUtf8(why));
    job.tracks.clear();
    job.tracks.resize(d->nb_titles);
    for (int t = 0; t < d->nb_titles; t++) {
        const DiscTitleInfo *ti = &d->titles[t];
        // "Title #3", name – chapters, duration, what it is made of (source)
        char tname[64], tdesc[2048];
        engine_title_line(ti, tname, sizeof(tname), tdesc, sizeof(tdesc));
        QString desc = QString::fromUtf8(tdesc);
        auto *title = new QTreeWidgetItem(job.item, QStringList{ QString::fromUtf8(tname) });
        title->setText(C_DESC, desc);
        title->setToolTip(C_DESC, desc);
        title->setText(C_SIZE, sizeText(ti->size));
        title->setData(C_NAME, R_JOB, job.id);
        title->setData(C_NAME, R_TITLE, t);
        title->setData(C_NAME, R_TRACK, -1);
        title->setFlags((title->flags() | Qt::ItemIsUserCheckable) & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
        // its chapters
        if (ti->nb_chapters) {
            auto *chapters = new QTreeWidgetItem(title, QStringList{ "Chapters" });
            chapters->setText(C_DESC, QString("%1 chapter%2").arg(ti->nb_chapters).arg(ti->nb_chapters == 1 ? "" : "s"));
            chapterRow(chapters, job.id, t);
            for (int c = 0; c < ti->nb_chapters; c++) {
                char cdesc[512];
                auto *ch = new QTreeWidgetItem(chapters, QStringList{ QString("Chapter %1").arg(c + 1) });
                engine_chapter_line(ti, c, cdesc, sizeof(cdesc));
                ch->setText(C_DESC, QString::fromUtf8(cdesc));
                chapterRow(ch, job.id, t);
            }
        }
        // its tracks
        job.tracks[t].resize(ti->nb_streams);
        for (int i = 0; i < ti->nb_streams; i++) {
            const DiscStreamInfo *k = &ti->streams[i];
            char kname[64], kdesc[2048];
            engine_track_line(ti, i, kname, sizeof(kname), kdesc, sizeof(kdesc));
            QString desc = QString::fromUtf8(kdesc);
            // a track taken out of another one goes under it
            QTreeWidgetItem *parent = k->from_track >= 0 && k->from_track < i ? job.tracks[t][k->from_track] : title;
            auto *track = new QTreeWidgetItem(parent, QStringList{ QString::fromUtf8(kname) });
            track->setText(C_DESC, desc);
            track->setToolTip(C_DESC, desc);
            track->setData(C_NAME, R_JOB, job.id);
            track->setData(C_NAME, R_TITLE, t);
            track->setData(C_NAME, R_TRACK, i);
            track->setToolTip(C_NAME, i == 0 ? "The video is always ripped" : QString());
            track->setFlags(track->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
            if (parent != title)
                parent->setExpanded(true);
            job.tracks[t][i] = track;
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
    if (i == R_CHAPTERS)
        return;
    bool on = item->checkState(C_NAME) != Qt::Unchecked;

    if (running) {
        // the choice of a disc in the run stays as it was
        for (int k = 0; k < titles(*j); k++)
            showTitleChecks(*j, k);
        showDiscCheck(*j);
        return;
    }
    if (i >= 0) {
        // a track: what the engine rips of its title (the video it keeps)
        if (engine_set_track_kept(j->disc, t, i, on) < 0)
            showTitleChecks(*j, t);
    } else if (t >= 0) {
        engine_set_title_rip(j->disc, t, on);
        showTitleChecks(*j, t);
    } else {
        // the disc: all its titles on or off
        for (int k = 0; k < titles(*j); k++) {
            engine_set_title_rip(j->disc, k, on);
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
    else if (i == R_CHAPTERS)
        details->showChapters(&d->titles[t]);
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
        const DiscTitleInfo &ti = engine_disc_info(j->disc)->titles[t];
        QString unit = ti.part_unit ? QString::fromUtf8(ti.part_unit) : QString("stream file");
        unit[0] = unit[0].toUpper();
        for (int k = 1; k < unit.size(); k++)
            if (unit[k - 1] == ' ')
                unit[k] = unit[k].toUpper();
        QAction *a = menu.addAction(QString("Write as %1 Parts (One per %2)").arg(ti.nb_parts).arg(unit));
        a->setCheckable(true);
        a->setChecked(engine_title_split(j->disc, t));
        a->setEnabled(!running);
        connect(a, &QAction::toggled, this, [this, id = j->id, t](bool on) {
            if (Job *x = jobById(id); x && x->disc) {
                engine_set_title_split(x->disc, t, on);
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
    j->tracks.clear();
    details->clear();
    queueScan(*j);
    updateControls();
    updateQueueLabel();
}

void MainWindow::openPrefs()
{
    PrefsDialog dlg(this);
    QStringList changed;
    bool reading;

    while (dlg.exec() == QDialog::Accepted) {
        if (dlg.apply(&changed, &reading)) {
            for (const QString &c : changed) {
                QByteArray what = c.toUtf8(), note(what.size() + 64, 0);
                engine_setting_kept_note(what.constData(), note.data(), note.size());
                console->addLine(LOG_INFO, 0, QString(), QString::fromUtf8(note.constData()), false);
            }
            if (reading && !running && !jobs.isEmpty() &&
                QMessageBox::question(this, "Preferences", "These settings change how a disc is read. Scan the "
                                                           "discs in the queue again so they use them?") ==
                    QMessageBox::Yes) {
                for (int r = 0; r < tree->topLevelItemCount(); r++) {
                    tree->setCurrentItem(tree->topLevelItem(r));
                    rescanSelected();
                }
            }
            break;
        }
        changed.clear();
    }
    syncFormatBox();
}

// the output selector shows the setting as it is now
void MainWindow::syncFormatBox()
{
    const QSignalBlocker block(formatBox);
    int i = formatBox->findData(QString::fromUtf8(setting_text("output.format")));
    formatBox->setCurrentIndex(i < 0 ? 0 : i);
}

// ---- the run ----

// The engine's folder rule (discs.h, the one demux of a folder uses):
// <main output folder>/<the disc's name>; with output.keep_folder_structure
// a disc found below an added folder: <main>/<added folder>/<folders>/<name>.
QString MainWindow::outputFolder(const Job &job) const
{
    QByteArray path = job.found.path.toUtf8(), name = job.found.name.toUtf8(), rel = job.found.relative.toUtf8(),
               root = job.found.dropRoot.toUtf8();
    EngineFound f = { path.data(), name.data(), rel.data(), root.data() };
    char folder[4096];

    engine_found_folder(&f, nullptr, folder, sizeof(folder));
    return QString::fromUtf8(folder);
}

void MainWindow::startQueue()
{
    QList<RipJob> run;

    if (running || pendingScans)
        return;
    for (int r = 0; r < tree->topLevelItemCount(); r++) {
        Job *j = jobOf(tree->topLevelItem(r));
        if (!j || !j->disc || !engine_titles_ripped(j->disc))
            continue;
        run.append({ j->id, j->disc, outputFolder(*j) });
        j->item->setText(C_STATUS, QString::fromUtf8(engine_state_text(ENGINE_STATE_RIP_WAITING)));
        newBar(j->item, 0);
    }
    if (run.isEmpty()) {
        console->addLine(LOG_WARNING, 0, QString(), QString::fromUtf8(engine_state_text(ENGINE_NOTE_NOTHING_CHECKED)),
                         false);
        return;
    }
    running = true;
    stopping = false;
    tree->reorderable = false;
    for (Job &j : jobs)
        for (int t = 0; t < titles(j); t++)
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
    console->addLine(LOG_WARNING, 0, QString(), QString::fromUtf8(engine_state_text(ENGINE_NOTE_STOPPING)), false);
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
    j->item->setText(C_STATUS, QString::fromUtf8(engine_state_text(ENGINE_STATE_RIPPING)));
}

int MainWindow::titles(const Job &j)
{
    return j.disc ? engine_disc_info(j.disc)->nb_titles : 0;
}

// the disc being ripped, NULL when none
MainWindow::Job *MainWindow::ripping()
{
    Job *j = jobById(ripId);
    return j && running && j->disc ? j : nullptr;
}

void MainWindow::onTitleStarted(TitleStart s)
{
    Job *j = ripping();
    QTreeWidgetItem *title = j && s.title >= 0 && s.title < j->item->childCount() ? j->item->child(s.title) : nullptr;
    char tstate[128], dstate[128];

    if (!title)
        return;
    ripTitle = s.title;
    ripOrdinal = s.ordinal;
    ripCount = s.count;
    titleClock.start();
    engine_title_start_state(s.title, s.part, s.ordinal, s.count, tstate, sizeof(tstate), dstate, sizeof(dstate));
    title->setText(C_STATUS, QString::fromUtf8(tstate));
    j->item->setText(C_STATUS, QString::fromUtf8(dstate));
}

void MainWindow::onTitleDone(TitleResult r)
{
    Job *j = ripping();
    QTreeWidgetItem *title = j && r.title >= 0 && r.title < j->item->childCount() ? j->item->child(r.title) : nullptr;
    char state[128];

    if (!title)
        return;
    engine_title_result_state(r.part, r.status, r.warnings, state, sizeof(state));
    title->setText(C_STATUS, QString::fromUtf8(state));
}

void MainWindow::onRunResult(RunResult r)
{
    if (Job *j = ripping())
        j->logFile = r.log;
}

void MainWindow::onProgress(int t, double done)
{
    Job *j = jobById(ripId);

    if (!j || !running || !j->disc || t != ripTitle || t >= j->item->childCount())
        return;
    const DiscTitleInfo *ti = &engine_disc_info(j->disc)->titles[t];
    char text[256];
    engine_progress_line(done, ti->size, titleClock.elapsed() / 1000.0, !engine_title_split(j->disc, t), text,
                         sizeof(text));
    j->item->child(t)->setText(C_STATUS, QString::fromUtf8(text));
    if (QProgressBar *bar = barOf(j->item))
        bar->setValue(int(100 * ((ripOrdinal - 1) + done) / qMax(1, ripCount)));
}

void MainWindow::ripDone(quint64 id, int result, bool stopped)
{
    Job *j = jobById(id);

    if (!j)
        return;
    j->item->setText(C_STATUS, QString::fromUtf8(engine_disc_result_state(result != 0, stopped)));
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
        for (int t = 0; t < titles(j); t++) {
            QString keep = j.item->child(t)->text(C_STATUS);
            showTitleChecks(j, t);
            j.item->child(t)->setText(C_STATUS, keep);     // the run's result stays shown
        }
    console->addLine(stopped ? LOG_WARNING : LOG_OK, 0, QString(),
                     QString::fromUtf8(engine_state_text(stopped ? ENGINE_NOTE_QUEUE_STOPPED : ENGINE_NOTE_QUEUE_DONE)),
                     true);
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
        for (int t = 0; t < d->nb_titles; t++)
            if (engine_title_rip(j.disc, t)) {
                bytes += d->titles[t].size;
                titles++;
            }
    }
    char text[256];
    engine_queue_line(jobs.size(), titles, bytes, pendingScans, text, sizeof(text));
    queueLabel->setText(QString::fromUtf8(text));
}
