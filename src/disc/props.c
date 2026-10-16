/*
 * The disc model's properties (info.h): each key's words, group and kind.
 */

#include <stddef.h>

#include "disc/info.h"

static const struct {
    const char   *name;
    DiscPropGroup group;
    const char   *label;               /* within its group (NULL: the name) */
    int           language;
} props[DISC_PROP_NB] = {
    [DISC_PROP_KIND] = { "Kind", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_MARKET_LANGUAGE] = { "Market language", DISC_PROP_GENERAL, NULL, 1 },
    [DISC_PROP_REGION] = { "Region", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_IFO_CHECK] = { "IFO and navigation check", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_MENU_NAMES] = { "Menu names", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_FAKE_PLAYLISTS] = { "Fake playlists", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_NAME_LANGUAGE] = { "Name's language", DISC_PROP_GENERAL, NULL, 1 },
    [DISC_PROP_ENGLISH_NAME] = { "English name", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_CONTENT_HASH_CHECK] = { "Content hash check", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_PLAYLISTS] = { "Playlists", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_CLIPS] = { "Clips", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_BDJ_ARCHIVES] = { "BD-J archives", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_CSS] = { "Protection: CSS", DISC_PROP_PROTECTION, "CSS", 0 },
    [DISC_PROP_AACS] = { "Protection: AACS", DISC_PROP_PROTECTION, "AACS", 0 },
    [DISC_PROP_AACS2] = { "Protection: AACS 2.0", DISC_PROP_PROTECTION, "AACS 2.0", 0 },
    [DISC_PROP_AACS_DISC_ID] = { "Protection: AACS disc ID", DISC_PROP_PROTECTION, "AACS disc ID", 0 },
    [DISC_PROP_BDPLUS] = { "Protection: BD+", DISC_PROP_PROTECTION, "BD+", 0 },
    [DISC_PROP_BUS_ENCRYPTION] = { "Protection: bus encryption", DISC_PROP_PROTECTION, "bus encryption", 0 },
    [DISC_PROP_TITLE_ON_DISC] = { "Title on the disc", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_TITLE_SET] = { "Title set", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_PROGRAM_CHAIN] = { "Program chain", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_NAVIGATION] = { "Navigation", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_ANGLES] = { "Angles", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_CELLS] = { "Cells", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_LENGTH_MEASURED] = { "Length measured", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_OTHER_NAME] = { "Other name", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_RECORDED] = { "Recorded", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_SIDE_CLIPS] = { "Side clips (text subtitles)", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_FAKE_PLAYLIST_CHECK] = { "Fake playlist check", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_FROM] = { "From", DISC_PROP_GENERAL, NULL, 0 },
    [DISC_PROP_TRACKS_FROM] = { "Tracks described from", DISC_PROP_GENERAL, NULL, 0 },
};

const char *disc_prop_name(DiscPropKey key)
{
    return (unsigned)key < DISC_PROP_NB ? props[key].name : "?";
}

DiscPropGroup disc_prop_group(DiscPropKey key)
{
    return (unsigned)key < DISC_PROP_NB ? props[key].group : DISC_PROP_GENERAL;
}

const char *disc_prop_label(DiscPropKey key)
{
    if ((unsigned)key >= DISC_PROP_NB)
        return "?";
    return props[key].label ? props[key].label : props[key].name;
}

int disc_prop_language(DiscPropKey key)
{
    return (unsigned)key < DISC_PROP_NB && props[key].language;
}

const DiscPropertyInfo *disc_prop_find(const DiscPropertyInfo *p, int n, DiscPropKey key)
{
    for (int i = 0; i < n; i++)
        if (p[i].key == key)
            return &p[i];
    return NULL;
}
