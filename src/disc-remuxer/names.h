/*
 * disc-remuxer: a title's output name from the file-name template
 * (setting output.file_name_template; default
 * {NAME1}{-:CMNT1}{-:DT}{title:+DFLT}{_t:N2} = <title name>_t<NN>).
 *
 * Fields: {VAR}, {prefix:VAR}, {prefix:VAR:default}; the prefix is written
 * only when the variable has a value and something was written before it
 * (or the value is DFLT). Variables: NAME, NAME0..NAME2 and CMNT,
 * CMNT0..CMNT2 (name / comment cleaned for file names by rule 0, 1, 2),
 * DY DM DD TH TM TS DT (the date YYYY-MM-DD hh:mm:ss; DT = YYYYMMDDhhmmss),
 * N / AN (title number), M / AM (number + 1), T / AT (the number, not for
 * title 0), DFLT (a value only at the start). +VAR / -VAR test whether a
 * variable has a value; a number takes a width (N2 = two digits). Without a
 * date N, M and T stand for the number; with one only the A forms do. An
 * empty result is "title"; an invalid template gives what it produced
 * before the error plus "!ERRtemplate_t<NN>".
 */

#ifndef DISC_REMUXER_NAMES_H
#define DISC_REMUXER_NAMES_H

/* name, comment, date may be NULL; out gets the name (no extension). */
void title_name(const char *tmpl, const char *name, const char *comment, const char *date, unsigned index,
                char *out, int size);

/* s cleaned for file names into out: rule 0: / \ * ; ? and control
 * characters -> _, | -> I, : -> -, " -> '; rule 1 also # $ -> _, middle dot
 * -> -, typographic quotes and primes -> '; rule 2 also . and space -> _.
 * Repeated _ become one, trailing _ are removed. */
void clean_name(const char *s, int rule, char *out, int size);

#endif
