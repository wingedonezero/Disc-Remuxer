/*
 * Title VOB sectors read for ripping: the sector check, the damaged ranges
 * and CSS (dvdread/vob_read.h).
 *
 * This file is part of libdvdread.
 *
 * libdvdread is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * libdvdread is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with libdvdread; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_DVDCSS_DVDCSS_H
# include <dvdcss/dvdcss.h>
#endif

#include "dvdread/dvd_filesystem.h"
#include "dvdread/dvd_reader.h"
#include "dvdread/ifo_read.h"
#include "dvdread/nav_read.h"
#include "dvdread/nav_types.h"
#include "dvdread/vob_read.h"
#include "dvdread_internal.h"
#include "logger.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SECTOR      DVD_VIDEO_LB_LEN
#define MAX_PARTS   9                  /* VTS_nn_1..9.VOB */
#define CLEAR       0x80               /* CSS leaves the first 128 bytes of a sector clear */
#define CHECK_LIMIT 8192               /* sectors after the key's start searched for one the key check can use */

typedef struct {
  char     name[16];                  /* "VTS_01_1.VOB" */
  void    *file;                      /* opened: a folder's file; an image's for the file view */
  uint64_t size;
  int64_t  sector;                    /* image: where the file starts on it when it is one piece, else -1 */
  int64_t (*pieces)[2];               /* image: its pieces on it (first sector -1: not recorded) */
  int      nb_pieces;
} vob_part_t;

/* A piece of the file view (an image's title VOBs through the VOB files):
 * view sectors first..first+n-1 from a VOB file (part) or from the title
 * VOBs' start place (part -1). */
typedef struct {
  int      part;
  uint32_t first, n;
} view_piece_t;

struct dvd_vob_reader_s {
  dvd_reader_t *ctx;
  int           title;
  uint32_t      blocks;               /* the title VOBs' length in sectors by the IFO / the files (CSS) */
  int           mpeg2;                /* the title set's video is MPEG-2 (its video attributes) */
  vob_part_t    parts[MAX_PARTS];
  int           nb_parts;
  /* image: the image, the title VOBs' first sector on it, its sectors */
  void         *image;
  int64_t       start;
  int64_t       image_sectors;
  uint64_t      bytes;                /* folder: the files' bytes */
  /* image: the file view (0 not built yet, 1 built, -1 none), used for the
   * segment being read when view_on */
  int           view_state;
  view_piece_t  view[MAX_PARTS + 1];
  int           nb_view;
  uint32_t      view_sectors;
  int           view_on;
  /* the damaged range being read */
  dvd_vob_damage_t open;
};

static uint32_t be32( const uint8_t *p )
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static const char *damage_text( dvd_damage_t kind )
{
  return kind == DVD_DAMAGE_ZERO ? "every byte is zero" : "no MPEG-2 pack with a packet";
}

/* len bytes of file from pos on: 0, -1 when they cannot all be read */
static int fs_read( dvd_reader_filesystem_h *fs, void *file, int64_t pos, uint8_t *buf, size_t len )
{
  if( fs->file_seek( file, pos, SEEK_SET ) != pos )
    return -1;
  while( len ) {
    ssize_t n = fs->file_read( file, (char *)buf, len );
    if( n <= 0 )
      return -1;
    buf += n;
    len -= (size_t)n;
  }
  return 0;
}

/* The file holding title VOB sector and the byte offset there; 0 when not
 * known. */
static int sector_place( const dvd_vob_reader_t *v, uint32_t sector, char *file, int64_t *offset )
{
  int64_t pos = (int64_t)sector * SECTOR;

  for( int i = 0; i < v->nb_parts; i++ ) {
    const vob_part_t *p = &v->parts[i];
    if( v->image ) {
      /* the file whose pieces hold it on the image */
      int64_t at = v->start + sector, before = 0;
      for( int k = 0; k < p->nb_pieces; before += p->pieces[k][1] * SECTOR, k++ )
        if( p->pieces[k][0] >= 0 && at >= p->pieces[k][0] && at < p->pieces[k][0] + p->pieces[k][1] &&
            (uint64_t)(before + (at - p->pieces[k][0]) * SECTOR) < p->size ) {
          snprintf( file, 16, "%s", p->name );
          *offset = before + (at - p->pieces[k][0]) * SECTOR;
          return 1;
        }
    } else {
      /* the files one after the other */
      if( (uint64_t)pos < p->size ) {
        snprintf( file, 16, "%s", p->name );
        *offset = pos;
        return 1;
      }
      pos -= (int64_t)p->size;
    }
  }
  file[0] = '\0';
  *offset = -1;
  return 0;
}

/* The open damaged range ends: reported, given to *ended. */
static void end_damage( dvd_vob_reader_t *v, dvd_vob_damage_t *ended )
{
  dvd_vob_damage_t *g = &v->open;
  dvd_report_t r = { 0 };
  char text[256], where[96] = "";

  if( ended )
    memset( ended, 0, sizeof(*ended) );
  if( !g->nb_sectors )
    return;
  sector_place( v, g->sector + g->nb_sectors - 1, g->last_file, &g->last_offset );
  if( g->file[0] && !strcmp( g->file, g->last_file ) )
    snprintf( where, sizeof(where), " (%s bytes %" PRId64 "-%" PRId64 ")", g->file, g->offset,
              g->last_offset + SECTOR - 1 );
  else if( g->file[0] && g->last_file[0] )
    snprintf( where, sizeof(where), " (%s byte %" PRId64 " to %s byte %" PRId64 ")", g->file, g->offset,
              g->last_file, g->last_offset + SECTOR - 1 );
  snprintf( text, sizeof(text), "title VOB sectors %" PRIu32 "-%" PRIu32 "%s: %s: left out", g->sector,
            g->sector + g->nb_sectors - 1, where, damage_text( g->kind ) );
  r.level      = DVD_REPORT_ERROR;
  r.file       = g->file[0] ? g->file : NULL;
  r.offset     = g->offset;
  r.sector     = g->sector;
  r.text       = text;
  r.nb_sectors = g->nb_sectors;
  r.damage     = g->kind;
  DVDReadReport( v->ctx->priv, &v->ctx->logcb, &r );
  if( ended )
    *ended = *g;
  memset( g, 0, sizeof(*g) );
}

/* A damaged sector read: the open range grows, or it ends and this one
 * starts the next. */
static void damaged( dvd_vob_reader_t *v, uint32_t sector, dvd_damage_t kind, dvd_vob_damage_t *ended )
{
  dvd_vob_damage_t *g = &v->open;

  if( g->nb_sectors && g->kind == kind && g->sector + g->nb_sectors == sector ) {
    g->nb_sectors++;
    return;
  }
  end_damage( v, ended );
  g->kind       = kind;
  g->sector     = sector;
  g->nb_sectors = 1;
  sector_place( v, sector, g->file, &g->offset );
}

/* What a sector is: DVD_DAMAGE_NONE when it is an MPEG-2 pack followed by a
 * packet start code (ISO/IEC 13818-1 2.5.3.3: pack header 14 bytes plus
 * pack_stuffing_length; the stuffing is counted only when its first byte is
 * not zero). */
