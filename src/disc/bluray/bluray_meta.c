/*
 * Blu-ray: disc names (disc library BDMV/META/DL/bdmt_LLL.xml, BDAV
 * info.bdav, AVCHD card files) and recorded text
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <ctype.h>
#include <string.h>

#include <expat.h>

#include "base/mem.h"
#include "base/str.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "bluray_internal.h"

/* ---- recorded text ---- */

/* Windows-1252 for 0x80 .. 0x9f (the bytes without a character map to
 * themselves) */
static const uint16_t cp1252[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178,
};

/* UTF-16 code units (to the first 0) as UTF-8; an unpaired high surrogate
 * is written as itself, a high surrogate as the last unit ends the text. */
static char *utf16_to_utf8(const uint16_t *u, uint32_t n)
{
    DrStr bp;

    dr_str_init(&bp);
    for (uint32_t i = 0; i < n && u[i]; i++) {
        uint32_t c = u[i];
        uint8_t b[4];
        int len;

        if (c >= 0xd800 && c <= 0xdbff) {
            if (i + 1 >= n || !u[i + 1])
                break;
            if (u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff)
                c = 0x10000 + ((c - 0xd800) << 10) + (u[++i] - 0xdc00);
        }
        if (c < 0x80) {
            b[0] = c;
            len  = 1;
        } else if (c < 0x800) {
            b[0] = 0xc0 | c >> 6;
            b[1] = 0x80 | (c & 0x3f);
            len  = 2;
        } else if (c < 0x10000) {
            b[0] = 0xe0 | c >> 12;
            b[1] = 0x80 | (c >> 6 & 0x3f);
            b[2] = 0x80 | (c & 0x3f);
            len  = 3;
        } else {
            b[0] = 0xf0 | c >> 18;
            b[1] = 0x80 | (c >> 12 & 0x3f);
            b[2] = 0x80 | (c >> 6 & 0x3f);
            b[3] = 0x80 | (c & 0x3f);
            len  = 4;
        }
        dr_str_cat_data(&bp, (const char *)b, len);
    }
    return dr_str_take(&bp, 1);
}

/* Bytes valid as UTF-8 for the reference program's check: no 0 byte, every
 * sequence complete (lead byte lengths up to 6 bytes). */
static int valid_utf8(const uint8_t *p, uint32_t len)
{
    const uint8_t *end = p + len;

    while (p < end) {
        int t = *p >= 0xfc && *p <= 0xfd ? 5 : *p >= 0xf8 && *p <= 0xfb ? 4 : *p >= 0xf0 && *p <= 0xf7 ? 3 :
                *p >= 0xe0 && *p <= 0xef ? 2 : *p >= 0xc0 && *p <= 0xdf ? 1 : 0;
        if (!*p || p + t + 1 > end)
            return 0;
        if (!t && *p >= 0x80)
            return 0;
        for (int i = 1; i <= t; i++)
            if ((p[i] & 0xc0) != 0x80)
                return 0;
        p += t + 1;
    }
    return 1;
}

char *ff_bluray_text(void *log, uint8_t charset, const uint8_t *text, uint32_t len)
{
    uint16_t *u;
    char *s;

    if (!len)
        return NULL;
    switch (charset) {
    case 0x10:                              /* as recorded */
        return dr_strndup((const char *)text, len);
    case 0x11:                              /* single bytes: Latin-1 with Windows-1252 at 0x80 .. 0x9f */
        if (!(u = dr_malloc_array(len, sizeof(*u))))
            return NULL;
        for (uint32_t i = 0; i < len; i++)
            u[i] = text[i] >= 0x80 && text[i] <= 0x9f ? cp1252[text[i] - 0x80] : text[i];
        s = utf16_to_utf8(u, len);
        dr_free(u);
        return s;
    case 0x20:                              /* UTF-8, else UTF-16 big-endian */
        if (valid_utf8(text, len))
            return dr_strndup((const char *)text, len);
        if (len & 1) {
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: recorded text of %u bytes is neither UTF-8 nor UTF-16\n", len);
            return NULL;
        }
        if (!(u = dr_malloc_array(len / 2, sizeof(*u))))
            return NULL;
        for (uint32_t i = 0; i < len / 2; i++)
            u[i] = text[2 * i] << 8 | text[2 * i + 1];
        s = utf16_to_utf8(u, len / 2);
        dr_free(u);
        return s;
    case 0x01:
        /* ARIB STD-B24 8-bit character code (Japanese recorders) */
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: recorded text in the ARIB character code is not decoded "
               "[untested on real discs]\n");
        return NULL;
    default:
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: recorded text in character set 0x%02x is not decoded\n", charset);
        return NULL;
    }
}

