#include "DiscFinder.h"

#include <QDir>
#include <QFileInfo>

static const char *const discDirs[] = { "VIDEO_TS", "BDMV", "HVDVD_TS", "HDDVD_TS" };

bool isDiscImage(const QString &path)
{
    QFileInfo fi(path);
    QString ext = fi.suffix().toLower();
    return fi.isFile() && (ext == "iso" || ext == "img");
}

// A folder holding a disc's structure, or being one (VIDEO_TS.IFO or
// index.bdmv directly in it).
static bool isDiscFolder(const QDir &d)
{
    for (const char *sub : discDirs)
        if (d.exists(sub) && QFileInfo(d.filePath(sub)).isDir())
            return true;
    return d.exists("VIDEO_TS.IFO") || d.exists("video_ts.ifo") || d.exists("index.bdmv") ||
           d.exists("INDEX.BDMV");
}

// A VIDEO_TS or BDMV folder says nothing about the disc: its parent's name.
static QString folderName(const QDir &d)
{
    QString n = d.dirName();
    if (n.compare("VIDEO_TS", Qt::CaseInsensitive) == 0 || n.compare("BDMV", Qt::CaseInsensitive) == 0) {
        QDir up(d);
        if (up.cdUp())
            return up.dirName();
    }
    return n;
}

static void search(const QDir &dir, const QDir &root, int depth, int maxDepth, QList<FoundDisc> &out)
{
    if (depth > maxDepth)
        return;
    if (isDiscFolder(dir)) {
        out.append({ dir.absolutePath(), folderName(dir), root.relativeFilePath(dir.absolutePath()),
                     root.absolutePath() });
        return;
    }
    // images and subfolders as they come in name order: a folder may hold
    // both (extras as an image, the volumes as disc folders)
    const QFileInfoList items = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                  QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &fi : items) {
        if (fi.isFile() && isDiscImage(fi.filePath()))
            out.append({ fi.absoluteFilePath(), fi.completeBaseName(), root.relativeFilePath(fi.absoluteFilePath()),
                         root.absolutePath() });
        else if (fi.isDir())
            search(QDir(fi.absoluteFilePath()), root, depth + 1, maxDepth, out);
    }
}

QList<FoundDisc> findDiscs(const QString &path, int maxDepth)
{
    QList<FoundDisc> out;
    QFileInfo fi(path);

    if (isDiscImage(path)) {
        out.append({ fi.absoluteFilePath(), fi.completeBaseName(), ".", fi.absolutePath() });
        return out;
    }
    if (!fi.isDir())
        return out;
    QDir dir(fi.absoluteFilePath());
    if (isDiscFolder(dir)) {
        out.append({ dir.absolutePath(), folderName(dir), ".", dir.absolutePath() });
        return out;
    }
    search(dir, dir, 0, maxDepth, out);
    return out;
}