static dvd_damage_t sector_kind( const uint8_t *b )
{
  if( b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xba && (b[4] & 0xc0) == 0x40 ) {
    int st = b[13] & 7, h = st && b[14] ? 14 + st : 14;
    if( b[h] == 0 && b[h + 1] == 0 && b[h + 2] == 1 )
      return DVD_DAMAGE_NONE;
  }
  for( int i = 0; i < SECTOR; i++ )
    if( b[i] )
      return DVD_DAMAGE_NO_PACK;
  return DVD_DAMAGE_ZERO;
}

/* ---- CSS ---- */

/* A step of the CSS handling of the title set, said as it happens (an
 * error, a warning, else a notice). */
static void css_report( dvd_vob_reader_t *v, dvd_report_level_t level, const char *fmt, ... )
{
  char text[256];
  int n = snprintf( text, sizeof(text), "VTS_%02d: ", v->title );
  va_list list;

  va_start( list, fmt );
  vsnprintf( text + n, sizeof(text) - n, fmt, list );
  va_end( list );
  DVDReadLog( v->ctx->priv, &v->ctx->logcb, level == DVD_REPORT_ERROR ? DVD_LOGGER_LEVEL_ERROR :
              level == DVD_REPORT_WARN ? DVD_LOGGER_LEVEL_WARN : DVD_LOGGER_LEVEL_NOTICE, "%s", text );
}

/* 1: the sector's packet is of a stream CSS scrambles (the video stream
 * 0xE0, private stream 1, MPEG audio 0xC0-0xC7) and its
 * PES_scrambling_control is set (*h: the pack header's length); 0: not;
 * -1: no MPEG-2 pack with a packet. */
static int css_kind( const uint8_t *b, int *h )
{
  int st, id;

  *h = 0;
  if( b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0xba || (b[4] & 0xc0) != 0x40 )
    return -1;
  st = b[13] & 7;
  *h = st && b[14] ? 14 + st : 14;
  if( b[*h] != 0 || b[*h + 1] != 0 || b[*h + 2] != 1 )
    return -1;
  id = b[*h + 3];
  if( id != 0xbd && id != 0xe0 && (id & 0xf8) != 0xc0 )
    return 0;
  return !!(b[*h + 6] & 0x30);
}

/* An AC-3 frame's size in bytes from its syncinfo (A/52 table 5.18), 0
 * when fscod / frmsizecod are not valid. */
static int ac3_frame_size( const uint8_t *si )
{
  static const uint16_t words[19][3] = {     /* 48, 44.1, 32 kHz */
    {   64,   69,   96 }, {   80,   87,  120 }, {   96,  104,  144 }, {  112,  121,  168 },
    {  128,  139,  192 }, {  160,  174,  240 }, {  192,  208,  288 }, {  224,  243,  336 },
    {  256,  278,  384 }, {  320,  348,  480 }, {  384,  417,  576 }, {  448,  487,  672 },
    {  512,  557,  768 }, {  640,  696,  960 }, {  768,  835, 1152 }, {  896,  975, 1344 },
    { 1024, 1114, 1536 }, { 1152, 1253, 1728 }, { 1280, 1393, 1920 },
  };
  int fscod = si[4] >> 6, frmsizecod = si[4] & 0x3f;

  if( fscod == 3 || frmsizecod >= 38 )
    return 0;
  return 2 * (words[frmsizecod >> 1][fscod] + (fscod == 1 ? frmsizecod & 1 : 0));
}

/* The A/52 CRC (generator x^16 + x^15 + x^2 + 1, A/52 7.10.1) of n bytes:
 * 0 over a whole frame after its syncword when the frame is intact. */
static uint16_t ac3_crc( const uint8_t *p, size_t n )
{
  uint16_t c = 0;

  while( n-- ) {
    c ^= (uint16_t)(*p++ << 8);
    for( int i = 0; i < 8; i++ )
      c = c & 0x8000 ? (uint16_t)(c << 1) ^ 0x8005 : (uint16_t)(c << 1);
  }
  return c;
}

/* Whether a descrambled sector (pack header h bytes) shows the key right:
 * 1 right, 0 wrong, -1 the sector cannot tell (*what: 1 AC-3, 2 video):
 * an AC-3 frame whose sync information lies in the sector's clear part and
 * which ends in its scrambled part (the frame's CRC, A/52 7.10.1), or an
 * MPEG-2 sequence header in the clear part whose sequence_extension start
 * code lies in the scrambled part (ISO/IEC 13818-2 6.2.2: in MPEG-2 video a
 * sequence_extension follows every sequence header). */
static int key_check( const uint8_t *sec, int h, int mpeg2, int *what )
{
  int id = sec[h + 3], len = sec[h + 4] << 8 | sec[h + 5], start = h + 9 + sec[h + 8], end = h + 6 + len;

  if( end > SECTOR || start >= end )
    return -1;
  if( id == 0xbd && (sec[start] & 0xf8) == 0x80 && end - start > 4 ) {
    /* AC-3: sub-stream id, frame count, first access unit pointer (from the
     * byte after it) */
    int first = sec[start + 2] << 8 | sec[start + 3], au = start + 3 + first, fs;

    if( !first || au + 7 > CLEAR || sec[au] != 0x0b || sec[au + 1] != 0x77 || !(fs = ac3_frame_size( sec + au )) ||
        au + fs > end || au + fs <= CLEAR )
      return -1;
    *what = 1;
    return !ac3_crc( sec + au + 2, (size_t)fs - 2 );
  }
  if( id == 0xe0 && mpeg2 ) {
    /* MPEG-2 video: a sequence header starting at the payload */
    int p = start, ext;

    if( p + 12 > CLEAR || sec[p] != 0 || sec[p + 1] != 0 || sec[p + 2] != 1 || sec[p + 3] != 0xb3 )
      return -1;
    ext = p + 12;
    if( sec[p + 11] & 2 )               /* load_intra_quantiser_matrix: 64 bytes, then the non-intra flag */
      ext += 64;
    if( ext > end || (sec[ext - 1] & 1 && (ext += 64) > end) || ext + 5 > end || ext + 4 <= CLEAR )
      return -1;
    *what = 2;
    return sec[ext] == 0 && sec[ext + 1] == 0 && sec[ext + 2] == 1 && sec[ext + 3] == 0xb5 && sec[ext + 4] >> 4 == 1;
  }
  return -1;
}

#ifdef HAVE_DVDCSS_DVDCSS_H
/* The title VOBs as a stream for libdvdcss (it cracks the key from their
 * scrambled sectors). */
typedef struct {
  dvd_vob_reader_t *v;
  uint32_t          pos;              /* sector */
} key_stream_t;

static int read_sector( dvd_vob_reader_t *v, uint32_t sector, uint8_t *buf );

static int ks_seek( void *opaque, uint64_t pos )
{
  key_stream_t *k = opaque;
  k->pos = (uint32_t)(pos / SECTOR);
  return 0;
}

static int ks_read( void *opaque, void *buf, int size )
{
  key_stream_t *k = opaque;
  int done = 0;

  for( ; done + SECTOR <= size && k->pos < k->v->blocks; done += SECTOR, k->pos++ )
    if( read_sector( k->v, k->pos, (uint8_t *)buf + done ) < 0 )
      return done ? done : -1;
  return done;
}

static int ks_readv( void *opaque, const void *iov, int blocks )
{
  (void)opaque; (void)iov; (void)blocks;
  return -1;
}

