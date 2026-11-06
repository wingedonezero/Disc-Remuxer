// disc-remuxer-gui [<disc or folder> ...]: the GUI over the engine
// (engine.h), the same engine and settings file (disc-remuxer.toml next to
// the program) as the command line. Discs named on the command line are
// put into the queue. DISC_REMUXER_GUI_SNAPSHOT=<file.png> (tests): the
// window saved as a picture once they are scanned (DISC_REMUXER_GUI_TEST=rip:
// once they are ripped too; =stop: the rip stopped after a second;
// =uncheck: the disc unchecked; =expand: the first title opened; =prefs:
// the settings menu; =menu: the first title's right-click menu, =menu:<item>:
// that item of it chosen, then the window with the title opened;
// =cuts:<title>:<time>:<keys>: the cut editor of a title,
// CutEditor::setTest()); DISC_REMUXER_GUI_TITLES: the titles ripped,
// DISC_REMUXER_GUI_CUT_LIST: "<title>:<cuts>", a title's cut list), then the
// program quits.
#include <QApplication>
#include <QMessageBox>

#include <sys/resource.h>

#include "MainWindow.h"

extern "C" {
#include "base/log.h"
}

// Every disc in the queue keeps its files open (a DVD folder about 23), and
// a desktop starts programs allowed 1024: a queue of a few box sets reaches
// that, and the window then cannot get the file it draws with. The limit
// raised to this many (as far as the system's ceiling allows).
static const rlim_t open_files = 5000;

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    int ret;

    QApplication::setApplicationName("disc-remuxer-gui");
    QApplication::setApplicationVersion(DISC_REMUXER_VERSION);
    // the engine's log to the window, from the settings file's reading on
    LogRelay relay;
    if ((ret = engine_init(nullptr, nullptr, 0, 0, 0))) {
        QMessageBox::critical(nullptr, "Disc Remuxer", "The settings file next to the program cannot be read "
                                                       "(run disc-remuxer settings in a terminal to see why).");
        return ret;
    }
    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < open_files) {
        rlim_t was = rl.rlim_cur;
        rl.rlim_cur = rl.rlim_max == RLIM_INFINITY || rl.rlim_max > open_files ? open_files : rl.rlim_max;
        if (!setrlimit(RLIMIT_NOFILE, &rl))
            log_text(LOG_DETAIL, "Open files allowed: %llu (was %llu)", (unsigned long long)rl.rlim_cur,
                     (unsigned long long)was);
        else
            log_text(LOG_WARNING, "Open files allowed: %llu, could not be raised (a long queue may run out)",
                     (unsigned long long)was);
    }
    MainWindow w(&relay);
    QStringList discs = QApplication::arguments().mid(1);
    QString snapshot = qEnvironmentVariable("DISC_REMUXER_GUI_SNAPSHOT");
    QString test = qEnvironmentVariable("DISC_REMUXER_GUI_TEST");
    w.show();
    if (!snapshot.isEmpty() && !discs.isEmpty())
        w.snapshotWhenScanned(snapshot, test);
    w.addPaths(discs);
    return app.exec();
}
