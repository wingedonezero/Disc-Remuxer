/*
 * Blu-ray: a disc's structure (BDMV, BDAV, AVCHD)
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

/*
 * The steps of the reference program's disc open, in its order:
 * 1. BD+ (/BDSVM/00000.svm) and BDAV (/BDAV/info.bdav: a recordable disc);
 * 2. the disc name: BDAV: from info.bdav; BDMV: from the disc library
 *    (BDMV/META/DL/bdmt_LLL.xml: the preferred language's file, else the
 *    first; the English name kept as well), else, for an AVCHD folder, from
 *    the card it is on (MISC/AUTPRINT.MRK, or .SonyVID);
 * 3. the numbered files: clip information (.clpi, else the 8.3 .CPI names,
 *    sorted), playlists (.mpls, else .MPL, sorted; BDAV: .rpls), JARs, BD-J
 *    objects; a disc without clip information cannot be read;
 * 4. each clip's information and its stream file (clips that fail are left
 *    out);
 * 5. each playlist (playlists that fail are left out);
 * 6. index.bdmv and CERTIFICATE/id.bdmv (BDMV).
 * Then the format part runs the AACS step (bluray_aacs.c). Still to come
 * with their steps: BD+, the individual segment table, the JAR date and the
 * BD-J analysis.
 */

#include <stdio.h>
#include <string.h>

#include "base/mem.h"
#include "base/str.h"
#include "base/bytes.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "bluray_internal.h"
#include "disc/lang.h"

static int has_file(DiscIOFS *fs, const char *path)
{
    DiscIOFile *f;

    if (fs->ops->open_file(fs, path, &f) < 0)
        return 0;
    ff_discio_file_free(&f);
    return 1;
}

/* A file read whole, without a BACKUP copy: 1, 0 missing, < 0 error. */
static int load_direct(void *log, DiscIOFS *fs, const char *path, BlurayFile *out)
{
    DiscIOFile *file;
    int ret;

    memset(out, 0, sizeof(*out));
    if ((ret = fs->ops->open_file(fs, path, &file)) < 0)
        return ret == DR_ERR(ENOENT) ? 0 : ret;
    if (!(out->data = dr_malloc(file->size + 1))) {
        ff_discio_file_free(&file);
        return DR_ERR(ENOMEM);
    }
    out->size = file->size;
    ret = ff_discio_file_read(fs, file, 0, out->data, file->size);
    ff_discio_file_free(&file);
    if (ret < 0) {
        ff_bluray_file_free(out);
        return ret;
    }
    out->data[out->size] = 0;
    snprintf(out->path, sizeof(out->path), "%s", path);
    return 1;
}

/* ---- the disc name ---- */

static void bdav_name(void *log, BlurayDisc *d)
{
    BlurayFile f;
    char *t;

    if (load_direct(log, d->fs, "/BDAV/info.bdav", &f) <= 0)
        return;
    if (f.size >= 0x140 && !memcmp(f.data, "BDAV", 4) && !memcmp(f.data + 0x28, "\x00\x00\x01\x14", 4) &&
        (t = ff_bluray_text(log, f.data[0x2e], f.data + 0x41, f.data[0x40])))
        d->name = t;
    ff_bluray_file_free(&f);
}

typedef struct MetaList {
    char **names;
    int    nb;
    int    err;
} MetaList;

static int meta_entry(void *opaque, const char *name, int is_dir)
{
    MetaList *m = opaque;
    char *n;

    if (is_dir || strlen(name) < 12 ||
        (memcmp(name, "BDMT_", 5) && memcmp(name, "bdmt_", 5)) ||
        (memcmp(name + 8, ".XML", 4) && memcmp(name + 8, ".xml", 4)))
        return 0;
    if (!(n = dr_strdup(name)) || dr_array_add_ptr(&m->names, &m->nb, n) < 0) {
        dr_free(n);
        return m->err = DR_ERR(ENOMEM);
    }
    return 0;
}