/* The key's check: the sectors from sector on, descrambled, until one tells:
 * 1 right, 0 wrong (or none can be read). */
static int check_key( dvd_vob_reader_t *v, uint32_t sector, const uint8_t *key )
{
  uint8_t b[SECTOR];

  for( uint32_t s = sector; s < v->blocks && s - sector < CHECK_LIMIT; s++ ) {
    int h, what = 0, ok;

    if( read_sector( v, s, b ) < 0 )
      return 0;
    if( css_kind( b, &h ) != 1 )
      continue;
    dvdcss_descramble( key, b );
    if( (ok = key_check( b, h, v->mpeg2, &what )) < 0 )
      continue;
    if( ok )
      Log2( v->ctx, "VTS_%02d: the CSS title key is right on sector %" PRIu32 " (%s)", v->title, s,
            what == 1 ? "an AC-3 frame's CRC" : "an MPEG-2 sequence header's extension" );
    else
      css_report( v, DVD_REPORT_ERROR, "the CSS title key is wrong on sector %" PRIu32 " (%s)", s,
                  what == 1 ? "an AC-3 frame's CRC" : "an MPEG-2 sequence header's extension" );
    return ok;
  }
  css_report( v, DVD_REPORT_WARN, "no sector within %d after %" PRIu32 " can check the CSS title key: it is used "
              "unchecked [untested on real discs]", CHECK_LIMIT, sector );
  return 1;
}
#endif

/* The title set's title VOBs lie inside title set o's (the same sectors of
 * an image) and o has its key. */
static int key_shared( const dvd_reader_t *ctx, int title, int o )
{
  int64_t fb = ctx->vob_css[title].start, ob = ctx->vob_css[o].start;
  uint32_t fn = ctx->vob_css[title].blocks, on = ctx->vob_css[o].blocks;

  return o != title && ctx->vob_css[o].state > 0 && fb && ob &&
         (ob == fb || (ob < fb && fn + fb <= ob + on));
}

/* The title set's key, looked for from its scrambled sector on: 0 found
 * (or taken), -1 none (never looked for again). */
static int find_key( dvd_vob_reader_t *v, uint32_t sector )
{
  dvd_reader_t *ctx = v->ctx;
  uint8_t *key = ctx->vob_css[v->title].key;

  for( int o = 1; o < 100; o++ )
    if( key_shared( ctx, v->title, o ) ) {
      memcpy( key, ctx->vob_css[o].key, 5 );
      ctx->vob_css[v->title].state = 1;
      Log2( ctx, "VTS_%02d: CSS title key taken from VTS_%02d (its title VOBs hold these)", v->title, o );
      return 0;
    }
  css_report( v, DVD_REPORT_INFO, "sector %" PRIu32 " is scrambled: looking for the CSS title key", sector );
  ctx->vob_css[v->title].state = -1;
#ifdef HAVE_DVDCSS_DVDCSS_H
  {
    key_stream_t ks = { v, 0 };
    dvdcss_stream_cb cb = { ks_seek, ks_read, ks_readv };
    dvdcss_t css;
    int ret;

    if( !(css = dvdcss_open_stream( &ks, &cb )) ) {
      css_report( v, DVD_REPORT_ERROR, "libdvdcss cannot be opened on the title VOBs" );
      return -1;
    }
    ret = dvdcss_title_key( css, (int)sector, key );
    dvdcss_close( css );
    if( ret <= 0 ) {
      css_report( v, DVD_REPORT_ERROR, "no CSS title key found (%s): its scrambled sectors cannot be read",
                  ret < 0 ? "libdvdcss failed" : "no scrambled data from there on" );
      return -1;
    }
    Log3( ctx, "VTS_%02d: CSS title key %02x %02x %02x %02x %02x", v->title, key[0], key[1], key[2], key[3], key[4] );
    if( !check_key( v, sector, key ) )
      return -1;
    ctx->vob_css[v->title].state = 1;
    css_report( v, DVD_REPORT_INFO, "CSS title key found" );
    return 0;
  }
#else
  css_report( v, DVD_REPORT_ERROR, "no CSS title key (built without libdvdcss): its scrambled sectors cannot be read" );
  return -1;
#endif
}

/* ---- where an image's title VOBs start ---- */

/* A navigation pack (DVD-Video: pack header, system header, PCI and DSI
 * packets) that says it is its VOB's first sector: its PCI's and DSI's
 * nv_pck_lbn are 0. */
static int vob_first_nav( const uint8_t *b )
{
  static const uint8_t pci[7] = { 0, 0, 1, 0xbf, 0x03, 0xd4, 0x00 }, dsi[7] = { 0, 0, 1, 0xbf, 0x03, 0xfa, 0x01 };

  return b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xba && (b[4] & 0xc0) == 0x40 &&
         be32( b + 0x0e ) == 0x000001bb && b[0x12] == 0 && b[0x13] == 0x12 &&
         !memcmp( b + 0x26, pci, 7 ) && !memcmp( b + 0x400, dsi, 7 ) &&
         be32( b + 0x2d ) == 0 && be32( b + 0x40b ) == 0;
}

/* Where title set title's title VOBs (menu: its menu VOBs; title 0: the
 * video manager's) start on the image: the IFO header's place (the IFO's
 * sector + vtstt_vobs / vtsm_vobs / vmgm_vobs) and the file system's
 * VTS_nn_1.VOB (VTS_nn_0.VOB, VIDEO_TS.VOB) when they agree or the file
 * system has no single place for it; when they differ (reported), the one
 * holding its VOB's first navigation pack, the IFO header's first; neither:
 * the IFO header's. 0: none (the field 0). */
static int64_t place_vobs( dvd_reader_t *ctx, ifo_handle_t *ifo, int title, int menu, void *image, int64_t ifo_at )
{
  dvd_reader_filesystem_h *fs = ctx->fs;
  const char *what = menu ? "menu VOBs" : "title VOBs", *field = menu ? (title ? "vtsm_vobs" : "vmgm_vobs") :
                     "vtstt_vobs";
  char path[48], vob[16], name[16], text[320];
  int64_t header, fs_at = -1, chosen;
  dvdstat_t st;
  uint8_t raw[4], b[SECTOR];
  const char *why;
  dvd_report_t r = { 0 };

  if( !ifoRead_rec_bytes( ifo, menu ? 0xc0 : 0xc4, raw, 4 ) || !be32( raw ) )
    return 0;
  header = ifo_at + be32( raw );
  if( title )
    snprintf( vob, sizeof(vob), "VTS_%02d_%d.VOB", title, !menu );
  else
    snprintf( vob, sizeof(vob), "VIDEO_TS.VOB" );
  snprintf( path, sizeof(path), "/VIDEO_TS/%s", vob );
  if( DVDStatFile_( ctx, path, &st ) >= 0 )
    fs_at = st.sector;
  if( fs_at < 0 || fs_at == header ) {
    if( fs_at < 0 )
      Log3( ctx, "%s: the file system has no single place for %s: the %s start where the IFO header puts them",
            title ? "VTS" : "VIDEO_TS", vob, what );
    return header;
  }
  if( fs_read( fs, image, header * SECTOR, b, SECTOR ) == 0 && vob_first_nav( b ) ) {
    chosen = header;
    why = "the IFO header's: its first sector is a VOB's first navigation pack";
  } else if( fs_read( fs, image, fs_at * SECTOR, b, SECTOR ) == 0 && vob_first_nav( b ) ) {
    chosen = fs_at;
    why = "the file system's: its first sector is a VOB's first navigation pack";
  } else {
    chosen = header;
    why = "the IFO header's: neither place starts with a VOB's first navigation pack";
  }
  if( title )
    snprintf( name, sizeof(name), "VTS_%02d_0.IFO", title );
  else
    snprintf( name, sizeof(name), "VIDEO_TS.IFO" );
  snprintf( text, sizeof(text), "the IFO header puts the %s at sector %" PRId64 " (%s %" PRIu32 "), the file "
            "system's %s is at sector %" PRId64 ": they are read from %s", what, header, field, be32( raw ), vob,
            fs_at, why );
  r.level    = DVD_REPORT_WARN;
  r.file     = name;
  r.table    = title ? "VTSI_MAT" : "VMGI_MAT";
  r.field    = field;
  r.offset   = menu ? 0xc0 : 0xc4;
  r.sector   = -1;
  r.bytes    = raw;
  r.nb_bytes = 4;
  r.text     = text;
  DVDReadReport( ctx->priv, &ctx->logcb, &r );
  return chosen;
}

