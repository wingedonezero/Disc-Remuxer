#include "CutEditor.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpressionValidator>
#include <QSpinBox>
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
#include <QTabWidget>
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

// Whether a cut at p is clean, for the note line (the Cut column's
// tooltip and the log say it in full).
static QString verdictNote(const CutPoint &p)
{
    char brief[64], full[768];
    QString s;

    if (cut_point_clean(&p, brief, sizeof(brief), full, sizeof(full)))
        return " A clean cut: nothing lost.";
    s = " ⚠ Not a clean cut (an open key frame):";
    if (p.left_out)
        s += QString(" the part before loses its last %1 picture(s) (%2 s)").arg(p.left_out)
                 .arg(double(p.lost) / DR_TICKS_PER_SECOND, 0, 'f', 3);
    if (p.undecodable)
        s += QString("%1 this part starts with %2 picture(s) players do not show (its first shown at %3 s)")
                 .arg(p.left_out ? ";" : "").arg(p.undecodable).arg(double(p.shown) / DR_TICKS_PER_SECOND, 0, 'f', 3);
    return s + ". Alt+Up / Alt+Down: the clean key frames.";
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

void CutTimeline::setCuts(const QVector<int64_t> &zeros, const QVector<bool> &cl)
{
    cuts  = zeros;
    clean = cl;
    update();
}

void CutTimeline::setSuggested(const QVector<int64_t> &zeros)
{
    suggested = zeros;
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
    // a number right of its tick, or left of it where the end leaves no room
    QFontMetrics fm(f);
    int last = -100;
    for (int i = 0; i < chapters.size(); i++) {
        int x = xOf(chapters[i]);
        QString n = QString::number(i + 1);
        int w = fm.horizontalAdvance(n), tx = x + 2 + w <= width() ? x + 2 : x - 2 - w;
        p.drawLine(x, top + bar, x, top + bar + 4);
        if (tx - last >= 4) {
            p.drawText(tx, height() - 1, n);
            last = tx + w;
        }
    }
    // suggestions not added yet
    p.setPen(QPen(QColor(240, 140, 0), 2, Qt::DashLine));
    for (int64_t c : suggested)
        p.drawLine(xOf(c), top, xOf(c), top + bar);
    // the cuts; one that is not clean with an orange mark on top
    p.setPen(QPen(QColor(220, 30, 30), 2));
    for (int64_t c : cuts)
        p.drawLine(xOf(c), top, xOf(c), top + bar);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(240, 140, 0));
    for (int i = 0; i < cuts.size(); i++)
        if (i < clean.size() && !clean[i])
            p.drawPolygon(QPolygon({ QPoint(xOf(cuts[i]) - 5, top), QPoint(xOf(cuts[i]) + 5, top),
                                     QPoint(xOf(cuts[i]), top + 8) }));
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
    // a step button held down steps on (as Avidemux's), every ms
    auto repeat = [](QToolButton *b, int ms) {
        b->setAutoRepeat(true);
        b->setAutoRepeatDelay(300);
        b->setAutoRepeatInterval(ms);
    };
    playBtn = button("", QStyle::SP_MediaPlay, "Play / pause (Space) - pictures only", [this] { togglePlay(); });
    sep();
    repeat(button("", QStyle::SP_MediaSeekBackward, "Previous frame (Left)", [this] { stopPlay(); show(cur - 1); }), 40);
    repeat(button("", QStyle::SP_MediaSeekForward, "Next frame (Right)", [this] { stopPlay(); show(cur + 1); }), 40);
    repeat(button("", QStyle::SP_MediaSkipBackward, "Previous key frame (Down)", [this] { goKey(-1); }), 150);
    repeat(button("", QStyle::SP_MediaSkipForward, "Next key frame (Up)", [this] { goKey(1); }), 150);
    repeat(button("◀ Clean", -1, "Previous key frame a cut at is clean: nothing lost (Alt+Down)", [this] { goClean(-1); }), 250);
    repeat(button("Clean ▶", -1, "Next key frame a cut at is clean: nothing lost (Alt+Up)", [this] { goClean(1); }), 250);
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
    repeat(button("-1 min", -1, "One minute back (Ctrl+Down)", [this] { stopPlay(); showAt(times[cur] - 60 * TPS); }), 250);
    repeat(button("+1 min", -1, "One minute on (Ctrl+Up)", [this] { stopPlay(); showAt(times[cur] + 60 * TPS); }), 250);
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
    cutList = list({ "#", "Part starts", "Cut", "From" });
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

    // the analysis: the MKV splitter's modes, fields and log
    auto *anBox = new QWidget;
    auto *ab = new QVBoxLayout(anBox);
    auto *af = new QFormLayout;
    modeBox = new QComboBox;
    for (int m = 0; m < CUT_MODE_COUNT; m++)
        modeBox->addItem(cut_mode_names[m]);
    modeBox->setFocusPolicy(Qt::NoFocus);
    af->addRow("Analysis Mode:", modeBox);
    ab->addLayout(af);
    modePages = new QStackedWidget;
    auto page = [this](const QString &label, QWidget *field) {
        auto *w = new QWidget;
        auto *f = new QFormLayout(w);
        f->setContentsMargins(0, 0, 0, 0);
        f->addRow(label, field);
        modePages->addWidget(w);
        return f;
    };
    targetEdit = new QLineEdit;
    targetEdit->setPlaceholderText("mm:ss, e.g. 23:40");
    targetEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(R"(\d{1,3}(:[0-5]?\d?|\.\d{0,3})?)"),
                                                             targetEdit));
    minSpin = new QDoubleSpinBox;
    minSpin->setSuffix(" min");
    minSpin->setRange(0.1, 240);
    episodesSpin = new QSpinBox;
    episodesSpin->setRange(2, 100);
    fromEndSpin = new QSpinBox;
    fromEndSpin->setRange(1, 100);
    page("Target Episode Duration (mm:ss):", targetEdit);
    page("Min Content Duration:", minSpin);
    page("Expected # of Episodes:", episodesSpin);
    page("Chapters to Remove from End:", fromEndSpin);
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        auto *hint = new QLabel("Analyze, then tick chapters in the Chapters tab to split before them.");
        auto *row = new QHBoxLayout;
        v->setContentsMargins(0, 0, 0, 0);
        hint->setWordWrap(true);
        v->addWidget(hint);
        for (bool on : { true, false }) {
            auto *b = new QPushButton(on ? "Select All Chapters" : "Clear Selection");
            b->setFocusPolicy(Qt::NoFocus);
            b->setAutoDefault(false);
            connect(b, &QPushButton::clicked, this, [this, on] {
                for (int i = 0; i < chapterList->topLevelItemCount(); i++)
                    if (QTreeWidgetItem *it = chapterList->topLevelItem(i); it->flags() & Qt::ItemIsUserCheckable)
                        it->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
            });
            row->addWidget(b);
        }
        row->addStretch();
        v->addLayout(row);
        modePages->addWidget(w);
    }
    stampEdit = new QLineEdit;
    stampEdit->setPlaceholderText("e.g. 21:30, 44:10.500  (HH:MM:SS, MM:SS, or 90s)");
    stampEdit->setToolTip("A part starts at each time (at a key frame: setting cuts.key_frame).\n"
                          "Double-click a row in the Chapters list to paste that chapter's start time.");
    page("Split after timestamps:", stampEdit);
    ab->addWidget(modePages);
    auto *arow = new QHBoxLayout;
    auto *analyzeBtn = new QPushButton("Analyze");
    addSuggestedBtn = new QPushButton("Add Suggested Cuts");
    for (QPushButton *b : { analyzeBtn, addSuggestedBtn }) {
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoDefault(false);
        arow->addWidget(b);
    }
    arow->addStretch();
    addSuggestedBtn->setEnabled(false);
    connect(analyzeBtn, &QPushButton::clicked, this, [this] {
        if (ix)
            analyze();
    });
    connect(addSuggestedBtn, &QPushButton::clicked, this, [this] {
        if (ix)
            addSuggested();
    });
    ab->addLayout(arow);
    analysisLog = new QPlainTextEdit;
    analysisLog->setReadOnly(true);
    analysisLog->setLineWrapMode(QPlainTextEdit::NoWrap);
    analysisLog->setFocusPolicy(Qt::NoFocus);
    {
        QFont mono = analysisLog->font();
        mono.setFamily("monospace");
        mono.setStyleHint(QFont::Monospace);
        analysisLog->setFont(mono);
    }
    ab->addWidget(analysisLog, 1);
    // the values last used (as the splitter keeps them)
    {
        int m = cut_mode_of_key(setting_text("cuts.helper_mode"));
        double v;
        char buf[32], err[160];
        modeBox->setCurrentIndex(m < 0 ? 0 : m);
        targetEdit->setText(cut_mode_target(setting_text("cuts.helper_target"), &v, err, sizeof(err)) < 0
                                ? QString::fromUtf8(setting_text("cuts.helper_target"))
                                : (cut_mode_target_text(v, buf, sizeof(buf)), QString::fromUtf8(buf)));
        minSpin->setValue(QString::fromUtf8(setting_text("cuts.helper_min_minutes")).toDouble());
        episodesSpin->setValue(int(setting_int("cuts.helper_episodes")));
        fromEndSpin->setValue(int(setting_int("cuts.helper_chapters_from_end")));
        stampEdit->setText(QString::fromUtf8(setting_text("cuts.helper_timestamps")));
    }
    connect(modeBox, &QComboBox::currentIndexChanged, this, [this] { modeChanged(); });

    auto *partBox = new QGroupBox("Parts");
    auto *pb = new QVBoxLayout(partBox);
    partList = list({ "Part", "Start", "Length" });
    pb->addWidget(partList);
    connect(partList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
        stopPlay();
        showAt(it->data(0, Qt::UserRole).toLongLong());
    });
    sideSplit->addWidget(partBox);

    auto *chBox = new QWidget;
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
        // After Timestamps: its start pasted into the times (as the splitter does)
        if (modeBox->currentIndex() == CUT_MODE_TIMESTAMPS) {
            QString t = stampEdit->text().trimmed(), s = it->data(1, Qt::UserRole).toString();
            stampEdit->setText(t.isEmpty() ? s : t + ", " + s);
        }
        stopPlay();
        showAt(it->data(0, Qt::UserRole).toLongLong());
    });
    // the analysis and the chapters under tabs, below the cuts and their parts
    tabs = new QTabWidget;
    tabs->addTab(anBox, "Analysis");
    tabs->addTab(chBox, "Chapters");
    tabs->setFocusPolicy(Qt::NoFocus);
    sideSplit->addWidget(tabs);
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
    key(QKeySequence(Qt::ALT | Qt::Key_Down), [this] { goClean(-1); });
    key(QKeySequence(Qt::ALT | Qt::Key_Up), [this] { goClean(1); });
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
    resize(1440, 900);
    split->setSizes({ 920, 520 });
    sideSplit->setSizes({ 260, 170, 470 });
}

