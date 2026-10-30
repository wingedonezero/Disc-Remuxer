// The cut editor: a title's pictures one by one, its chapters and the cuts
// it is split at (cuts.h), laid out as Avidemux's main window (the picture,
// a timeline and the navigation buttons below it, the same keys) with the
// cut list on the right. It reads the title on the disc as it is open (its
// index, then pictures from the place of a key frame) on a thread of its
// own; closing it frees all of that: only the title's cut list stays.
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QThread>
#include <QVector>
#include <QWidget>

extern "C" {
#include "engine.h"
#include "cuts.h"
#include "cut_preview.h"
#include "cut_modes.h"
}

class LogRelay;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QProgressBar;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QTreeWidget;

// The title's index and pictures, on the editor's thread. A picture asked
// for replaces one asked for before that has not been begun (scrubbing
// decodes only the newest).
class PreviewWorker : public QObject {
    Q_OBJECT
public:
    PreviewWorker(EngineDisc *disc, int title) : disc(disc), title(title) {}
    // any thread
    void want(int n, QSize size);
    void stopWork();

public slots:
    void open();
    void work();
    void finish();

signals:
    void opened(bool ok);
    void picture(int n, QImage image, QChar type);
    void failed(int n);

private:
    EngineDisc *disc;
    int         title;
    CutPreview *preview = nullptr;
    QMutex      lock;
    int         wantN = -1;
    QSize       wantSize;
    bool        scheduled = false, stopping = false;
};

// The title from 0 to its end: its parts shaded in turn, chapter ticks,
// the cuts as red lines, the picture shown. A click or a drag moves it.
class CutTimeline : public QWidget {
    Q_OBJECT
public:
    explicit CutTimeline(QWidget *parent = nullptr);
    void setTitle(int64_t end, const QVector<int64_t> &chapters);
    void setCuts(const QVector<int64_t> &zeros);
    // where an analysis' suggestions would start parts (dashed, not cuts yet)
    void setSuggested(const QVector<int64_t> &zeros);
    void setPosition(int64_t t);
    QSize sizeHint() const override { return QSize(400, 34); }

signals:
    void dragged(int64_t t);
    void released(int64_t t);
    void stepped(int delta);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    int64_t timeAt(int x) const;
    int     xOf(int64_t t) const;

    int64_t          end = 0, pos = 0;
    QVector<int64_t> chapters, cuts, suggested;
    bool             dragging = false;
};

// The picture, its aspect kept, on black.
class PictureView : public QWidget {
    Q_OBJECT
public:
    explicit PictureView(QWidget *parent = nullptr);
    void setImage(const QImage &img);

signals:
    void stepped(int delta);

protected:
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    QImage image;
};

class CutEditor : public QDialog {
    Q_OBJECT
public:
    CutEditor(EngineDisc *disc, int title, LogRelay *relay, QWidget *parent = nullptr);
    ~CutEditor() override;

    // The cut list as the title keeps it ("" none), after OK.
    QString items() const;
    // Tests: once open, the picture at time `at` shown, then the keys of
    // steps pressed (L R frames, D U key frames, K add a cut, X remove it,
    // C<n> a cut at chapter n; the analysis: M<mode key>, T<target minutes>,
    // E<episodes>, N<min minutes>, A analyze, S add the suggestions; comma
    // separated); once its picture is there
    // the window saved as a picture into file, what it shows printed, and it
    // closes (its sizes are not kept).
    void setTest(const QString &at, const QString &steps, const QString &file);

public slots:
    void reject() override;
    void accept() override;

private slots:
    void opened(bool ok);
    void arrived(int n, QImage image, QChar type);
    void failed(int n);
    void progress(int title, double done);

private:
    struct Cut {
        QString item;      // as the title keeps it: "c5", or the key frame's time
        int     point;     // in pts
        QString from;      // "manual", "chapter 5"
    };

    void    buildUi();
    void    shutdown();
    void    loadCuts();
    void    sortCuts();
    void    refresh();       // the lists and the timeline after a change of the cuts
    void    show(int n);     // picture n (display order) asked for
    void    showAt(int64_t t);
    int     at(int64_t t) const;
    int     keyAtOrBefore(int n) const;
    void    stopPlay();
    void    togglePlay();
    void    playTick();
    void    goKey(int dir);
    void    goCut(int dir);
    void    goChapter(int dir);
    void    goTime();
    void    addCutHere();
    void    addChapterCut();
    void    removeCut();
    void    clearCuts();
    void    setNote(const QString &s);
    QString keyText(int n) const;
    int     pointOfKey(int64_t key_time) const;
    void    saveState();
    void    loadState();
    // the analysis (cut_modes.h): the mode's page and the chapters' check
    // boxes, the run, its suggestions added
    void    modeChanged();
    void    analyze();
    void    addSuggested();
    void    storeHelperValues();
    QVector<int> ticked() const;
    // the requests resolved as cuts (refused ones said in the note)
    void    addItems(const QString &items, const QString &from);
    void    runTest();
    void    endTest();

    EngineDisc   *disc;
    int           title;
    const DiscTitleInfo *ti;
    QThread       thread;
    PreviewWorker *worker;
    bool          loading = true, closeWhenOpened = false, saved = false;
    QString       testAt, testSteps, testFile;
    bool          testWaiting = false;

    // the title (display order)
    const CutIndex  *ix = nullptr;
    QVector<int>     order;      // display number -> the index's picture
    QVector<int64_t> times;
    QVector<int>     keys;       // display numbers of the key frames
    CutPoint        *pts = nullptr;
    int              nbPts = 0;
    int              rule = CUT_NEAREST;
    QSize            picSize;
    int              cur = 0;

    QVector<Cut>     cuts;
    QString          suggested;    // the last analysis' split points as cut list items

    // play: pictures at the title's own pace from where it started
    QTimer        playTimer;
    QElapsedTimer playClock;
    int64_t       playFrom = 0;

    QStackedWidget *stack;
    QProgressBar   *loadBar;
    QLabel         *loadLabel;
    PictureView    *view;
    CutTimeline    *timeline;
    QToolButton    *playBtn;
    QPushButton    *timeBtn, *okButton;
    QLabel         *timeLabel, *totalLabel, *typeLabel, *note;
    QTreeWidget    *cutList, *partList, *chapterList;
    QSplitter      *split, *sideSplit;
    QComboBox      *modeBox;
    QStackedWidget *modePages;
    QLineEdit      *targetEdit, *stampEdit;
    QDoubleSpinBox *minSpin;
    QSpinBox       *episodesSpin, *fromEndSpin;
    QPlainTextEdit *analysisLog;
    QPushButton    *addSuggestedBtn;
    QWidget        *controls, *side;
};