/* The title VOBs' start (decided once) and their length by the IFO (up to
 * its BUP, the IFO's place when the BUP has none of its own). */
static int64_t place_title_vobs( dvd_reader_t *ctx, ifo_handle_t *ifo, int title, void *image, int64_t ifo_at )
{
  char path[48];
  dvdstat_t bup;
  int64_t bup_at;
  uint8_t raw[4];

  if( ctx->vob_css[title].placed )
    return ctx->vob_css[title].start;
  ctx->vob_css[title].placed = 1;
  if( !(ctx->vob_css[title].start = place_vobs( ctx, ifo, title, 0, image, ifo_at )) ||
      !ifoRead_rec_bytes( ifo, 0xc4, raw, 4 ) )
    return ctx->vob_css[title].start;
  /* the length by the IFO header's vtstt_vobs, whichever start was chosen */
  snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.BUP", title );
  bup_at = DVDStatFile_( ctx, path, &bup ) >= 0 && bup.sector >= 0 ? bup.sector : ifo_at;
  ctx->vob_css[title].blocks = (uint32_t)(bup_at - ifo_at) - be32( raw );
  return ctx->vob_css[title].start;
}

int64_t DVDTitleVOBStart_rec( ifo_handle_t *ifo, int title )
{
  struct ifo_handle_private_s *ifop;
  dvd_reader_t *ctx;
  char path[48];
  dvdstat_t st;
  void *image;
  int64_t start;

  if( !ifo || title < 1 || title > 99 )
    return 0;
  ifop = container_of( ifo, struct ifo_handle_private_s, handle );
  ctx  = ifop->ctx;
  if( ctx->vob_css[title].placed )
    return ctx->vob_css[title].start;
  snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.IFO", title );
  if( !ctx->fs || !ctx->fs->image_open || DVDStatFile_( ctx, path, &st ) < 0 || st.sector < 0 ||
      !(image = ctx->fs->image_open( ctx->fs )) )
    return 0;
  start = place_title_vobs( ctx, ifo, title, image, st.sector );
  ctx->fs->file_close( image );
  return start;
}

int64_t DVDMenuVOBStart_rec( ifo_handle_t *ifo, int title )
{
  struct ifo_handle_private_s *ifop;
  dvd_reader_t *ctx;
  char path[48];
  dvdstat_t st;
  void *image;

  if( !ifo || title < 0 || title > 99 )
    return 0;
  ifop = container_of( ifo, struct ifo_handle_private_s, handle );
  ctx  = ifop->ctx;
  if( ctx->vob_css[title].menu_placed )
    return ctx->vob_css[title].menu_start;
  if( title )
    snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.IFO", title );
  else
    snprintf( path, sizeof(path), "/VIDEO_TS/VIDEO_TS.IFO" );
  if( !ctx->fs || !ctx->fs->image_open || DVDStatFile_( ctx, path, &st ) < 0 || st.sector < 0 ||
      !(image = ctx->fs->image_open( ctx->fs )) )
    return 0;
  ctx->vob_css[title].menu_placed = 1;
  ctx->vob_css[title].menu_start  = place_vobs( ctx, ifo, title, 1, image, st.sector );
  ctx->fs->file_close( image );
  return ctx->vob_css[title].menu_start;
}

/* ---- the file view: an image's title VOBs through its VOB files ---- */

/* Builds the file view: VTS_nn_1.VOB, _2, ... one after the other from the
 * title VOBs' start, until they hold the title VOBs' sectors (by the IFO):
 * each file must start where the previous one ends (or have no place
 * known) and be whole sectors; VTS_nn_1 and _2 must be there (unless the
 * first holds them all), a missing third or later one is taken from the
 * start place to the end. 1 built, 0 none. */
static int build_view( dvd_vob_reader_t *v )
{
  dvd_reader_filesystem_h *fs = v->ctx->fs;
  uint32_t rem = v->blocks, off = 0;
  char path[48], full[PATH_MAX + 1];

  for( int k = 1; rem; k++ ) {
    vob_part_t *p = k <= v->nb_parts ? &v->parts[k - 1] : NULL;
    view_piece_t piece = { -1, off, rem };
    uint32_t n = rem;

    if( !p ) {
      if( k < 3 ) {
        Log3( v->ctx, "VTS_%02d: no file view: VTS_%02d_%d.VOB is missing", v->title, v->title, k );
        return 0;
      }
    } else {
      int64_t at = p->nb_pieces > 0 && p->pieces[0][0] > 0 ? p->pieces[0][0] : 0;
      if( (at && at != v->start + off) || p->size % SECTOR ) {
        Log3( v->ctx, "VTS_%02d: no file view: %s %s", v->title, p->name,
              p->size % SECTOR ? "is not whole sectors" : "does not start where the title VOBs have it" );
        return 0;
      }
      snprintf( path, sizeof(path), "/VIDEO_TS/%s", p->name );
      if( !p->file && (DVDFindFile_( v->ctx, path, full ) < 0 || !(p->file = fs->file_open( fs, full ))) )
        return 0;
      piece.part = k - 1;
      piece.n    = n = (uint32_t)(p->size / SECTOR);
    }
    v->view[v->nb_view++] = piece;
    off += n;
    if( v->nb_view == 1 && rem <= n )
      break;                            /* the first file holds them all */
    if( n > rem )
      break;
    rem -= n;
  }
  v->view_sectors = off;
  return 1;
}

/* View sector s into buf: 0, -1 when it cannot be read. */
static int view_read( dvd_vob_reader_t *v, uint32_t s, uint8_t *buf )
{
  for( int i = 0; i < v->nb_view; i++ ) {
    const view_piece_t *p = &v->view[i];
    if( s < p->first || s - p->first >= p->n )
      continue;
    if( p->part < 0 )
      return fs_read( v->ctx->fs, v->image, (v->start + s) * SECTOR, buf, SECTOR );
    return fs_read( v->ctx->fs, v->parts[p->part].file, (int64_t)(s - p->first) * SECTOR, buf, SECTOR );
  }
  return -1;
}