void CutEditor::loadState()
{
    const QStringList s = QString::fromUtf8(setting_text("gui.cut_editor")).split(',', Qt::SkipEmptyParts);

    if (s.size() != 7)
        return;
    restoreGeometry(QByteArray::fromBase64(s[0].toLatin1()));
    split->setSizes({ s[1].toInt(), s[2].toInt() });
    sideSplit->setSizes({ s[3].toInt(), s[4].toInt(), s[5].toInt() });
    tabs->setCurrentIndex(s[6].toInt());
}

void CutEditor::saveState()
{
    QStringList s = { QString::fromLatin1(saveGeometry().toBase64()) };
    char err[256];

    if (saved || !testFile.isEmpty())
        return;
    saved = true;
    storeHelperValues();
    {
        // the target kept as the field shows it (one it cannot read: the default)
        double v;
        char buf[32], err[160];
        if (cut_mode_target(targetEdit->text().toUtf8().constData(), &v, err, sizeof(err)) < 0)
            snprintf(buf, sizeof(buf), "23:00");
        else
            cut_mode_target_text(v, buf, sizeof(buf));
        settings_store("cuts.helper_target", buf, err, sizeof(err));
    }
    for (int v : split->sizes())
        s << QString::number(v);
    for (int v : sideSplit->sizes())
        s << QString::number(v);
    s << QString::number(tabs->currentIndex());
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
    for (int k : keys) {
        int p = pointOfKey(times[k]);
        char brief[64], full[768];
        if (p >= 0 && cut_point_clean(&pts[p], brief, sizeof(brief), full, sizeof(full)))
            cleanKeys << k;
    }
    QVector<int64_t> chapters;
    for (int c = 0; c < ix->nb_chapters; c++) {
        int64_t s = ix->chapters[c], e = c + 1 < ix->nb_chapters ? ix->chapters[c + 1] : ix->end;
        auto *it = new QTreeWidgetItem(chapterList, { QString::number(c + 1), msText(s), msText(e - s) });
        char st[40];
        cut_mode_start_text((s * 25 + 13) / 27, st, sizeof(st));
        it->setData(0, Qt::UserRole, (qlonglong)s);
        it->setData(1, Qt::UserRole, QString::fromUtf8(st));
        it->setToolTip(1, exactText(s));
        chapters << s;
    }
    timeline->setTitle(ix->end, chapters);
    modeChanged();
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
    QVector<bool> cleanCuts;

    cutList->clear();
    for (int i = 0; i < cuts.size(); i++) {
        const CutPoint &p = pts[cuts[i].point];
        char brief[64], full[768];
        bool ok = cut_point_clean(&p, brief, sizeof(brief), full, sizeof(full));
        auto *it = new QTreeWidgetItem(cutList, { QString::number(i + 1), msText(p.zero), QString::fromUtf8(brief),
                                                  cuts[i].from });
        it->setData(0, Qt::UserRole, i);
        it->setToolTip(1, QString("The part starts at %1; its key frame at %2").arg(exactText(p.zero),
                                                                                     exactText(p.key_time)));
        it->setToolTip(2, QString::fromUtf8(full));
        if (!ok) {
            it->setForeground(2, QColor(200, 110, 0));
            it->setIcon(2, style()->standardIcon(QStyle::SP_MessageBoxWarning));
        }
        zeros << p.zero;
        cleanCuts << ok;
    }
    partList->clear();
    QVector<int64_t> b = { 0 };
    b += zeros;
    b += ix->end;
    for (int i = 0; i + 1 < b.size(); i++) {
        auto *it = new QTreeWidgetItem(partList, { QString::number(i + 1), msText(b[i]), msText(b[i + 1] - b[i]) });
        it->setData(0, Qt::UserRole, (qlonglong)b[i]);
    }
    timeline->setCuts(zeros, cleanCuts);
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
    QString verdict = verdictNote(p);
    if (r.on_point || !moved)
        setNote(QString("Cut added: the part starts at %1.").arg(msText(p.zero)) + verdict);
    else
        setNote(QString("Not a key frame: moved to the %1 key frame, %2 picture(s) %3; the part starts at %4.")
                    .arg(rule == CUT_NEXT ? "next" : "nearest")
                    .arg(std::abs(moved))
                    .arg(moved > 0 ? "later" : "earlier")
                    .arg(msText(p.zero)) + verdict);
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
    QString verdict = verdictNote(p);
    setNote((r.on_point ? QString("Cut added at chapter %1: the part starts at %2.").arg(req.chapter).arg(msText(p.zero))
                        : QString("Chapter %1's mark is not on a key frame: the %2 one is taken; the part starts at %3.")
                              .arg(req.chapter)
                              .arg(rule == CUT_NEXT ? "next" : "nearest")
                              .arg(msText(p.zero))) + verdict);
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

// ---- the analysis ----

// The mode's own field(s); Before Chapters: check boxes on the chapters
// (never on one at 0:00).
void CutEditor::modeChanged()
{
    int m = modeBox->currentIndex();

    modePages->setCurrentIndex(m == CUT_MODE_TIME ? 0 : m == CUT_MODE_EPISODES ? 2 : m == CUT_MODE_REMOVE_END ? 3 :
                               m == CUT_MODE_CHAPTERS ? 4 : m == CUT_MODE_TIMESTAMPS ? 5 : 1);
    for (int i = 0; ix && i < chapterList->topLevelItemCount(); i++) {
        QTreeWidgetItem *it = chapterList->topLevelItem(i);
        bool can = m == CUT_MODE_CHAPTERS && (ix->chapters[i] * 25 + 13) / 27 / 1000 > 0;
        if (can) {
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            if (!it->data(0, Qt::CheckStateRole).isValid())
                it->setCheckState(0, Qt::Unchecked);
        } else {
            it->setFlags(it->flags() & ~Qt::ItemIsUserCheckable);
            it->setData(0, Qt::CheckStateRole, QVariant());
        }
    }
}

QVector<int> CutEditor::ticked() const
{
    QVector<int> t;
    for (int i = 0; i < chapterList->topLevelItemCount(); i++)
        if (QTreeWidgetItem *it = chapterList->topLevelItem(i);
            (it->flags() & Qt::ItemIsUserCheckable) && it->checkState(0) == Qt::Checked)
            t << i + 1;
    return t;
}

// The box's values into the settings the analysis reads (kept when the
// editor closes, as the splitter keeps its last ones).
void CutEditor::storeHelperValues()
{
    char err[256];

    settings_store("cuts.helper_mode", cut_mode_keys[modeBox->currentIndex()], err, sizeof(err));
    settings_store("cuts.helper_target", targetEdit->text().toUtf8().constData(), err, sizeof(err));
    settings_store("cuts.helper_min_minutes", QString::number(minSpin->value()).toUtf8().constData(), err, sizeof(err));
    settings_store("cuts.helper_episodes", QByteArray::number(episodesSpin->value()).constData(), err, sizeof(err));
    settings_store("cuts.helper_chapters_from_end", QByteArray::number(fromEndSpin->value()).constData(), err,
                   sizeof(err));
    settings_store("cuts.helper_timestamps", stampEdit->text().toUtf8().constData(), err, sizeof(err));
}

void CutEditor::analyze()
{
    QVector<int> t = ticked();
    CutModeResult r;
    char *items = nullptr;

    storeHelperValues();
    if (engine_cut_mode(ix, t.data(), t.size(), &r, &items) < 0) {
        analysisLog->setPlainText("The analysis could not be run (no memory).");
        return;
    }
    analysisLog->setPlainText(QString::fromUtf8(r.log));
    suggested = QString::fromUtf8(items);
    cut_mode_result_free(&r);
    free(items);
    // where they would start parts, by the rule the cuts take
    QVector<int64_t> zeros;
    QByteArray b = suggested.toUtf8();
    CutRequest *reqs = nullptr;
    const char *bad;
    int nb = 0;
    if (!suggested.isEmpty() && cut_parse(b.constData(), &reqs, &nb, &bad) == 0) {
        QVector<CutResult> res(nb);
        cut_resolve(pts, nbPts, ix->chapters, ix->nb_chapters, ix->end, reqs, nb, rule, res.data());
        for (const CutResult &x : res)
            if (x.point >= 0)
                zeros << pts[x.point].zero;
    }
    free(reqs);
    timeline->setSuggested(zeros);
    addSuggestedBtn->setEnabled(!suggested.isEmpty());
    setNote(suggested.isEmpty() ? QString("The analysis suggests no cut.")
                                : QString("Suggested: %1 (dashed on the timeline). Add Suggested Cuts adds them.")
                                      .arg(suggested));
}

void CutEditor::addSuggested()
{
    addItems(suggested, cut_mode_names[modeBox->currentIndex()]);
    suggested.clear();
    timeline->setSuggested({});
    addSuggestedBtn->setEnabled(false);
}

// Items as the cut list keeps them, each placed by the cuts' rule; one on a
// key frame that has a cut already, or refused, is said and left out.
void CutEditor::addItems(const QString &items, const QString &from)
{
    QByteArray b = items.toUtf8();
    CutRequest *reqs = nullptr;
    const char *bad;
    QStringList left;
    int nb = 0, added = 0, open = 0;

    if (items.isEmpty() || cut_parse(b.constData(), &reqs, &nb, &bad) < 0)
        return;
    QVector<CutResult> res(nb);
    cut_resolve(pts, nbPts, ix->chapters, ix->nb_chapters, ix->end, reqs, nb, rule, res.data());
    for (int i = 0; i < nb; i++) {
        QString text = QString::fromUtf8(reqs[i].text);
        bool there = false;
        if (res[i].point < 0) {
            left << QString("%1 (%2)").arg(text, res[i].same_as >= 0 ? QString("the same key frame as %1")
                                                                           .arg(QString::fromUtf8(reqs[res[i].same_as].text))
                                                                     : QString::fromUtf8(res[i].why));
            continue;
        }
        for (const Cut &c : cuts)
            there |= c.point == res[i].point;
        if (there) {
            left << QString("%1 (a cut is already at its key frame)").arg(text);
            continue;
        }
        cuts << Cut{ text, res[i].point, from };
        added++;
        char brief[64], full[768];
        open += !cut_point_clean(&pts[res[i].point], brief, sizeof(brief), full, sizeof(full));
    }
    free(reqs);
    sortCuts();
    refresh();
    setNote(QString("%1 cut(s) added from %2").arg(added).arg(from) +
            (open ? QString(", ⚠ %1 of them not clean (see the Cut column).").arg(open)
                  : added ? QString(", all clean.") : QString(".")) +
            (left.isEmpty() ? QString() : " Left out: " + left.join("; ") + "."));
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
        char brief[64], full[768];
        if (cut_point_clean(&pts[p], brief, sizeof(brief), full, sizeof(full)))
            s += ": a cut here is clean";
        else
            s += ": a cut here " +
                 QStringList({ pts[p].left_out ? QString("loses %1 picture(s)").arg(pts[p].left_out) : QString(),
                               pts[p].undecodable ? QString("starts with %1 not shown").arg(pts[p].undecodable)
                                                  : QString() }).filter(QRegularExpression(".")).join(", ");
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
        else if (s == "CD" || s == "CU")
            goClean(s == "CU" ? 1 : -1);
        else if (s == "K")
            addCutHere();
        else if (s == "X")
            removeCut();
        else if (s.startsWith('M'))
            modeBox->setCurrentIndex(std::max(0, cut_mode_of_key(s.mid(1).toUtf8().constData())));
        else if (s.startsWith('T'))
            targetEdit->setText(s.mid(1));
        else if (s.startsWith('E'))
            episodesSpin->setValue(s.mid(1).toInt());
        else if (s.startsWith('N'))
            minSpin->setValue(s.mid(1).toDouble());
        else if (s == "A")
            analyze();
        else if (s == "S")
            addSuggested();
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
    if (!analysisLog->toPlainText().isEmpty())
        std::printf("cut editor: analysis log:\n%s\n", analysisLog->toPlainText().toUtf8().constData());
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

void CutEditor::goClean(int dir)
{
    stopPlay();
    if (dir < 0) {
        auto k = std::lower_bound(cleanKeys.begin(), cleanKeys.end(), cur);
        if (k != cleanKeys.begin())
            show(*(k - 1));
    } else {
        auto k = std::upper_bound(cleanKeys.begin(), cleanKeys.end(), cur);
        if (k != cleanKeys.end())
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
