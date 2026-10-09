// disc-remuxer-gui [<disc or folder> ...]: the GUI over the engine
// (engine.h), the same engine and settings file (disc-remuxer.toml next to
// the program) as the command line. Discs named on the command line are
// put into the queue. DISC_REMUXER_GUI_SNAPSHOT=<file.png> (tests): the
// window saved as a picture once they are scanned, then the program quits.
#include <QApplication>
#include <QMessageBox>

#include "MainWindow.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    int ret;

    QApplication::setApplicationName("disc-remuxer-gui");
    QApplication::setApplicationVersion(DISC_REMUXER_VERSION);
    if ((ret = engine_init(nullptr, nullptr, 0, 0, 0))) {
        QMessageBox::critical(nullptr, "Disc Remuxer", "The settings file next to the program cannot be read "
                                                       "(run disc-remuxer settings in a terminal to see why).");
        return ret;
    }
    MainWindow w;
    QStringList discs = QApplication::arguments().mid(1);
    QString snapshot = qEnvironmentVariable("DISC_REMUXER_GUI_SNAPSHOT");
    w.show();
    if (!snapshot.isEmpty() && !discs.isEmpty())
        w.snapshotWhenScanned(snapshot);
    w.addPaths(discs);
    return app.exec();
}