/* The language letters of a disc library file name ("bdmt_eng.xml"). */
static void name_lang(const char *name, char out[4])
{
    memcpy(out, name + 5, 3);
    out[3] = 0;
}

/* Two language codes name the same language. */
static int same_lang(const char *a, const char *b)
{
    const char *x, *y;

    if (strlen(a) < 2 || strlen(b) < 2)
        return strlen(a) < 2 && strlen(b) < 2;
    if (!strcmp(a, b))
        return 1;
    x = dr_disc_lang_code(a);
    y = dr_disc_lang_code(b);
    return x && y && !strcmp(x, y);
}

/* A disc library file: 2 parsed (*name / lang from it), 1 opened but not
 * parsed, 0 not opened, < 0 error. */
static int parse_meta(void *log, BlurayDisc *d, const char *file, char **name, char lang[8])
{
    char path[64];
    BlurayFile f;
    int ret;

    *name   = NULL;
    lang[0] = 0;
    snprintf(path, sizeof(path), "/BDMV/META/DL/%s", file);
    if ((ret = load_direct(log, d->fs, path, &f)) <= 0)
        return ret;
    ret = ff_bluray_meta_read(log, &f, name, lang);
    ff_bluray_file_free(&f);
    return ret < 0 ? ret : 1 + ret;
}

/* AVCHD: the name the card's files give. */
static void card_name(void *log, BlurayDisc *d)
{
    BlurayFile f;

    if (!d->parent_fs)
        return;
    if (load_direct(log, d->parent_fs, "/MISC/AUTPRINT.MRK", &f) > 0) {
        char *p = (char *)f.data, *end = p + f.size;
        while (p < end) {
            char *line = p, *nl = memchr(p, '\n', end - p), *e = nl ? nl : end, *v, *q;
            p = nl ? nl + 1 : end;
            while (line < e && (*line == '\r' || *line == '\n'))
                line++;
            if (e - line < 9 || memcmp(line, "GEN CRT=\"", 9))
                continue;
            v = line + 9;
            if ((q = memchr(v, '"', e - v)) && q - v <= 255) {
                d->name = dr_strndup(v, q - v);
                break;
            }
        }
        ff_bluray_file_free(&f);
    } else if (has_file(d->parent_fs, "/.SonyVID")) {
        d->name = dr_strdup("SONY_AVCHD");
    }
}

static int disc_name(void *log, BlurayDisc *d, const char *pref)
{
    MetaList m = { 0 };
    const char *eng = NULL, *chosen = NULL;
    char *name = NULL, lang[8], l[4];
    int ret, opened = 0;

    if (d->bdav) {
        bdav_name(log, d);
        return 0;
    }
    if (!pref || strlen(pref) < 2)
        pref = "eng";
    ret = d->fs->ops->list_dir(d->fs, "/BDMV/META/DL", meta_entry, &m);
    if (m.err < 0) {
        ret = m.err;
        goto end;
    }
    if (ret < 0 && ret != DR_ERR(ENOENT)) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: /BDMV/META/DL cannot be listed\n");
        ret = 0;
        goto end;
    }
    ret = 0;
    for (int i = 0; i < m.nb; i++) {
        name_lang(m.names[i], l);
        if (d->nb_meta_lang < (int)DR_ARRAY_ELEMS(d->meta_lang))
            dr_strlcpy(d->meta_lang[d->nb_meta_lang++], l, sizeof(d->meta_lang[0]));
        if (!eng && strlen(l) >= 2 && same_lang(l, "eng"))
            eng = m.names[i];
        if (!chosen && same_lang(pref, l))
            chosen = m.names[i];
    }
    if (m.nb && !chosen)
        chosen = m.names[0];
    if (chosen) {
        ret = parse_meta(log, d, chosen, &name, lang);
        if (ret < 0)
            goto end;
        opened = ret > 0;
        if (ret == 2) {
            const char *code;
            if (!lang[0])
                name_lang(chosen, lang);
            if (strlen(lang) <= 5 && (code = dr_disc_lang_code(lang)))
                dr_strlcpy(d->name_lang, code, sizeof(d->name_lang));
            d->name = name;
        }
        if (d->name && chosen == eng) {
            if (!(d->name_eng = dr_strdup(d->name))) {
                ret = DR_ERR(ENOMEM);
                goto end;
            }
        } else if (eng && (ret = parse_meta(log, d, eng, &d->name_eng, lang)) < 0) {
            goto end;
        }
    }
    ret = 0;
    if (!d->name && !opened)
        card_name(log, d);