/* The two sectors from s on read the same through the file view and from
 * the start place. */
static int same_two( dvd_vob_reader_t *v, uint32_t s )
{
  uint8_t a[2 * SECTOR], b[2 * SECTOR];

  return view_read( v, s, a ) == 0 && view_read( v, s + 1, a + SECTOR ) == 0 &&
         fs_read( v->ctx->fs, v->image, (v->start + s) * SECTOR, b, sizeof(b) ) == 0 && !memcmp( a, b, sizeof(a) );
}

/* An extent of the segment as the file view and the start place have it:
 * both hold it, its first two sectors and (3 or more) its last two read
 * the same. */
static int extent_same( dvd_vob_reader_t *v, uint32_t sector, uint32_t count )
{
  uint64_t end = (uint64_t)sector + count;

  return end <= v->view_sectors && end <= (uint64_t)(v->image_sectors - v->start) && same_two( v, sector ) &&
         (count < 3 || same_two( v, sector + count - 2 ));
}

int DVDUseTitleVOBView_rec( dvd_vob_reader_t *v, const uint32_t *sectors, const uint32_t *counts, int n )
{
  if( !v )
    return 0;
  v->view_on = 0;
  if( !v->image || n <= 0 )
    return 0;
  if( !v->view_state )
    v->view_state = build_view( v ) ? 1 : -1;
  if( v->view_state < 0 || !extent_same( v, sectors[0], counts[0] ) ||
      (n > 1 && !extent_same( v, sectors[n - 1], counts[n - 1] )) )
    return 0;
  for( int i = 1; i < n - 1; i++ )
    if( (uint64_t)sectors[i] + counts[i] > v->view_sectors )
      return 0;
  v->view_on = 1;
  return 1;
}

/* The title set's video coding (vts_video_attr, its first two bits: 1 = MPEG-2). */
static dvd_vob_reader_t *opened( dvd_vob_reader_t *v, ifo_handle_t *ifo )
{
  uint8_t attr;

  v->mpeg2 = ifoRead_rec_bytes( ifo, 0x200, &attr, 1 ) && attr >> 6 == 1;
  return v;
}

dvd_vob_reader_t *DVDOpenTitleVOBs_rec( ifo_handle_t *ifo, int title )
{
  struct ifo_handle_private_s *ifop;
  dvd_reader_t *ctx;
  dvd_vob_reader_t *v;
  char path[48], full[PATH_MAX + 1], why[96] = "";
  dvdstat_t st;
  uint8_t b[4];
  int found;

  if( !ifo || title < 1 || title > 99 )
    return NULL;
  ifop = container_of( ifo, struct ifo_handle_private_s, handle );
  ctx  = ifop->ctx;
  if( !ctx->fs || !(v = calloc( 1, sizeof(*v) )) )
    return NULL;
  v->ctx   = ctx;
  v->title = title;

  /* the title VOB files, up to the first missing one */
  for( int k = 1; k <= MAX_PARTS; k++ ) {
    vob_part_t *p = &v->parts[v->nb_parts];
    snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_%d.VOB", title, k );
    if( DVDFindFile_( ctx, path, full ) < 0 )
      break;
    st.sector = -1;
    if( ctx->fs->stat( ctx->fs, full, &st ) < 0 )
      break;
    snprintf( p->name, sizeof(p->name), "VTS_%02d_%d.VOB", title, k );
    p->size   = st.size;
    p->sector = st.sector;
    v->nb_parts++;
    /* where its pieces lie on an image */
    if( ctx->fs->file_pieces ) {
      int np = ctx->fs->file_pieces( ctx->fs, full, NULL, 0 );
      if( np > 0 && (p->pieces = calloc( np, sizeof(*p->pieces) )) )
        p->nb_pieces = ctx->fs->file_pieces( ctx->fs, full, p->pieces, np );
    } else if( st.sector >= 0 && (p->pieces = calloc( 1, sizeof(*p->pieces) )) ) {
      p->pieces[0][0] = st.sector;
      p->pieces[0][1] = (int64_t)((st.size + SECTOR - 1) / SECTOR);
      p->nb_pieces    = 1;
    }
  }

  /* an image: the title VOBs from where the IFO header puts them */
  snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.IFO", title );
  if( ctx->fs->image_open && DVDStatFile_( ctx, path, &st ) >= 0 && st.sector >= 0 ) {
    int64_t size;
    if( !ifoRead_rec_bytes( ifo, 0xc4, b, 4 ) || !be32( b ) ) {   /* vtstt_vobs */
      snprintf( why, sizeof(why), "the IFO header gives no place for them (vtstt_vobs)" );
      goto fail;
    }
    if( !(v->image = ctx->fs->image_open( ctx->fs )) ) {
      snprintf( why, sizeof(why), "the image cannot be opened" );
      goto fail;
    }
    if( !(v->start = place_title_vobs( ctx, ifo, title, v->image, st.sector )) ) {
      snprintf( why, sizeof(why), "where they start on the image cannot be found" );
      goto fail;
    }
    if( (size = ctx->fs->file_seek( v->image, 0, SEEK_END )) < 0 ) {
      snprintf( why, sizeof(why), "the image's size cannot be read" );
      goto fail;
    }
    v->image_sectors = (size + SECTOR - 1) / SECTOR;
    if( v->start >= v->image_sectors ) {
      snprintf( why, sizeof(why), "they start at sector %" PRId64 ", past the image's %" PRId64 " sectors",
                v->start, v->image_sectors );
      goto fail;
    }
    v->blocks = ctx->vob_css[title].blocks;
    return opened( v, ifo );
  }

  /* a folder: the files joined */
  found = v->nb_parts;
  for( int i = 0; i < v->nb_parts; i++ ) {
    vob_part_t *p = &v->parts[i];
    snprintf( path, sizeof(path), "/VIDEO_TS/%s", p->name );
    if( DVDFindFile_( ctx, path, full ) < 0 || !(p->file = ctx->fs->file_open( ctx->fs, full )) ) {
      v->nb_parts = i;
      break;
    }
    v->bytes += p->size;
  }
  if( !v->nb_parts ) {
    snprintf( why, sizeof(why), found ? "VTS_%02d_1.VOB cannot be opened" : "VTS_%02d_1.VOB is missing", title );
    goto fail;
  }
  v->blocks = (uint32_t)(v->bytes / SECTOR);
  return opened( v, ifo );

fail:
  /* said, with why, to whoever asked for them (the title list, the scan, a rip) */
  snprintf( path, sizeof(path), "VTS_%02d_0.IFO", title );
  DVDReadReportf( ctx->priv, &ctx->logcb, DVD_REPORT_ERROR, path, NULL, -1, "title set %d: its title VOBs cannot be "
                  "opened: %s", title, why );
  DVDCloseTitleVOBs_rec( v );
  return NULL;
}

int DVDTitleVOBSectorPlace_rec( const dvd_vob_reader_t *v, uint32_t sector, char file[16], int64_t *offset )
{
  if( !v ) {
    file[0] = '\0';
    *offset = -1;
    return 0;
  }
  return sector_place( v, sector, file, offset );
}

