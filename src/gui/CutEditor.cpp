#include "CutEditor.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMutexLocker>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "EngineWorker.h"

extern "C" {
#include "base/settings.h"
}

static const int64_t TPS = DR_TICKS_PER_SECOND;

// A time to the ms, as the editor shows it: 0:22:52.204.
static QString msText(int64_t ticks)
{
    int64_t ms = (std::max<int64_t>(ticks, 0) * 1000 + TPS / 2) / TPS;
    return QString::asprintf("%d:%02d:%02d.%03d", int(ms / 3600000), int(ms / 60000 % 60), int(ms / 1000 % 60),
                             int(ms % 1000));
}

// A time as the title's cut list keeps it (cuts.h cut_parse(): to the ns),
// so the rip finds the same key frame: 0:22:52.204166667.
static QString itemText(int64_t ticks)
{
    int64_t s = ticks / TPS, ns = (ticks % TPS * 1000000000 + TPS / 2) / TPS;
    if (ns >= 1000000000) {
        s++;
        ns -= 1000000000;
    }
    return QString::asprintf("%d:%02d:%02d.%09lld", int(s / 3600), int(s / 60 % 60), int(s % 60), (long long)ns);
}

// The exact time (ticks in words), for tooltips.
static QString exactText(int64_t ticks)
{
    char b[48];
    return QString::fromUtf8(cut_time_text(b, sizeof(b), ticks));
}

static QString startText(int start)
{
    return start == DR_DISC_START_CLEAN  ? "clean" :
           start == DR_DISC_START_OPEN   ? "open" :
           start == DR_DISC_START_BROKEN ? "broken link" : "not told";
}

// ---- the editor's thread ----

void PreviewWorker::want(int n, QSize size)
{
    QMutexLocker l(&lock);
    wantN    = n;
    wantSize = size;
    if (!scheduled && !stopping) {
        scheduled = true;
        QMetaObject::invokeMethod(this, "work", Qt::QueuedConnection);
    }
}

void PreviewWorker::stopWork()
{
    QMutexLocker l(&lock);
    stopping = true;
    wantN    = -1;
}

void PreviewWorker::open()
{
    emit opened(engine_cut_editor_open(disc, title, &preview) == 0);
}

void PreviewWorker::work()
{
    for (;;) {
        CutFrame f;
        QSize s;
        int n;
        {
            QMutexLocker l(&lock);
            if (stopping || wantN < 0) {
                scheduled = false;
                return;
            }
            n     = wantN;
            s     = wantSize;
            wantN = -1;
        }
        if (!preview || cut_preview_picture(preview, n, s.width(), s.height(), &f) < 0) {
            emit failed(n);
            continue;
        }
        QImage img(f.bgra, f.width, f.height, f.stride, QImage::Format_RGB32);
        emit picture(n, img.copy(), QChar(f.type ? f.type : '?'));
    }
}

void PreviewWorker::finish()
{
    engine_cut_editor_close(disc, title, &preview);
}

// ---- the timeline ----

CutTimeline::CutTimeline(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumHeight(34);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void CutTimeline::setTitle(int64_t e, const QVector<int64_t> &c)
{
    end      = e;
    chapters = c;
    update();
}

void CutTimeline::setCuts(const QVector<int64_t> &zeros)
{
    cuts = zeros;
    update();
}

void CutTimeline::setPosition(int64_t t)
{
    pos = t;
    update();
}

int CutTimeline::xOf(int64_t t) const
{
    return end > 0 ? int(double(t) * (width() - 1) / double(end)) : 0;
}

int64_t CutTimeline::timeAt(int x) const
{
    x = std::clamp(x, 0, std::max(1, width() - 1));
    return int64_t(double(x) * double(end) / double(std::max(1, width() - 1)));
}

void CutTimeline::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const int top = 2, bar = height() - 15;

    p.fillRect(rect(), palette().window());
    if (end <= 0)
        return;
    // the parts, shaded in turn
    QVector<int64_t> b = { 0 };
    b += cuts;
    b += end;
    for (int i = 0; i + 1 < b.size(); i++)
        p.fillRect(QRect(xOf(b[i]), top, xOf(b[i + 1]) - xOf(b[i]) + 1, bar),
                   i % 2 ? palette().alternateBase() : palette().base());
    p.setPen(palette().mid().color());
    p.drawRect(0, top, width() - 1, bar);
    // the chapters: ticks under the bar, numbered where there is room
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    p.setPen(palette().text().color());
    int last = -100;
    for (int i = 0; i < chapters.size(); i++) {
        int x = xOf(chapters[i]);
        p.drawLine(x, top + bar, x, top + bar + 4);
        if (x - last >= 18) {
            p.drawText(x + 2, height() - 1, QString::number(i + 1));
            last = x;
        }
    }
    // the cuts
    p.setPen(QPen(QColor(220, 30, 30), 2));
    for (int64_t c : cuts)
        p.drawLine(xOf(c), top, xOf(c), top + bar);
    // the picture shown
    int x = xOf(pos);
    p.setPen(QPen(palette().highlight().color(), 2));
    p.drawLine(x, 0, x, top + bar + 2);
    p.setBrush(palette().highlight());
    p.drawPolygon(QPolygon({ QPoint(x - 4, 0), QPoint(x + 4, 0), QPoint(x, 5) }));
}