end:
    for (int i = 0; i < m.nb; i++)
        dr_free(m.names[i]);
    dr_free(m.names);
    return ret;
}

/* ---- AVCHD: the card's root ---- */

/* The card root of an AVCHD folder ("<card>/PRIVATE/AVCHD"), mounted for
 * its name files. */
static void mount_card(void *log, BlurayDisc *d, const char *path)
{
    static const char *const suffixes[] = { "/PRIVATE/AVCHDL", "/PRIVATE/AVCHD", "/AVCHDL", "/AVCHD" };
    size_t n = strlen(path), L, cut = 0;
    char *root;

    if (n < 3)
        return;
    L = path[n - 1] == '/' ? n - 1 : n;
    for (int i = 0; i < 4 && !cut; i++) {
        size_t s = strlen(suffixes[i]);
        if (L >= s + 1 && !memcmp(path + L - s, suffixes[i], s))
            cut = L - s;
    }
    if (!cut || !(root = dr_strndup(path, cut)))
        return;
    if (ff_discio_folder_is_dir(root) && ff_discio_mount_folder(log, root, d->fs->src->attempts, &d->parent_fs) < 0)
        d->parent_fs = NULL;
    dr_free(root);
}

/* ---- the open ---- */

static int list(void *log, DiscIOFS *fs, const char *dir, const char *ext, const char *ext83,
                uint32_t **v, int *n)
{
    int ret = ff_bluray_list_numbered(log, fs, dir, ext, v, n);

    if (ret >= 0 && !*n && ext83) {
        dr_freep(v);
        if ((ret = ff_bluray_list_numbered(log, fs, dir, ext83, v, n)) >= 0)
            ff_bluray_sort_numbers(*v, *n);
    }
    return ret;
}

