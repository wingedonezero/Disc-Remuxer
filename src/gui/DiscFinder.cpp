#include "DiscFinder.h"

#include <QByteArray>

extern "C" {
#include "discs.h"
}

bool isDiscImage(const QString &path)
{
    return engine_is_disc_image(path.toUtf8().constData());
}

QList<FoundDisc> findDiscs(const QString &path, int maxDepth)
{
    QList<FoundDisc> out;
    EngineFoundList l = {};
    QByteArray p = path.toUtf8();

    if (engine_find_discs(p.constData(), maxDepth, &l) >= 0)
        for (int i = 0; i < l.nb; i++)
            out.append({ QString::fromUtf8(l.discs[i].path), QString::fromUtf8(l.discs[i].name),
                         QString::fromUtf8(l.discs[i].relative), QString::fromUtf8(l.discs[i].root) });
    engine_found_free(&l);
    return out;
}