/* ---- the disc library title ---- */

static int ends(const char *s, size_t L, const char *suffix)
{
    size_t n = strlen(suffix);
    return L >= n && !memcmp(s + L - n, suffix, n);
}

char *ff_bluray_clean_title(const char *s)
{
    size_t L = strlen(s), n;
    char *b;

    if (!L)
        return NULL;
    for (;;) {
        size_t before = L;

        if (L >= 3) {
            if (ends(s, L, "\xe2\x84\xa2"))                      L -= 3;
            else if (L >= 11 && ends(s, L, " Blu-ray 3D"))       L -= 11;
            else if (L == 11 && ends(s, L, " 3D Blu-ray"))       L -= 11;
            else if (L >= 12 && ends(s, L, " 3-D Blu-ray"))      L -= 12;
            else if (L >= 11 && ends(s, L, " 3D Blu-ray"))       L -= 11;
            else if (L >= 12 && ends(s, L, " 4K Ultra HD"))      L -= 12;
            else if (L >= 9 && ends(s, L, " Ultra HD"))          L -= 9;
            else if (ends(s, L, " - "))                          L -= 3;
            else if (L >= 5 && ends(s, L, " \xe2\x80\x93 "))     L -= 5;
        }
        if (L == before && L >= 2) {
            if (ends(s, L, " -"))                                L -= 2;
            else if (L >= 4 && ends(s, L, " \xe2\x80\x93"))      L -= 4;
            else if (L >= 7 && ends(s, L, "Blu-ray"))            L -= 7;
            else if (L >= 5 && ends(s, L, ", The"))              L -= 5;
        }
        if (L == before && L && s[L - 1] == ' ')
            L -= 1;
        if (L == before)
            break;
        if (!L)
            return NULL;
    }
    if (!(b = dr_strndup(s, L)))
        return NULL;
    n = L;
    while (n >= 3 && (uint8_t)b[n - 1] <= 0x20)
        n--;
    if (n < 3)
        n = DR_MIN(L, 2);
    for (size_t i = 0; i < n;) {
        if (b[i] == '\r') {
            memmove(b + i, b + i + 1, n - i);
            n--;
            i = 1;
            continue;
        }
        if (b[i] == '\n')
            b[i] = ' ';
        i++;
    }
    b[n] = 0;
    if (!strcmp(b, "Blu-ray"))
        dr_freep(&b);
    return b;
}

/* ---- the disc library XML ---- */

typedef struct Lib {
    int      depth;
    int      in_disclib, discinfo, title, name, language;  /* element counts seen */
    int      in_name, in_lang;                              /* collecting text */
    DrStr name_text, lang_text;
} Lib;

/* The element name without its namespace prefix. */
static const char *local(const char *name)
{
    const char *c = strrchr(name, ':');
    return c ? c + 1 : name;
}

static void XMLCALL lib_start(void *opaque, const XML_Char *el, const XML_Char **attr)
{
    Lib *l = opaque;
    const char *n = local(el);

    l->depth++;
    if (l->depth == 1) {
        l->in_disclib = !strcmp(n, "disclib");
    } else if (l->depth == 2 && l->in_disclib && !strcmp(n, "discinfo")) {
        l->discinfo++;
    } else if (l->depth == 3 && l->discinfo == 1) {
        if (!strcmp(n, "title"))
            l->title++;
        else if (!strcmp(n, "language") && !l->language++)
            l->in_lang = 1;
    } else if (l->depth == 4 && l->discinfo == 1 && l->title == 1 && !strcmp(n, "name") && !l->name++) {
        l->in_name = 1;
    }
}