int ff_bluray_disc_open(void *log, DiscIOFS *fs, const char *path, const char *lang, BlurayDisc *d)
{
    const char *root;
    uint32_t *ids = NULL;
    int n = 0, ret;

    memset(d, 0, sizeof(*d));
    d->fs     = fs;
    d->bdplus = has_file(fs, "/BDSVM/00000.svm");
    d->bdav   = has_file(fs, "/BDAV/info.bdav");
    root      = d->bdav ? "BDAV" : "BDMV";
    if (path && ff_discio_folder_is_dir(path))
        mount_card(log, d, path);
    dr_vlog(log, DR_VLOG_VERBOSE, "Blu-ray: a %s disc%s\n", d->bdav ? "BDAV (recordable)" : "BDMV",
           d->bdplus ? " with BD+" : "");
    /* BD+ repairs parts of the stream files that the disc carries damaged on
     * purpose; a decrypted copy may already be repaired and still keep the
     * BD+ files, which cannot be told from the files themselves. */
    if (d->bdplus)
        dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: the disc uses BD+ (/BDSVM/00000.svm), which is not supported: "
               "unless this copy was made with BD+ removed, parts of the video may be damaged\n");

    if ((ret = disc_name(log, d, lang)) < 0)
        return ret;

    /* clip information and stream files */
    if ((ret = list(log, fs, d->bdav ? "/BDAV/CLIPINF" : "/BDMV/CLIPINF", ".clpi", d->bdav ? NULL : ".CPI",
                    &ids, &n)) < 0)
        return ret;
    if (!n) {
        dr_free(ids);
        dr_vlog(log, DR_VLOG_ERROR, "Blu-ray: the disc has no clip information files\n");
        return DR_ERR_INVALID;
    }
    if (!(d->clips = dr_calloc(n, sizeof(*d->clips)))) {
        dr_free(ids);
        return DR_ERR(ENOMEM);
    }
    for (int i = 0; i < n; i++) {
        static const char *const exts[] = { "clpi", "CPI", NULL };
        BlurayClip *c = &d->clips[d->nb_clips];
        char num[8];
        const char *names[] = { num, NULL };
        BlurayFile f;

        snprintf(num, sizeof(num), "%05u", ids[i]);
        memset(c, 0, sizeof(*c));
        c->number = ids[i];
        if ((ret = ff_bluray_load(log, fs, root, "/CLIPINF", names, exts, &f)) < 0)
            break;
        ret = ret ? ff_bluray_clpi_parse(log, &f, c) : 0;
        ff_bluray_file_free(&f);
        if (ret < 0)
            break;
        if (ret && ff_bluray_clip_stream_file(log, fs, c)) {
            d->nb_clips++;
        } else {
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: clip %05u %s: left out\n", ids[i],
                   ret ? "has no stream file" : "information cannot be read");
            ff_bluray_clip_free(c);
        }
        ret = 0;
    }
    dr_freep(&ids);
    if (ret < 0)
        return ret;

    /* playlists */
    if ((ret = d->bdav ? list(log, fs, "/BDAV/PLAYLIST", ".rpls", NULL, &ids, &n)
                       : list(log, fs, "/BDMV/PLAYLIST", ".mpls", ".MPL", &ids, &n)) < 0)
        return ret;
    if (n && !(d->playlists = dr_calloc(n, sizeof(*d->playlists)))) {
        dr_free(ids);
        return DR_ERR(ENOMEM);
    }
    for (int i = 0; i < n; i++) {
        static const char *const mpls[] = { "mpls", "MPL", NULL }, *const rpls[] = { "rpls", NULL };
        BlurayPlaylist *pl = &d->playlists[d->nb_playlists];
        char num[8];
        const char *names[] = { num, NULL };
        BlurayFile f;

        snprintf(num, sizeof(num), "%05u", ids[i]);
        memset(pl, 0, sizeof(*pl));
        pl->number = ids[i];
        if ((ret = ff_bluray_load(log, fs, root, "/PLAYLIST", names, d->bdav ? rpls : mpls, &f)) < 0)
            break;
        ret = !ret ? 0 : d->bdav ? ff_bluray_rpls_parse(log, &f, pl) : ff_bluray_mpls_parse(log, &f, pl);
        ff_bluray_file_free(&f);
        if (ret < 0)
            break;
        if (ret) {
            d->nb_playlists++;
        } else {
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: playlist %05u cannot be read: left out\n", ids[i]);
            ff_bluray_playlist_free(pl);
        }
        ret = 0;
    }
    d->playlist_ids    = ids;      /* the listing, for the AACS step */
    d->nb_playlist_ids = n;
    ids = NULL;
    if (ret < 0)
        return ret;
    if (d->bdav)
        return 0;

    /* JARs and BD-J objects (read with the BD-J step) */
    if ((ret = list(log, fs, "/BDMV/JAR", ".jar", NULL, &d->jars, &d->nb_jars)) < 0 ||
        (ret = list(log, fs, "/BDMV/BDJO", ".bdjo", NULL, &d->bdjo, &d->nb_bdjo)) < 0)
        return ret;

    /* index, disc id */
    {
        static const char *const exts[] = { "bdmv", "BDM", NULL };
        static const char *const index_names[] = { "index", "INDEX", NULL };
        static const char *const id_names[] = { "id", NULL };
        BlurayFile f;

        if ((ret = ff_bluray_load(log, fs, "BDMV", "", index_names, exts, &f)) < 0)
            return ret;
        if (ret && (ret = ff_bluray_index_parse(log, &f, &d->index)) < 0) {
            ff_bluray_file_free(&f);
            return ret;
        }
        d->has_index = ret;
        if (!ret)
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: index.bdmv cannot be read\n");
        ff_bluray_file_free(&f);
        if (!fs->ops->find_dir(fs, "/CERTIFICATE")) {
            if ((ret = ff_bluray_load(log, fs, "CERTIFICATE", "", id_names, exts, &f)) < 0)
                return ret;
            d->has_id = ret && ff_bluray_id_parse(log, &f, &d->id) > 0;
            ff_bluray_file_free(&f);
        }
    }
    return 0;
}