void CutTimeline::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton || end <= 0)
        return;
    dragging = true;
    emit dragged(timeAt(e->position().toPoint().x()));
}

void CutTimeline::mouseMoveEvent(QMouseEvent *e)
{
    int64_t t = timeAt(e->position().toPoint().x());

    if (end <= 0)
        return;
    if (dragging)
        emit dragged(t);
    QToolTip::showText(e->globalPosition().toPoint(), msText(t), this);
}

void CutTimeline::mouseReleaseEvent(QMouseEvent *e)
{
    if (!dragging || e->button() != Qt::LeftButton)
        return;
    dragging = false;
    emit released(timeAt(e->position().toPoint().x()));
}

void CutTimeline::wheelEvent(QWheelEvent *e)
{
    if (int d = e->angleDelta().y())
        emit stepped(d > 0 ? 1 : -1);
}

// ---- the picture ----

PictureView::PictureView(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(320, 180);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PictureView::setImage(const QImage &img)
{
    image = img;
    update();
}

void PictureView::paintEvent(QPaintEvent *)
{
    QPainter p(this);

    p.fillRect(rect(), Qt::black);
    if (image.isNull())
        return;
    QSize s = image.size().scaled(size(), Qt::KeepAspectRatio);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(QRect(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s), image);
}

void PictureView::wheelEvent(QWheelEvent *e)
{
    if (int d = e->angleDelta().y())
        emit stepped(d > 0 ? 1 : -1);
}

// ---- the editor ----

CutEditor::CutEditor(EngineDisc *d, int t, LogRelay *relay, QWidget *parent)
    : QDialog(parent), disc(d), title(t), ti(&engine_disc_info(d)->titles[t])
{
    setWindowTitle(QString("Cuts - Title #%1%2").arg(t).arg(ti->name ? QString(" (%1)").arg(QString::fromUtf8(ti->name))
                                                                     : QString()));
    rule = strcmp(setting_text("cuts.key_frame"), "next") ? CUT_NEAREST : CUT_NEXT;
    // the pictures decoded at their display aspect, at most 1920 wide (the
    // window scales them; a size of their own keeps them as the cache holds them)
    if (ti->nb_streams > 0) {
        const DiscStreamInfo &v = ti->streams[0];
        int w = v.width, h = v.height;
        if (v.aspect.num > 0 && v.aspect.den > 0 && h > 0)
            w = int(double(h) * v.aspect.num / v.aspect.den + 0.5);
        if (w > 1920) {
            h = int(double(h) * 1920 / w + 0.5);
            w = 1920;
        }
        if (w > 0 && h > 0)
            picSize = QSize(w, h);
    }
    buildUi();
    loadState();

    worker = new PreviewWorker(d, t);
    worker->moveToThread(&thread);
    connect(worker, &PreviewWorker::opened, this, &CutEditor::opened);
    connect(worker, &PreviewWorker::picture, this, &CutEditor::arrived);
    connect(worker, &PreviewWorker::failed, this, &CutEditor::failed);
    connect(relay, &LogRelay::progress, this, &CutEditor::progress);
    connect(&playTimer, &QTimer::timeout, this, &CutEditor::playTick);
    thread.start();
    QMetaObject::invokeMethod(worker, "open", Qt::QueuedConnection);
}

CutEditor::~CutEditor()
{
    shutdown();
}

void CutEditor::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    split = new QSplitter(Qt::Horizontal);
    outer->addWidget(split, 1);

    // left: the picture, the timeline, the buttons (Avidemux's order), the time
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    stack = new QStackedWidget;
    auto *loadPage = new QWidget;
    auto *ll = new QVBoxLayout(loadPage);
    loadLabel = new QLabel("Reading the title's pictures…");
    loadBar = new QProgressBar;
    loadBar->setRange(0, 1000);
    ll->addStretch();
    ll->addWidget(loadLabel, 0, Qt::AlignHCenter);
    ll->addWidget(loadBar);
    ll->addStretch();
    view = new PictureView;
    stack->addWidget(loadPage);
    stack->addWidget(view);
    lv->addWidget(stack, 1);

    controls = new QWidget;
    auto *cv = new QVBoxLayout(controls);
    cv->setContentsMargins(0, 0, 0, 0);
    timeline = new CutTimeline;
    cv->addWidget(timeline);

    auto *row = new QHBoxLayout;
    auto button = [&](const QString &text, int icon, const QString &tip, std::function<void()> fn) {
        auto *b = new QToolButton;
        if (icon >= 0)
            b->setIcon(style()->standardIcon(QStyle::StandardPixmap(icon)));
        else
            b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QToolButton::clicked, this, [this, fn] {
            if (ix)
                fn();
        });
        row->addWidget(b);
        return b;
    };
    auto sep = [&] { row->addSpacing(10); };
    playBtn = button("", QStyle::SP_MediaPlay, "Play / pause (Space) - pictures only", [this] { togglePlay(); });
    sep();
    button("", QStyle::SP_MediaSeekBackward, "Previous frame (Left)", [this] { stopPlay(); show(cur - 1); });
    button("", QStyle::SP_MediaSeekForward, "Next frame (Right)", [this] { stopPlay(); show(cur + 1); });
    button("", QStyle::SP_MediaSkipBackward, "Previous key frame (Down)", [this] { goKey(-1); });
    button("", QStyle::SP_MediaSkipForward, "Next key frame (Up)", [this] { goKey(1); });
    sep();
    button("+ Cut", -1, "Add a cut here (Ctrl+K): at this picture's key frame, by the rule of setting cuts.key_frame",
           [this] { addCutHere(); });
    button("- Cut", -1, "Remove the cut selected in the list, or the one at this picture (Del)", [this] { removeCut(); });
    sep();
    button("◀ Cut", -1, "Previous cut (Shift+Down)", [this] { goCut(-1); });
    button("Cut ▶", -1, "Next cut (Shift+Up)", [this] { goCut(1); });
    button("◀ Ch", -1, "Previous chapter (Ctrl+PgUp)", [this] { goChapter(-1); });
    button("Ch ▶", -1, "Next chapter (Ctrl+PgDn)", [this] { goChapter(1); });
    sep();
    button("⏮", -1, "First frame (Home)", [this] { stopPlay(); show(0); });
    button("⏭", -1, "Last frame (End)", [this] { stopPlay(); show(times.size() - 1); });
    button("-1 min", -1, "One minute back (Ctrl+Down)", [this] { stopPlay(); showAt(times[cur] - 60 * TPS); });
    button("+1 min", -1, "One minute on (Ctrl+Up)", [this] { stopPlay(); showAt(times[cur] + 60 * TPS); });
    row->addStretch();
    cv->addLayout(row);

    auto *trow = new QHBoxLayout;
    timeBtn = new QPushButton("Time:");
    timeBtn->setToolTip("Go to a time or a chapter");
    timeBtn->setFocusPolicy(Qt::NoFocus);
    timeBtn->setAutoDefault(false);
    connect(timeBtn, &QPushButton::clicked, this, [this] {
        if (ix)
            goTime();
    });
    timeLabel = new QLabel("0:00:00.000");
    totalLabel = new QLabel("/ 0:00:00.000");
    typeLabel = new QLabel;
    QFont mono = timeLabel->font();
    mono.setFamily("monospace");
    mono.setStyleHint(QFont::Monospace);
    timeLabel->setFont(mono);
    totalLabel->setFont(mono);
    trow->addWidget(timeBtn);
    trow->addWidget(timeLabel);
    trow->addWidget(totalLabel);
    trow->addSpacing(20);
    trow->addWidget(new QLabel("Frame type:"));
    trow->addWidget(typeLabel, 1);
    cv->addLayout(trow);
    lv->addWidget(controls);
    split->addWidget(left);

    // right: the cuts, the parts they make, the chapters
    side = new QWidget;
    auto *sl = new QVBoxLayout(side);
    sl->setContentsMargins(0, 0, 0, 0);
    sideSplit = new QSplitter(Qt::Vertical);
    sl->addWidget(sideSplit);
    auto list = [](const QStringList &heads) {
        auto *w = new QTreeWidget;
        w->setHeaderLabels(heads);
        w->setRootIsDecorated(false);
        w->setFocusPolicy(Qt::NoFocus);
        w->header()->setStretchLastSection(true);
        return w;
    };

    auto *cutBox = new QGroupBox("Cuts");
    auto *cb = new QVBoxLayout(cutBox);
    cutList = list({ "#", "Part starts", "Key frame", "From" });
    cb->addWidget(cutList);
    auto *cbr = new QHBoxLayout;
    for (auto [text, fn] : { std::pair<const char *, void (CutEditor::*)()>{ "Add Cut Here", &CutEditor::addCutHere },
                             { "Remove", &CutEditor::removeCut }, { "Clear", &CutEditor::clearCuts } }) {
        auto *b = new QPushButton(text);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoDefault(false);
        connect(b, &QPushButton::clicked, this, [this, fn = fn] {
            if (ix)
                (this->*fn)();
        });
        cbr->addWidget(b);
    }
    cb->addLayout(cbr);
    note = new QLabel;
    note->setWordWrap(true);
    cb->addWidget(note);
    connect(cutList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        int i = it->data(0, Qt::UserRole).toInt();
        stopPlay();
        if (i >= 0 && i < cuts.size())
            showAt(pts[cuts[i].point].zero);
    });
    sideSplit->addWidget(cutBox);

    auto *partBox = new QGroupBox("Parts");
    auto *pb = new QVBoxLayout(partBox);
    partList = list({ "Part", "Start", "Length" });
    pb->addWidget(partList);
    connect(partList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        stopPlay();
        showAt(it->data(0, Qt::UserRole).toLongLong());
    });
    sideSplit->addWidget(partBox);

    auto *chBox = new QGroupBox("Chapters");
    auto *chb = new QVBoxLayout(chBox);
    chapterList = list({ "#", "Start", "Length" });
    chb->addWidget(chapterList);
    auto *chCut = new QPushButton("Cut at Chapter");
    chCut->setToolTip("A cut at the selected chapter (kept as c<N>: the rip places it the same way)");
    chCut->setFocusPolicy(Qt::NoFocus);
    chCut->setAutoDefault(false);
    connect(chCut, &QPushButton::clicked, this, [this] {
        if (ix)
            addChapterCut();
    });
    chb->addWidget(chCut);
    connect(chapterList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        stopPlay();
        showAt(it->data(0, Qt::UserRole).toLongLong());
    });
    sideSplit->addWidget(chBox);
    split->addWidget(side);
    split->setStretchFactor(0, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    for (QAbstractButton *b : buttons->buttons()) {
        b->setFocusPolicy(Qt::NoFocus);
        if (auto *p = qobject_cast<QPushButton *>(b)) {
            p->setAutoDefault(false);
            p->setDefault(false);
        }
    }
    buttons->button(QDialogButtonBox::Ok)->setToolTip("The title is ripped in these parts");
    connect(buttons, &QDialogButtonBox::accepted, this, &CutEditor::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CutEditor::reject);
    outer->addWidget(buttons);
    okButton = buttons->button(QDialogButtonBox::Ok);

    // Avidemux's keys
    auto key = [this](const QKeySequence &k, std::function<void()> fn) {
        auto *s = new QShortcut(k, this);
        connect(s, &QShortcut::activated, this, [this, fn] {
            if (ix)
                fn();
        });
    };
    key(Qt::Key_Space, [this] { togglePlay(); });
    key(Qt::Key_Left, [this] { stopPlay(); show(cur - 1); });
    key(Qt::Key_Right, [this] { stopPlay(); show(cur + 1); });
    key(Qt::Key_Down, [this] { goKey(-1); });
    key(Qt::Key_Up, [this] { goKey(1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_K), [this] { addCutHere(); });
    key(Qt::Key_Delete, [this] { removeCut(); });
    key(QKeySequence(Qt::SHIFT | Qt::Key_Down), [this] { goCut(-1); });
    key(QKeySequence(Qt::SHIFT | Qt::Key_Up), [this] { goCut(1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_PageUp), [this] { goChapter(-1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_PageDown), [this] { goChapter(1); });
    key(Qt::Key_Home, [this] { stopPlay(); show(0); });
    key(Qt::Key_End, [this] { stopPlay(); show(times.size() - 1); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Down), [this] { stopPlay(); showAt(times[cur] - 60 * TPS); });
    key(QKeySequence(Qt::CTRL | Qt::Key_Up), [this] { stopPlay(); showAt(times[cur] + 60 * TPS); });

    connect(timeline, &CutTimeline::dragged, this, [this](int64_t t) {
        if (!ix)
            return;
        stopPlay();
        show(keyAtOrBefore(at(t)));        // while dragging: key frames only
    });
    connect(timeline, &CutTimeline::released, this, [this](int64_t t) {
        if (ix)
            show(at(t));
    });
    connect(timeline, &CutTimeline::stepped, this, [this](int d) {
        if (ix) {
            stopPlay();
            show(cur + d);
        }
    });
    connect(view, &PictureView::stepped, this, [this](int d) {
        if (ix) {
            stopPlay();
            show(cur + d);
        }
    });

    controls->setEnabled(false);
    side->setEnabled(false);
    okButton->setEnabled(false);
    resize(1280, 760);
    split->setSizes({ 900, 380 });
}

void CutEditor::loadState()
{
    const QStringList s = QString::fromUtf8(setting_text("gui.cut_editor")).split(',', Qt::SkipEmptyParts);

    if (s.size() != 6)
        return;
    restoreGeometry(QByteArray::fromBase64(s[0].toLatin1()));
    split->setSizes({ s[1].toInt(), s[2].toInt() });
    sideSplit->setSizes({ s[3].toInt(), s[4].toInt(), s[5].toInt() });
}

void CutEditor::saveState()
{
    QStringList s = { QString::fromLatin1(saveGeometry().toBase64()) };
    char err[256];

    if (saved || !testFile.isEmpty())
        return;
    saved = true;
    for (int v : split->sizes())
        s << QString::number(v);
    for (int v : sideSplit->sizes())
        s << QString::number(v);
    settings_store("gui.cut_editor", s.join(',').toUtf8().constData(), err, sizeof(err));
    settings_save();
}

// The pictures and the index freed on the editor's thread, the thread ended.
void CutEditor::shutdown()
{
    if (!worker)
        return;
    playTimer.stop();
    worker->stopWork();
    QMetaObject::invokeMethod(worker, "finish", Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
    delete worker;
    worker = nullptr;
    ix = nullptr;
    free(pts);
    pts   = nullptr;
    nbPts = 0;
}

void CutEditor::progress(int t, double done)
{
    if (loading && t == title)
        loadBar->setValue(int(done * 1000));
}

void CutEditor::opened(bool ok)
{
    loading = false;
    if (closeWhenOpened || !ok) {
        if (!ok && !closeWhenOpened)
            QMessageBox::warning(this, "Cuts", "The title's pictures cannot be read: the log says why.");
        saveState();
        shutdown();
        QDialog::reject();
        return;
    }
    ix = engine_cut_index(disc, title);
    if (!ix || ix->nb_pics <= 0 || cut_points(ix->pics, ix->nb_pics, ix->codec, &pts, &nbPts) < 0) {
        QMessageBox::warning(this, "Cuts", "The title has no pictures to cut at.");
        ix = nullptr;
        saveState();
        shutdown();
        QDialog::reject();
        return;
    }
    // the pictures in display order
    order.resize(ix->nb_pics);
    for (int i = 0; i < ix->nb_pics; i++)
        order[i] = i;
    std::sort(order.begin(), order.end(), [this](int a, int b) { return ix->pics[a].time < ix->pics[b].time; });
    times.resize(ix->nb_pics);
    for (int n = 0; n < ix->nb_pics; n++) {
        times[n] = ix->pics[order[n]].time;
        if (ix->pics[order[n]].key)
            keys << n;
    }
    QVector<int64_t> chapters;
    for (int c = 0; c < ix->nb_chapters; c++) {
        int64_t s = ix->chapters[c], e = c + 1 < ix->nb_chapters ? ix->chapters[c + 1] : ix->end;
        auto *it = new QTreeWidgetItem(chapterList, { QString::number(c + 1), msText(s), msText(e - s) });
        it->setData(0, Qt::UserRole, (qlonglong)s);
        it->setToolTip(1, exactText(s));
        chapters << s;
    }
    timeline->setTitle(ix->end, chapters);
    totalLabel->setText("/ " + msText(ix->end));
    loadCuts();
    refresh();
    stack->setCurrentWidget(view);
    controls->setEnabled(true);
    side->setEnabled(true);
    okButton->setEnabled(true);
    view->setFocus();
    show(0);
    if (!testFile.isEmpty())
        QTimer::singleShot(0, this, &CutEditor::runTest);
}

void CutEditor::reject()
{
    if (loading) {
        // the reading stops; the window closes once it has
        engine_cancel();
        closeWhenOpened = true;
        loadLabel->setText("Stopping…");
        return;
    }
    saveState();
    shutdown();
    QDialog::reject();
}

void CutEditor::accept()
{
    QByteArray list = items().toUtf8();

    if (loading)
        return;
    if (engine_set_title_cuts(disc, title, list.isEmpty() ? nullptr : list.constData()) < 0) {
        QMessageBox::warning(this, "Cuts", "The cut list cannot be kept: " + items());
        return;
    }
    saveState();
    shutdown();
    QDialog::accept();
}

QString CutEditor::items() const
{
    QStringList l;
    for (const Cut &c : cuts)
        l << c.item;
    return l.join(',');
}

// ---- the cuts ----

// The title's cut list as kept, placed as the rip places it.
void CutEditor::loadCuts()
{
    const char *have = engine_title_cuts(disc, title), *bad;
    CutRequest *reqs = nullptr;
    QStringList refused;
    int nb = 0;

    if (!have || cut_parse(have, &reqs, &nb, &bad) < 0)
        return;
    QVector<CutResult> res(nb);
    cut_resolve(pts, nbPts, ix->chapters, ix->nb_chapters, ix->end, reqs, nb, rule, res.data());
    for (int i = 0; i < nb; i++) {
        if (res[i].point < 0) {
            refused << QString("%1 (%2)").arg(QString::fromUtf8(reqs[i].text),
                                              res[i].same_as >= 0 ? QString("the same key frame as %1")
                                                                        .arg(QString::fromUtf8(reqs[res[i].same_as].text))
                                                                  : QString::fromUtf8(res[i].why));
            continue;
        }
        cuts << Cut{ QString::fromUtf8(reqs[i].text), res[i].point,
                     reqs[i].chapter ? QString("chapter %1").arg(reqs[i].chapter) : QString("manual") };
    }
    free(reqs);
    sortCuts();
    if (!refused.isEmpty())
        setNote("Not placed, left out of the list: " + refused.join("; "));
}

void CutEditor::sortCuts()
{
    std::sort(cuts.begin(), cuts.end(), [this](const Cut &a, const Cut &b) { return pts[a.point].zero < pts[b.point].zero; });
}

void CutEditor::refresh()
{
    QVector<int64_t> zeros;

    cutList->clear();
    for (int i = 0; i < cuts.size(); i++) {
        const CutPoint &p = pts[cuts[i].point];
        auto *it = new QTreeWidgetItem(cutList, { QString::number(i + 1), msText(p.zero), startText(p.start),
                                                  cuts[i].from });
        it->setData(0, Qt::UserRole, i);
        it->setToolTip(1, QString("The part starts at %1; its key frame at %2").arg(exactText(p.zero),
                                                                                     exactText(p.key_time)));
        zeros << p.zero;
    }
    partList->clear();
    QVector<int64_t> b = { 0 };
    b += zeros;
    b += ix->end;
    for (int i = 0; i + 1 < b.size(); i++) {
        auto *it = new QTreeWidgetItem(partList, { QString::number(i + 1), msText(b[i]), msText(b[i + 1] - b[i]) });
        it->setData(0, Qt::UserRole, (qlonglong)b[i]);
    }
    timeline->setCuts(zeros);
    for (QTreeWidget *w : { cutList, partList })
        for (int c = 0; c < w->columnCount() - 1; c++)
            w->resizeColumnToContents(c);
}

void CutEditor::setNote(const QString &s)
{
    note->setText(s);
}

void CutEditor::addCutHere()
{
    CutRequest req = {};
    CutResult r;

    req.time = times[cur];
    snprintf(req.text, sizeof(req.text), "%s", msText(times[cur]).toUtf8().constData());
    cut_resolve(pts, nbPts, ix->chapters, ix->nb_chapters, ix->end, &req, 1, rule, &r);
    if (r.point < 0) {
        setNote(QString("No cut at %1: %2.").arg(msText(times[cur]), QString::fromUtf8(r.why)));
        return;
    }
    const CutPoint &p = pts[r.point];
    for (const Cut &c : cuts)
        if (c.point == r.point) {
            setNote(QString("A cut is already at the key frame there (the part starts at %1).").arg(msText(p.zero)));
            return;
        }
    cuts << Cut{ itemText(p.key_time), r.point, "manual" };
    sortCuts();
    refresh();
    int moved = at(p.zero) - cur;
    if (r.on_point || !moved)
        setNote(QString("Cut added: the part starts at %1.").arg(msText(p.zero)));
    else
        setNote(QString("Not a key frame: moved to the %1 key frame, %2 picture(s) %3; the part starts at %4.")
                    .arg(rule == CUT_NEXT ? "next" : "nearest")
                    .arg(std::abs(moved))
                    .arg(moved > 0 ? "later" : "earlier")
                    .arg(msText(p.zero)));
}

void CutEditor::addChapterCut()
{
    QTreeWidgetItem *it = chapterList->currentItem();
    CutRequest req = {};
    CutResult r;

    if (!it) {
        setNote("Select a chapter first.");
        return;
    }
    req.chapter = chapterList->indexOfTopLevelItem(it) + 1;
    snprintf(req.text, sizeof(req.text), "c%d", req.chapter);
    cut_resolve(pts, nbPts, ix->chapters, ix->nb_chapters, ix->end, &req, 1, rule, &r);
    if (r.point < 0) {
        setNote(QString("No cut at chapter %1: %2.").arg(req.chapter).arg(QString::fromUtf8(r.why)));
        return;
    }
    const CutPoint &p = pts[r.point];
    for (const Cut &c : cuts)
        if (c.point == r.point) {
            setNote(QString("A cut is already at chapter %1's key frame (the part starts at %2).")
                        .arg(req.chapter)
                        .arg(msText(p.zero)));
            return;
        }
    cuts << Cut{ QString(req.text), r.point, QString("chapter %1").arg(req.chapter) };
    sortCuts();
    refresh();
    setNote(r.on_point ? QString("Cut added at chapter %1: the part starts at %2.").arg(req.chapter).arg(msText(p.zero))
                       : QString("Chapter %1's mark is not on a key frame: the %2 one is taken; the part starts at %3.")
                             .arg(req.chapter)
                             .arg(rule == CUT_NEXT ? "next" : "nearest")
                             .arg(msText(p.zero)));
    stopPlay();
    show(at(p.zero));
}

void CutEditor::removeCut()
{
    int i = -1;

    if (QTreeWidgetItem *it = cutList->currentItem(); it && it->isSelected())
        i = it->data(0, Qt::UserRole).toInt();
    for (int k = 0; i < 0 && k < cuts.size(); k++)
        if (at(pts[cuts[k].point].zero) == cur || at(pts[cuts[k].point].key_time) == cur)
            i = k;
    if (i < 0 || i >= cuts.size()) {
        setNote("No cut selected in the list or starting at this picture.");
        return;
    }
    setNote(QString("Cut removed (the part that started at %1).").arg(msText(pts[cuts[i].point].zero)));
    cuts.remove(i);
    refresh();
}

void CutEditor::clearCuts()
{
    cuts.clear();
    refresh();
    setNote("Every cut removed: with OK the title is ripped whole.");
}

// ---- moving through the title ----

int CutEditor::at(int64_t t) const
{
    int n = int(std::upper_bound(times.begin(), times.end(), t) - times.begin()) - 1;
    return std::clamp(n, 0, int(times.size()) - 1);
}

int CutEditor::keyAtOrBefore(int n) const
{
    auto k = std::upper_bound(keys.begin(), keys.end(), n);
    return k == keys.begin() ? 0 : *(k - 1);
}

int CutEditor::pointOfKey(int64_t key_time) const
{
    int lo = 0, hi = nbPts - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (pts[mid].key_time == key_time)
            return mid;
        if (pts[mid].key_time < key_time)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

QString CutEditor::keyText(int n) const
{
    const CutPicture &pic = ix->pics[order[n]];

    if (!pic.key)
        return QString();
    QString s = QString("key frame, %1").arg(startText(pic.start));
    int p = pointOfKey(pic.time);
    if (p >= 0) {
        int lead = pts[p].kept + pts[p].undecodable + pts[p].left_out;
        if (lead)
            s += QString(" (%1 picture(s) shown before it").arg(lead) +
                 (pts[p].left_out ? QString(", %1 left out at a cut").arg(pts[p].left_out) : QString()) +
                 (pts[p].undecodable ? QString(", %1 kept that need the pictures before").arg(pts[p].undecodable)
                                     : QString()) +
                 ")";
    }
    return s;
}

void CutEditor::show(int n)
{
    n   = std::clamp(n, 0, int(times.size()) - 1);
    cur = n;
    timeline->setPosition(times[n]);
    timeLabel->setText(msText(times[n]));
    timeLabel->setToolTip(QString("Picture %1 of %2 at %3").arg(n + 1).arg(times.size()).arg(exactText(times[n])));
    QString k = keyText(n);
    typeLabel->setText(k.isEmpty() ? QString("…") : "… " + k);
    worker->want(n, picSize);
}

void CutEditor::showAt(int64_t t)
{
    show(at(t));
}

void CutEditor::arrived(int n, QImage image, QChar type)
{
    view->setImage(image);
    if (n == cur) {
        QString k = keyText(n);
        typeLabel->setText(k.isEmpty() ? QString(type) : QString("%1, %2").arg(type).arg(k));
        if (testWaiting)
            endTest();
    }
}

// ---- tests ----

void CutEditor::setTest(const QString &at, const QString &steps, const QString &file)
{
    testAt    = at;
    testSteps = steps;
    testFile  = file;
}

void CutEditor::runTest()
{
    if (!testAt.isEmpty()) {
        QByteArray b = testAt.toUtf8();
        CutRequest *reqs = nullptr;
        const char *bad;
        int nb = 0;
        if (cut_parse(b.constData(), &reqs, &nb, &bad) == 0 && nb == 1 && !reqs[0].chapter)
            showAt(reqs[0].time);
        free(reqs);
    }
    for (const QString &s : testSteps.split(',', Qt::SkipEmptyParts)) {
        if (s == "L" || s == "R")
            show(cur + (s == "R" ? 1 : -1));
        else if (s == "D" || s == "U")
            goKey(s == "U" ? 1 : -1);
        else if (s == "K")
            addCutHere();
        else if (s == "X")
            removeCut();
        else if (s.startsWith('C')) {
            if (QTreeWidgetItem *it = chapterList->topLevelItem(s.mid(1).toInt() - 1))
                chapterList->setCurrentItem(it);
            addChapterCut();
        }
    }
    testWaiting = true;
    worker->want(cur, picSize);     // shown again, in case it was there before the steps
}

void CutEditor::endTest()
{
    testWaiting = false;
    std::printf("cut editor: picture %d of %d at %s (%s), frame type: %s\n", cur + 1, int(times.size()),
                msText(times[cur]).toUtf8().constData(), exactText(times[cur]).toUtf8().constData(),
                typeLabel->text().toUtf8().constData());
    std::printf("cut editor: cuts \"%s\"; parts:", items().toUtf8().constData());
    for (int i = 0; i < partList->topLevelItemCount(); i++)
        std::printf(" %s+%s", partList->topLevelItem(i)->text(1).toUtf8().constData(),
                    partList->topLevelItem(i)->text(2).toUtf8().constData());
    std::printf("\ncut editor: note: %s\n", note->text().toUtf8().constData());
    std::fflush(stdout);
    grab().save(testFile);
    reject();
}

void CutEditor::failed(int n)
{
    if (n == cur)
        typeLabel->setText("this picture cannot be read (the log says why)");
}

void CutEditor::goKey(int dir)
{
    stopPlay();
    if (dir < 0) {
        auto k = std::lower_bound(keys.begin(), keys.end(), cur);
        show(k == keys.begin() ? 0 : *(k - 1));
    } else {
        auto k = std::upper_bound(keys.begin(), keys.end(), cur);
        if (k != keys.end())
            show(*k);
    }
}

void CutEditor::goCut(int dir)
{
    stopPlay();
    int best = -1;
    for (const Cut &c : cuts) {
        int n = at(pts[c.point].zero);
        if (dir < 0 ? n < cur && n > best : n > cur && (best < 0 || n < best))
            best = n;
    }
    if (best >= 0)
        show(best);
}

void CutEditor::goChapter(int dir)
{
    stopPlay();
    int best = -1;
    for (int c = 0; c < ix->nb_chapters; c++) {
        int n = at(ix->chapters[c]);
        if (dir < 0 ? n < cur && n > best : n > cur && (best < 0 || n < best))
            best = n;
    }
    if (best >= 0)
        show(best);
}

void CutEditor::goTime()
{
    bool ok = false;
    QString s = QInputDialog::getText(this, "Go to", "A time on the title (h:mm:ss.fff or m:ss.fff) or a chapter (c5):",
                                      QLineEdit::Normal, msText(times[cur]), &ok);
    CutRequest *reqs = nullptr;
    const char *bad;
    int nb = 0;

    if (!ok || s.trimmed().isEmpty())
        return;
    QByteArray b = s.trimmed().toUtf8();
    if (cut_parse(b.constData(), &reqs, &nb, &bad) < 0 || nb != 1) {
        free(reqs);
        setNote(QString("\"%1\" is not a time or a chapter.").arg(s));
        return;
    }
    stopPlay();
    if (reqs[0].chapter > 0 && reqs[0].chapter <= ix->nb_chapters)
        showAt(ix->chapters[reqs[0].chapter - 1]);
    else if (reqs[0].chapter > 0)
        setNote(QString("The title has %1 chapter(s).").arg(ix->nb_chapters));
    else
        showAt(reqs[0].time);
    free(reqs);
}

// ---- play: the pictures at the title's pace (no sound); pictures that
// cannot be decoded in time are passed over ----

void CutEditor::togglePlay()
{
    if (playTimer.isActive()) {
        stopPlay();
        return;
    }
    if (cur >= times.size() - 1)
        return;
    playFrom = times[cur];
    playClock.start();
    playTimer.start(10);
    playBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
}

void CutEditor::stopPlay()
{
    if (!playTimer.isActive())
        return;
    playTimer.stop();
    playBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
}

void CutEditor::playTick()
{
    int n = at(playFrom + playClock.elapsed() * TPS / 1000);

    if (n >= times.size() - 1) {
        stopPlay();
        show(times.size() - 1);
    } else if (n != cur)
        show(n);
}