uint32_t DVDTitleVOBSectors_rec( const dvd_vob_reader_t *v )
{
  if( !v )
    return 0;
  if( v->image )
    return (uint32_t)(v->image_sectors - v->start);
  return (uint32_t)(v->bytes / SECTOR);
}

/* The sector's bytes: 0, -1 when they cannot be read. */
static int read_sector( dvd_vob_reader_t *v, uint32_t sector, uint8_t *buf )
{
  dvd_reader_filesystem_h *fs = v->ctx->fs;
  int64_t pos = (int64_t)sector * SECTOR;
  size_t len = SECTOR;

  if( v->image ) {
    /* from the start place; when that fails, through the file view (the
     * segment's checked), then from the start place once more */
    if( v->start + sector >= v->image_sectors )
      return -1;
    if( fs_read( fs, v->image, (v->start + sector) * SECTOR, buf, SECTOR ) == 0 )
      return 0;
    if( v->view_on && view_read( v, sector, buf ) == 0 ) {
      Log2( v->ctx, "VTS_%02d: title VOB sector %" PRIu32 " cannot be read where the title VOBs start: read "
            "through the VOB files", v->title, sector );
      return 0;
    }
    return fs_read( fs, v->image, (v->start + sector) * SECTOR, buf, SECTOR );
  }
  if( (uint64_t)pos + SECTOR > v->bytes )
    return -1;
  /* the files' bytes one after the other (a sector may span two files) */
  for( int i = 0; i < v->nb_parts && len; i++ ) {
    vob_part_t *p = &v->parts[i];
    size_t k;
    if( (uint64_t)pos >= p->size ) {
      pos -= (int64_t)p->size;
      continue;
    }
    k = (size_t)(p->size - (uint64_t)pos) < len ? (size_t)(p->size - (uint64_t)pos) : len;
    if( fs_read( fs, p->file, pos, buf, k ) < 0 )
      return -1;
    buf += k;
    len -= k;
    pos  = 0;
  }
  return len ? -1 : 0;
}

int DVDReadTitleVOBSector_rec( dvd_vob_reader_t *v, uint32_t sector, uint8_t *buf, dvd_vob_damage_t *ended )
{
  dvd_damage_t kind;
  int h;

  if( ended )
    memset( ended, 0, sizeof(*ended) );
  if( !v || !buf )
    return -1;
  if( read_sector( v, sector, buf ) < 0 ) {
    end_damage( v, ended );
    Log3( v->ctx, "VTS_%02d: title VOB sector %" PRIu32 " cannot be read", v->title, sector );
    return -1;
  }
  if( (kind = sector_kind( buf )) != DVD_DAMAGE_NONE ) {
    damaged( v, sector, kind, ended );
    return 0;
  }
  end_damage( v, ended );
  /* CSS: a scrambled packet descrambled with the title set's key, the
   * scrambling bits cleared */
  if( css_kind( buf, &h ) == 1 ) {
    int state = v->ctx->vob_css[v->title].state;
    if( state < 0 || (!state && find_key( v, sector ) < 0) )
      return -2;
#ifdef HAVE_DVDCSS_DVDCSS_H
    dvdcss_descramble( v->ctx->vob_css[v->title].key, buf );
#endif
    buf[h + 6] &= 0xcf;
  }
  return 1;
}

int DVDTitleVOBKey_rec( dvd_reader_t *ctx, int title, uint8_t key[5] )
{
  if( !ctx || title < 1 || title > 99 )
    return 0;
  if( ctx->vob_css[title].state > 0 && key )
    memcpy( key, ctx->vob_css[title].key, 5 );
  return ctx->vob_css[title].state;
}

void DVDEndTitleVOBDamage_rec( dvd_vob_reader_t *v, dvd_vob_damage_t *ended )
{
  if( ended )
    memset( ended, 0, sizeof(*ended) );
  if( v )
    end_damage( v, ended );
}

/* ---- VOBUs ---- */

typedef struct dvd_vobu_store_s vobu_store_t;

/* A finding about the title set's VOBUs (an IFO's table or the navigation
 * packs: the disc's findings). */
static void vobu_report( dvd_vob_reader_t *v, dvd_report_level_t level, const char *file, const char *table,
                         int64_t sector, const char *fmt, ... )
{
  char text[256];
  va_list list;

  va_start( list, fmt );
  vsnprintf( text, sizeof(text), fmt, list );
  va_end( list );
  DVDReadReportf( v->ctx->priv, &v->ctx->logcb, level, file, table, sector, "%s", text );
}

/* The store a title set's records are in, and the key of its sector. */
static vobu_store_t *vobu_store( dvd_vob_reader_t *v )
{
  return v->ctx->vob_css[v->title].start ? &v->ctx->vobus : &v->ctx->vob_css[v->title].vobus;
}

static uint32_t vobu_key( const dvd_vob_reader_t *v, uint32_t sector )
{
  return (uint32_t)v->ctx->vob_css[v->title].start + sector;
}