int ff_bluray_has_bdj(const BlurayDisc *d)
{
    int n;

    if (!d->has_index)
        return 0;
    n = d->index.first_play.object_type == 2 || d->index.top_menu.object_type == 2;
    for (int i = 0; i < d->index.nb_titles; i++)
        n |= d->index.titles[i].object_type == 2;
    return n;
}

void ff_bluray_disc_free(BlurayDisc *d)
{
    ff_bluray_titles_free(d);
    for (int i = 0; i < d->nb_clips; i++)
        ff_bluray_clip_free(&d->clips[i]);
    dr_freep(&d->clips);
    for (int i = 0; i < d->nb_playlists; i++)
        ff_bluray_playlist_free(&d->playlists[i]);
    dr_freep(&d->playlists);
    dr_freep(&d->playlist_ids);
    dr_freep(&d->bdjo);
    dr_freep(&d->jars);
    dr_freep(&d->pl_check);
    dr_freep(&d->pl_repeats);
    ff_bluray_menu_free(d);
    ff_bluray_index_free(&d->index);
    dr_freep(&d->name);
    dr_freep(&d->name_eng);
    ff_discio_fs_close(&d->parent_fs);
    d->nb_clips = d->nb_playlists = d->nb_playlist_ids = d->nb_bdjo = d->nb_jars = 0;
}

/* ---- the structure as text ---- */

static void stn(DrStr *bp, const char *kind, const BlurayStnStream *s)
{
    dr_str_catf(bp, "    %s entry %u len %u [", kind, s->entry_type, s->entry_len);
    for (int i = 0; i < DR_MIN(s->entry_len, 8); i++)
        dr_str_catf(bp, "%s%02x", i ? " " : "", s->entry[i]);
    dr_str_catf(bp, "] coding 0x%02x len %u [", s->coding_type, s->attr_len);
    for (int i = 0; i < DR_MIN(s->attr_len, 32); i++)
        dr_str_catf(bp, "%s%02x", i ? " " : "", s->attr[i]);
    dr_str_catf(bp, "]\n");
}

