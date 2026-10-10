// disc-remuxer-gui [<disc or folder> ...]: the GUI over the engine
// (engine.h), the same engine and settings file (disc-remuxer.toml next to
// the program) as the command line. Discs named on the command line are
// put into the queue. DISC_REMUXER_GUI_SNAPSHOT=<file.png> (tests): the
// window saved as a picture once they are scanned (DISC_REMUXER_GUI_TEST=rip:
// once they are ripped too; =stop: the rip stopped after a second;
// =uncheck: the disc unchecked; =expand: the first title opened; =prefs:
// the settings menu), then the
// program quits.
#include <QApplication>
#include <QMessageBox>

#include "MainWindow.h"

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
