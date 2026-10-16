// Finding discs in what the user adds: an image, a disc folder, or a folder
// holding any number of them (searched a few levels deep) — the engine's
// finder (discs.h), the one find and demux of a folder use.
#pragma once

#include <QList>
#include <QString>

struct FoundDisc {
    QString path;          // what the engine opens: the image, or the disc's folder
    QString name;          // the disc's name: the image's or folder's (a VIDEO_TS / BDMV folder: its parent's)
    QString relative;      // where it lies below the added folder ("." : the added path itself)
    QString dropRoot;      // the added folder
};

// The discs at or below path, in name order. An image: .iso / .img; a disc
// folder: one holding VIDEO_TS, HVDVD_TS / HDDVD_TS or BDMV, or a VIDEO_TS /
// BDMV folder itself.
QList<FoundDisc> findDiscs(const QString &path, int maxDepth = 5);
bool isDiscImage(const QString &path);