void ff_bluray_disc_dump(const BlurayDisc *d, DrStr *bp)
{
    dr_str_catf(bp, "disc %s%s name '%s' lang '%s' english '%s' clips %d playlists %d jars %d bdjo %d\n",
               d->bdav ? "BDAV" : "BDMV", d->bdplus ? " BD+" : "", d->name ? d->name : "", d->name_lang,
               d->name_eng ? d->name_eng : "", d->nb_clips, d->nb_playlists, d->nb_jars, d->nb_bdjo);
    if (d->has_index) {
        dr_str_catf(bp, "index version %04x first_play %u/%u top_menu %u/%u titles %d\n", d->index.hdr.version,
                   d->index.first_play.object_type, d->index.first_play.ref, d->index.top_menu.object_type,
                   d->index.top_menu.ref, d->index.nb_titles);
        for (int i = 0; i < d->index.nb_titles; i++)
            dr_str_catf(bp, "  title %d object %u ref %u access %u playback %u\n", i + 1,
                       d->index.titles[i].object_type, d->index.titles[i].ref, d->index.titles[i].access_type,
                       d->index.titles[i].playback_type);
    }
    if (d->has_id) {
        dr_str_catf(bp, "id org %08x disc ", d->id.org_id);
        for (int i = 0; i < 16; i++)
            dr_str_catf(bp, "%02x", d->id.disc_id[i]);
        dr_str_catf(bp, "\n");
    }
    for (int i = 0; i < d->nb_clips; i++) {
        const BlurayClip *c = &d->clips[i];
        char type[5] = { 0 };
        dr_wb32((uint8_t *)type, c->hdr.type);
        dr_str_catf(bp, "clip %05u type %.4s version %04x stream_type %u app_type %u rate %u packets %u "
                   "format '%.4s' cpi %u file %s %"PRId64"\n", c->number, type,
                   c->hdr.version, c->stream_type, c->app_type, c->rate, c->packets, c->format_id, c->cpi_type,
                   c->stream_path, c->stream_size);
        for (int k = 0; k < c->nb_atc; k++)
            for (int j = 0; j < c->atc[k].nb_stc; j++) {
                const BlurayStc *t = &c->atc[k].stc[j];
                dr_str_catf(bp, "  atc %d stc %d pcr_pid 0x%04x spn %u start %u end %u\n", k, j, t->pcr_pid,
                           t->spn_stc_start, t->start, t->end);
            }
        dr_str_catf(bp, "  program spn %u pmt 0x%04x streams %d\n", c->program.spn_start, c->program.pmt_pid,
                   c->program.nb_streams);
        for (int k = 0; k < c->program.nb_streams; k++) {
            const BlurayClipStream *s = &c->program.streams[k];
            dr_str_catf(bp, "    pid 0x%04x coding 0x%02x len %u [", s->pid, s->coding_type, s->info_len);
            for (int b = 0; b < DR_MIN(s->info_len, 32); b++)
                dr_str_catf(bp, "%s%02x", b ? " " : "", s->info[b]);
            dr_str_catf(bp, "]\n");
        }
    }
    for (int i = 0; i < d->nb_playlists; i++) {
        const BlurayPlaylist *pl = &d->playlists[i];
        dr_str_catf(bp, "playlist %05u version %04x playback %u items %d subpaths %d marks %d ext %d ext_subpath %d\n",
                   pl->number, pl->hdr.version, pl->playback_type, pl->nb_items, pl->nb_subpaths, pl->nb_marks,
                   pl->nb_ext, pl->ext_subpath_type);
        for (int k = 0; k < pl->nb_items; k++) {
            const BlurayPlayItem *it = &pl->items[k];
            dr_str_catf(bp, "  item %d clip %.5s codec %.4s in %u out %u connection %u stc %u angles %d "
                       "audio %d pg %d ig %u ss %d\n", k, it->clip, it->codec, it->in_time, it->out_time,
                       it->connection, it->stc_id, it->nb_angles + (it->multi_angle ? 1 : 0), it->nb_audio,
                       it->nb_pg, it->nb_ig, it->ss.present);
            for (int a = 0; a < it->nb_angles; a++)
                dr_str_catf(bp, "    angle %d clip %.5s codec %.4s stc %u\n", a + 2, it->angles[a].clip,
                           it->angles[a].codec, it->angles[a].stc_id);
            if (!pl->bdav) {
                stn(bp, "video", &it->video);
                for (int a = 0; a < it->nb_audio; a++)
                    stn(bp, "audio", &it->audio[a]);
                for (int a = 0; a < it->nb_pg; a++)
                    stn(bp, "pg", &it->pg[a]);
            }
        }
        for (int k = 0; k < pl->nb_subpaths; k++) {
            const BluraySubPath *sp = &pl->subpaths[k];
            dr_str_catf(bp, "  subpath %d type %u items %d\n", k, sp->type, sp->nb_items);
            for (int j = 0; j < sp->nb_items; j++)
                dr_str_catf(bp, "    subitem %d clip %.5s codec %.4s in %u out %u sync_item %u sync_pts %u\n", j,
                           sp->items[j].clip, sp->items[j].codec, sp->items[j].in_time, sp->items[j].out_time,
                           sp->items[j].sync_play_item, sp->items[j].sync_start_pts);
        }
        for (int k = 0; k < pl->nb_marks; k++)
            dr_str_catf(bp, "  mark %d type %u item %u time %u pid 0x%04x duration %u\n", k, pl->marks[k].type,
                       pl->marks[k].play_item, pl->marks[k].time, pl->marks[k].pid, pl->marks[k].duration);
    }
    for (int i = 0; i < d->nb_titles; i++) {
        const BlurayTitle *t = &d->titles[i];
        dr_str_catf(bp, "title %d %s angle %d duration %"PRId64" size %"PRId64" segments ", i, t->name, t->angle,
                   ff_bluray_title_duration(t), ff_bluray_title_size(t));
        for (int k = 0; k < t->nb_segs; k++)
            dr_str_catf(bp, "%s%s%s", k ? "," : "", t->segs[k].desc, t->segs[k].interleaved ? "*" : "");
        dr_str_catf(bp, " marks");
        for (int k = 0; k < t->nb_marks; k++)
            dr_str_catf(bp, "%s%"PRIu64, k ? "," : " ", t->marks[k]);
        dr_str_catf(bp, " pids");
        for (int k = 0; k < t->nb_pids; k++)
            dr_str_catf(bp, "%s0x%04x", k ? "," : " ", t->pids[k]);
        if (t->title_name)
            dr_str_catf(bp, " name '%s'", t->title_name);
        if (t->alt_name)
            dr_str_catf(bp, " alt '%s'", t->alt_name);
        if (t->year)
            dr_str_catf(bp, " recorded %04d-%02d-%02d %02d:%02d:%02d", t->year, t->month, t->day, t->hour, t->minute,
                       t->second);
        for (int k = 0; k < t->nb_side; k++)
            dr_str_catf(bp, " text %s", t->side[k].seg.desc);
        if (ff_bluray_title_parts(t, NULL) > 1) {
            /* each part: its first segment, duration and own marks */
            int *first = dr_calloc(t->nb_segs + 1, sizeof(*first)), n;
            uint64_t *m = dr_calloc(DR_MAX(t->nb_marks, 1), sizeof(*m));
            if (first && m) {
                n = ff_bluray_title_parts(t, first);
                dr_str_catf(bp, " parts");
                for (int p = 0; p < n; p++) {
                    int nm = ff_bluray_segs_marks(t, first[p], first[p + 1], m);
                    dr_str_catf(bp, "%s%d:%"PRId64":", p ? "," : " ", first[p],
                               ff_bluray_segs_duration(t, first[p], first[p + 1]));
                    for (int k = 0; k < nm; k++)
                        dr_str_catf(bp, "%s%"PRIu64, k ? "+" : "", m[k]);
                }
            }
            dr_free(first);
            dr_free(m);
        }
        dr_str_catf(bp, "\n");
        for (int k = 0; k < t->nb_tracks; k++) {
            const BlurayTrack *tk = &t->tracks[k];
            dr_str_catf(bp, "  track %d kind 0x%02x pid 0x%04x sel 0x%x 0x%x flags 0x%x lang '%s' stream %d from %d inner %d",
                       k, tk->code, tk->pid, tk->sel1, tk->sel2, tk->flags, tk->lang, tk->stream, tk->from_track,
                       tk->inner_track);
            if (tk->offset_seq >= 0)
                dr_str_catf(bp, " offset %d", tk->offset_seq);
            if (tk->flags & BLURAY_TRACK_SIDE)
                dr_str_catf(bp, " clip %05u stc %d", tk->side_clip, tk->side_stc);
            dr_str_catf(bp, "\n");
        }
    }
    /* the menu's project data */
    if (d->menu_name) {
        dr_str_catf(bp, "menu '%s' named %d\n", d->menu_name, d->nb_menu_named);
        for (int p = 0; p < d->nb_playlists; p++)
            if (d->pl_menu_name[p])
                dr_str_catf(bp, "menu %05u '%s'\n", d->playlists[p].number, d->pl_menu_name[p]);
    }
    /* the fake playlist check */
    if (d->pl_check) {
        dr_str_catf(bp, "fakes %d real %d suspected %d\n", d->nb_fake, d->nb_real, d->fake_suspected);
        for (int p = 0; p < d->nb_playlists; p++)
            if (d->pl_check[p])
                dr_str_catf(bp, "check %05u %s repeats %d\n", d->playlists[p].number,
                           d->pl_check[p] == BLURAY_PL_FAKE ? "fake" : "real", d->pl_repeats[p]);
    }
}