static int vobu_lower( const vobu_store_t *st, uint32_t key )
{
  int lo = 0, hi = st->nb;
  while( lo < hi ) {
    int mid = (lo + hi) / 2;
    if( st->recs[mid].key < key )
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* A record inserted; the first one of a key is kept. */
static int vobu_insert( vobu_store_t *st, const dvd_vobu_rec_t *r )
{
  int at = vobu_lower( st, r->key );

  if( at < st->nb && st->recs[at].key == r->key )
    return 0;
  if( st->nb == st->alloc ) {
    int alloc = st->alloc ? 2 * st->alloc : 16;
    dvd_vobu_rec_t *n = realloc( st->recs, alloc * sizeof(*n) );
    if( !n )
      return -1;
    st->recs  = n;
    st->alloc = alloc;
  }
  memmove( st->recs + at + 1, st->recs + at, (st->nb - at) * sizeof(*st->recs) );
  st->recs[at] = *r;
  st->nb++;
  return 0;
}

const dvd_vobu_rec_t *DVDFindVobu_rec( dvd_vob_reader_t *v, uint32_t sector, int64_t *next_key )
{
  vobu_store_t *st = vobu_store( v );
  uint32_t key = vobu_key( v, sector ), start = (uint32_t)v->ctx->vob_css[v->title].start;
  int at = vobu_lower( st, key );

  if( at == st->nb ) {
    if( next_key )
      *next_key = -1;
    return NULL;
  }
  if( st->recs[at].key == key ) {
    if( next_key )
      *next_key = at + 1 < st->nb ? (int64_t)st->recs[at + 1].key - start : -1;
    return &st->recs[at];
  }
  if( next_key )
    *next_key = (int64_t)st->recs[at].key - start;
  return NULL;
}

int DVDMarkVobuBad_rec( dvd_vob_reader_t *v, uint32_t sector )
{
  dvd_vobu_rec_t none = { vobu_key( v, sector ), 0, 0, DVD_VOBU_NO_NAV, 0, 0 };
  return vobu_insert( vobu_store( v ), &none );
}

int DVDIsNavPack_rec( const uint8_t *b )
{
  static const uint8_t pci[7] = { 0, 0, 1, 0xbf, 0x03, 0xd4, 0x00 }, dsi[7] = { 0, 0, 1, 0xbf, 0x03, 0xfa, 0x01 };

  return b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xba && (b[4] & 0xc0) == 0x40 &&
         be32( b + 0x0e ) == 0x000001bb && b[0x12] == 0 && b[0x13] == 0x12 &&
         !memcmp( b + 0x26, pci, 7 ) && !memcmp( b + 0x400, dsi, 7 );
}

/* A navigation pack's fields, read by libdvdread's PCI / DSI readers (their
 * checks report to the reader). */
static void parse_nav( dvd_vob_reader_t *v, const uint8_t *b, dvd_vobu_nav_t *n )
{
  uint8_t copy[SECTOR];
  pci_t pci;
  dsi_t dsi;

  memcpy( copy, b, SECTOR );
  navRead_PCI_ctx( v->ctx, &pci, copy + 0x2d );
  navRead_DSI_ctx( v->ctx, &dsi, copy + DSI_START_BYTE );
  n->vobu_s_ptm = pci.pci_gi.vobu_s_ptm;
  n->vobu_e_ptm = pci.pci_gi.vobu_e_ptm;
  n->vobu_ea    = dsi.dsi_gi.vobu_ea;
  n->category   = dsi.sml_pbi.category;
  n->ilvu_ea    = dsi.sml_pbi.ilvu_ea;
  n->next_vobu  = dsi.vobu_sri.next_vobu;
}

int DVDReadVobuNav_rec( dvd_vob_reader_t *v, uint32_t sector, dvd_vobu_nav_t *n )
{
  uint8_t b[SECTOR];

  if( read_sector( v, sector, b ) == 0 ) {
    int blank = 1;
    if( DVDIsNavPack_rec( b ) ) {
      parse_nav( v, b, n );
      return 1;
    }
    for( int i = 0; i < 32; i++ )
      blank &= b[i] == 0xff;
    if( blank && be32( b + 0x2d ) == sector && be32( b + 0x40b ) == sector && be32( b + 0x40f ) <= 0xfffff ) {
      /* a navigation pack whose headers were blanked: taken, 0.5 s */
      memset( n, 0, sizeof(*n) );
      n->vobu_e_ptm = 45000;
      n->vobu_ea    = be32( b + 0x40f );
      n->next_vobu  = n->vobu_ea + 1;
      vobu_report( v, DVD_REPORT_INFO, NULL, "NAV", sector, "title VOB sector %" PRIu32 " is a navigation pack "
                   "whose headers were blanked: taken as a VOBU of %" PRIu32 " sectors and 0.5 s", sector,
                   n->vobu_ea + 1 );
      return 1;
    }
    Log3( v->ctx, "VTS_%02d: title VOB sector %" PRIu32 " is no navigation pack (it starts %02x %02x %02x %02x "
          "%02x %02x %02x %02x)", v->title, sector, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7] );
  }
  DVDMarkVobuBad_rec( v, sector );
  return 0;
}

int DVDNoteVobuSector_rec( dvd_vob_reader_t *v, uint32_t sector, const uint8_t *buf )
{
  dvd_vobu_nav_t n;
  uint32_t next, len;

  if( !DVDIsNavPack_rec( buf ) )
    return DVDMarkVobuBad_rec( v, sector );
  parse_nav( v, buf, &n );
  DVDRegisterVobu_rec( v, &n, sector, 1, &next, &len );
  return 0;
}

int DVDRegisterVobu_rec( dvd_vob_reader_t *v, const dvd_vobu_nav_t *n, uint32_t sector, int allow_self,
                         uint32_t *next_out, uint32_t *len_out )
{
  uint32_t nx = n->next_vobu & 0x7fffffff, ea = n->vobu_ea;
  int64_t next = nx == 0x3fffffff ? -2 : (int64_t)sector + nx;

  if( next == sector ) {
    if( !allow_self ) {
      Log3( v->ctx, "VTS_%02d: the VOBU at %" PRIu32 " names itself as the next one", v->title, sector );
      return 0;
    }
    *next_out = DVD_VOBU_SELF;
    *len_out  = ea + 1;
    return 1;                           /* nothing recorded */
  }
  if( next == -2 && (n->category & 0x4000) && ea != n->ilvu_ea ) {
    /* the cell ends inside an interleaved unit */
    if( n->ilvu_ea < ea ) {
      char file[16];
      int64_t offset;
      dvd_report_t r = { 0 };
      char text[256];
      sector_place( v, sector, file, &offset );
      snprintf( text, sizeof(text), "title VOB sector %" PRIu32 ": the interleaved unit ends (ilvu_ea %" PRIu32
                ") before its VOBU (vobu_ea %" PRIu32 "): the data is corrupt here, worked around", sector,
                n->ilvu_ea, ea );
      r.level  = DVD_REPORT_WARN;
      r.file   = file[0] ? file : NULL;
      r.table  = "DSI";
      r.field  = "sml_pbi.ilvu_ea";
      r.offset = offset;
      r.sector = sector;
      r.text   = text;
      DVDReadReport( v->ctx->priv, &v->ctx->logcb, &r );
    } else {
      dvd_vobu_nav_t n2;
      uint32_t nn, nl;
      next = (int64_t)sector + ea + 1;
      if( !DVDFindVobu_rec( v, (uint32_t)next, NULL ) &&
          (!DVDReadVobuNav_rec( v, (uint32_t)next, &n2 ) ||
           !DVDRegisterVobu_rec( v, &n2, (uint32_t)next, 0, &nn, &nl )) ) {
        Log3( v->ctx, "VTS_%02d: the VOBU after %" PRIu32 " in its interleaved unit cannot be read", v->title,
              sector );
        return 0;
      }
    }
  }
  *next_out = next == -2 ? DVD_VOBU_END : (uint32_t)next;
  *len_out  = ea + 1;
  if( ea < 0x10000 && n->ilvu_ea < 0x10000 && (next == -2 || next - sector < 0x10000000) ) {
    dvd_vobu_rec_t r = { vobu_key( v, sector ), (uint16_t)ea, (uint16_t)n->ilvu_ea,
                         (uint32_t)(n->category & 0x6000) << 16 |
                         (next == -2 ? 0x10000000 : (uint32_t)(next - sector)),
                         n->vobu_s_ptm, n->vobu_e_ptm };
    vobu_insert( vobu_store( v ), &r );
  } else {
    Log3( v->ctx, "VTS_%02d: the VOBU at %" PRIu32 " does not fit a record", v->title, sector );
  }
  return 1;
}

/* ---- VOBU starts ---- */

int DVDSetVobuMap_rec( dvd_reader_t *ctx, int title, const uint32_t *starts, int nb, int trusted )
{
  uint32_t *map = NULL;

  if( !ctx || title < 1 || title > 99 )
    return -1;
  if( nb > 0 && !(map = malloc( nb * sizeof(*map) )) )
    return -1;
  if( nb > 0 )
    memcpy( map, starts, nb * sizeof(*map) );
  free( ctx->vob_css[title].map );
  ctx->vob_css[title].map            = map;
  ctx->vob_css[title].nb_map         = nb > 0 ? nb : 0;
  ctx->vob_css[title].map_distrusted = !trusted;
  return 0;
}

int DVDVobuMapTrusted_rec( dvd_reader_t *ctx, int title )
{
  return ctx && title >= 1 && title <= 99 && !ctx->vob_css[title].map_distrusted;
}

static int map_lower( const dvd_reader_t *ctx, int t, uint32_t sector )
{
  const uint32_t *m = ctx->vob_css[t].map;
  int lo = 0, hi = ctx->vob_css[t].nb_map;
  while( lo < hi ) {
    int mid = (lo + hi) / 2;
    if( m[mid] < sector )
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

static int map_has( const dvd_reader_t *ctx, int t, uint32_t sector )
{
  int at = map_lower( ctx, t, sector );
  return at < ctx->vob_css[t].nb_map && ctx->vob_css[t].map[at] == sector;
}

/* The VOBU after the one at sector by title set t's map. */
static int map_next( const dvd_reader_t *ctx, int t, uint32_t sector, uint32_t *next )
{
  int at;

  if( ctx->vob_css[t].map_distrusted )
    return 0;
  at = map_lower( ctx, t, sector );
  if( at == ctx->vob_css[t].nb_map || ctx->vob_css[t].map[at] != sector || at + 1 == ctx->vob_css[t].nb_map )
    return 0;
  *next = ctx->vob_css[t].map[at + 1];
  return 1;
}

/* Title set o's title VOBs hold title set t's (same place, or around it)
 * and o's map is trusted. */
static int shares_title_vobs( const dvd_reader_t *ctx, int t, int o, uint32_t t_blocks )
{
  int64_t tb = ctx->vob_css[t].start, ob = ctx->vob_css[o].start;

  return o != t && !ctx->vob_css[o].map_distrusted && ob && tb &&
         (ob == tb || (ob <= tb && tb + t_blocks <= ob + ctx->vob_css[o].blocks));
}

/* sector starts a VOBU by its navigation pack (recorded, else read). */
static int start_by_nav( dvd_vob_reader_t *v, uint32_t sector )
{
  const dvd_vobu_rec_t *r = DVDFindVobu_rec( v, sector, NULL );
  dvd_vobu_nav_t n;
  uint32_t next, len;

  if( r )
    return r->next != DVD_VOBU_NO_NAV;
  if( !DVDReadVobuNav_rec( v, sector, &n ) )
    return 0;
  DVDRegisterVobu_rec( v, &n, sector, 1, &next, &len );
  return 1;
}

int DVDVobuStart_rec( dvd_vob_reader_t *v, uint32_t sector )
{
  dvd_reader_t *ctx = v->ctx;

  if( !ctx->vob_css[v->title].map_distrusted ) {
    if( map_has( ctx, v->title, sector ) )
      return 1;
    char ifo[16];
    snprintf( ifo, sizeof(ifo), "VTS_%02d_0.IFO", v->title );
    vobu_report( v, DVD_REPORT_WARN, ifo, "VTS_VOBU_ADMAP", sector, "title VOB sector %" PRIu32 " starts no VOBU "
                 "of the VOBU address map: its VOBU starts are not trusted (the VOBs must be scanned)", sector );
    ctx->vob_css[v->title].map_distrusted = 1;
    return start_by_nav( v, sector );
  }
  if( ctx->vob_css[v->title].start ) {
    /* title sets sharing these title VOBs: two or more maps agreeing are
     * enough (the sector asked as it is, relative to each one's own VOBs) */
    int hits = 0;
    for( int o = 1; o < 100; o++ ) {
      if( !shares_title_vobs( ctx, v->title, o, v->blocks ) )
        continue;
      if( !map_has( ctx, o, sector ) )
        return start_by_nav( v, sector );
      hits++;
    }
    if( hits >= 2 )
      return 1;
  }
  return start_by_nav( v, sector );
}

int DVDNextVobu_rec( dvd_vob_reader_t *v, uint32_t sector, uint32_t *next )
{
  dvd_reader_t *ctx = v->ctx;
  const dvd_vobu_rec_t *r;
  dvd_vobu_nav_t n, n2;
  uint32_t nx, nl, after;

  if( !ctx->vob_css[v->title].map_distrusted )
    return map_next( ctx, v->title, sector, next );
  if( ctx->vob_css[v->title].start ) {
    /* the maps of title sets sharing these title VOBs, when two or more agree */
    int64_t tb = ctx->vob_css[v->title].start;
    int hits = 0, agree = 1;
    uint32_t cand = 0;
    for( int o = 1; o < 100 && agree; o++ ) {
      int64_t ob = ctx->vob_css[o].start;
      uint32_t on;
      if( !shares_title_vobs( ctx, v->title, o, v->blocks ) )
        continue;
      if( !map_next( ctx, o, (uint32_t)(sector - ob + tb), &on ) ) {
        agree = 0;
        break;
      }
      on = (uint32_t)(on + ob - tb);
      if( hits && on != cand )
        agree = 0;
      cand = on;
      hits++;
    }
    if( agree && hits >= 2 ) {
      *next = cand;
      return 1;
    }
  }
  /* the navigation packs: the record of sector, else sector and the VOBU
   * after it read */
  if( (r = DVDFindVobu_rec( v, sector, NULL )) ) {
    if( r->next == DVD_VOBU_NO_NAV )
      return 0;
    *next = sector + 1 + r->vobu_ea;
    return 1;
  }
  if( !DVDReadVobuNav_rec( v, sector, &n ) )
    return 0;
  after = sector + n.vobu_ea + 1;
  if( !DVDReadVobuNav_rec( v, after, &n2 ) )
    return 0;
  *next = after;
  DVDRegisterVobu_rec( v, &n, sector, 1, &nx, &nl );
  DVDRegisterVobu_rec( v, &n2, after, 1, &nx, &nl );
  return 1;
}

int DVDVobuBad_rec( dvd_vob_reader_t *v, uint32_t sector )
{
  const dvd_vobu_rec_t *r;
  uint8_t b[SECTOR];
  dvd_vobu_nav_t n;
  uint32_t next, len;

  if( sector >= v->blocks )
    return 1;
  if( (r = DVDFindVobu_rec( v, sector, NULL )) )
    return r->next == DVD_VOBU_NO_NAV;
  if( read_sector( v, sector, b ) < 0 || !DVDIsNavPack_rec( b ) ) {
    DVDMarkVobuBad_rec( v, sector );
    return 1;
  }
  parse_nav( v, b, &n );
  DVDRegisterVobu_rec( v, &n, sector, 1, &next, &len );
  return 0;
}

/* ---- for the cell rules (the DVD part's, until the title VOBs' reading
 * leaves too) ---- */

uint32_t DVDTitleVOBBlocks_rec( const dvd_vob_reader_t *v )
{
  return v ? v->blocks : 0;
}

int DVDVobuMap_rec( const dvd_vob_reader_t *v, const uint32_t **map, int *nb )
{
  const dvd_reader_t *ctx = v->ctx;

  *map = ctx->vob_css[v->title].map;
  *nb  = ctx->vob_css[v->title].nb_map;
  return !ctx->vob_css[v->title].map_distrusted;
}

void DVDCloseTitleVOBs_rec( dvd_vob_reader_t *v )
{
  if( !v )
    return;
  end_damage( v, NULL );
  for( int i = 0; i < MAX_PARTS; i++ ) {
    if( v->parts[i].file )
      v->ctx->fs->file_close( v->parts[i].file );
    free( v->parts[i].pieces );
  }
  if( v->image )
    v->ctx->fs->file_close( v->image );
  free( v );
}