static void XMLCALL lib_end(void *opaque, const XML_Char *el)
{
    Lib *l = opaque;

    if (l->depth == 4)
        l->in_name = 0;
    if (l->depth == 3)
        l->in_lang = 0;
    l->depth--;
}

static void XMLCALL lib_text(void *opaque, const XML_Char *s, int len)
{
    Lib *l = opaque;

    if (l->in_name)
        dr_str_cat_data(&l->name_text, s, len);
    else if (l->in_lang)
        dr_str_cat_data(&l->lang_text, s, len);
}

static int xml_parse(Lib *l, const char *buf, size_t len)
{
    XML_Parser p = XML_ParserCreate(NULL);
    int ok;

    if (!p)
        return DR_ERR(ENOMEM);
    memset(l, 0, sizeof(*l));
    dr_str_init(&l->name_text);
    dr_str_init(&l->lang_text);
    XML_SetUserData(p, l);
    XML_SetElementHandler(p, lib_start, lib_end);
    XML_SetCharacterDataHandler(p, lib_text);
    ok = XML_Parse(p, buf, len, 1) == XML_STATUS_OK;
    XML_ParserFree(p);
    if (!ok) {
        dr_str_free(&l->name_text);
        dr_str_free(&l->lang_text);
    }
    return ok;
}

/* The repairs the reference program tries when a disc library file does
 * not parse: href=value -> href="value" (at most 8), then the first
 * <di:description> element blanked. */
static int xml_parse_tolerant(void *log, const BlurayFile *f, Lib *l)
{
    char *b, *h, *d, *e;
    int ret, fixes = 0;
    size_t L = f->size;

    if ((ret = xml_parse(l, (const char *)f->data, f->size)))
        return ret;
    if (L > 0x40000 || !(b = dr_mallocz(L + 0x81)))
        return 0;
    memcpy(b, f->data, L);
    for (h = b;;) {
        char *p = strstr(h, " href="), *q, *v;
        if (!p || (uint8_t)p[6] >= 0x80)
            break;
        if (p[6] == '\'' || p[6] == '"') {
            h = p + 7;
            continue;
        }
        if (!isalnum((uint8_t)p[6]) || !(e = strchr(p + 6, '>')))
            break;
        for (v = p + 6; v < e; v++) {
            if ((uint8_t)*v >= 0x80)
                continue;
            if (strchr("\"'/<>\\", *v))
                goto repaired;
            if (isspace((uint8_t)*v)) {
                e = v;
                break;
            }
        }
        memmove(e + 2, e, b + L + 0x80 - (e + 2));
        memmove(p + 7, p + 6, e - (p + 6));
        p[6] = '"';
        e[1] = '"';
        q = e;
        h = q;
        if (++fixes >= 8)
            break;
    }
repaired:
    if (fixes) {
        b[L + 0x80] = 0;
        if ((ret = xml_parse(l, b, strlen(b)))) {
            dr_free(b);
            return ret;
        }
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s does not parse with its href values quoted\n", f->path);
    }
    memcpy(b, f->data, L);
    b[L] = 0;
    if ((d = strstr(b, "<di:description>")) && (e = strstr(d + 1, "</di:description>"))) {
        memset(d, ' ', e + 17 - d);
        ret = xml_parse(l, b, L);
        if (!ret)
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s does not parse without its description\n", f->path);
    } else {
        ret = 0;
    }
    dr_free(b);
    return ret;
}

int ff_bluray_meta_read(void *log, const BlurayFile *f, char **name, char lang[8])
{
    Lib l;
    int ret;

    *name   = NULL;
    lang[0] = 0;
    if ((ret = xml_parse_tolerant(log, f, &l)) <= 0) {
        if (!ret)
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s is not readable XML\n", f->path);
        return ret;
    }
    if (l.language && dr_str_ok(&l.lang_text))
        dr_strlcpy(lang, l.lang_text.str, 8);
    if (l.name && dr_str_ok(&l.name_text))
        *name = ff_bluray_clean_title(l.name_text.str);
    dr_str_free(&l.name_text);
    dr_str_free(&l.lang_text);
    return 1;
}
