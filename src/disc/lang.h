/*
 * The disc layer: language codes (ISO 639).
 */

#ifndef DISC_DISC_LANG_H
#define DISC_DISC_LANG_H

/**
 * The ISO 639-2 bibliographic code of a language code written on a disc:
 * a 2-letter ISO 639-1 code, a 3-letter ISO 639-2 code (bibliographic or
 * terminological) or one of "jp" (Japanese), "iw" (Hebrew) and "ptb"
 * (Brazilian Portuguese, its own code). Matched as written, then in lower
 * case; only 2 or 3 characters, ASCII only. The withdrawn ISO 639-1 codes
 * "in", "ji", "jw", "mo" and "sh" are not known.
 * @return the 3-letter code, NULL when the code is not known
 */
const char *dr_disc_lang_code(const char *code);

/**
 * The English name of a language code as dr_disc_lang_code() reads it
 * ("eng" -> "English"), from the ISO 639-2 code list; NULL when not known.
 */
const char *dr_disc_lang_name(const char *code);

/**
 * The ISO 639-2 bibliographic code of a language named in English, as the
 * ISO 639-2 code list names it, in any case: one of its names ("Spanish"
 * or "Castilian" for "Spanish; Castilian"), or such a name up to its first
 * ", " or " (" when only one language's names shorten to it ("Greek" is
 * none: Ancient and Modern Greek both do); NULL when not known.
 */
const char *dr_disc_lang_of_name(const char *name);

#endif /* DISC_DISC_LANG_H */
