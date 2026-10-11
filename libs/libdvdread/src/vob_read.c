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

/* ---- cells ---- */

#define CELL_FLAGS(c)   ((c)->category[0]) /* block_mode 0xc0, block_type 0x30, seamless_play 8, interleaved 4,
                                              stc_discontinuity 2, seamless_angle 1 */
#define CELL_CMD_NR(c)  ((c)->category[3]) /* cell_cmd_nr */
#define CHECK_SAMPLE    50                 /* VOBUs a full check reads when it does not read them all */
#define SEAMLESS_CHECKED 1                 /* cell state: the seamless-angle check ran ... */
#define SEAMLESS_OK      2                 /* ... and passed */

int DVDKeepPGCIT_rec( dvd_reader_t *ctx, int title, pgcit_rec_t *pgcit )
{
  uint8_t **state = NULL;

  if( !ctx || title < 1 || title > 99 )
    return -1;
  if( pgcit && pgcit->nr_of_pgcs ) {
    if( !(state = calloc( pgcit->nr_of_pgcs, sizeof(*state) )) )
      return -1;
    for( int p = 0; p < pgcit->nr_of_pgcs; p++ )
      if( pgcit->pgcs[p] && pgcit->pgcs[p]->nr_of_cells &&
          !(state[p] = calloc( pgcit->pgcs[p]->nr_of_cells, 1 )) ) {
        while( p-- )
          free( state[p] );
        free( state );
        return -1;
      }
  }
  ctx->vob_css[title].pgcit      = pgcit;
  ctx->vob_css[title].cell_state = state;
  return 0;
}

void DVDSetCellCheck_rec( dvd_reader_t *ctx, dvd_cell_check_t mode, uint64_t seed )
{
  if( !ctx )
    return;
  ctx->cell_check = mode;
  ctx->cell_rng   = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

/* The next number of the random sample (xorshift64*). */
static uint64_t cell_random( dvd_reader_t *ctx )
{
  uint64_t x = ctx->cell_rng ? ctx->cell_rng : 0x9e3779b97f4a7c15ULL;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  ctx->cell_rng = x;
  return x * 0x2545f4914f6cdd1dULL;
}

static const pgc_rec_t *pgc_of( const dvd_vob_reader_t *v, int pgcn )
{
  const pgcit_rec_t *t = v->ctx->vob_css[v->title].pgcit;
  return t && pgcn >= 0 && pgcn < t->nr_of_pgcs ? t->pgcs[pgcn] : NULL;
}

static const cell_rec_t *cell_of( const dvd_vob_reader_t *v, int pgcn, int cell )
{
  const pgc_rec_t *p = pgc_of( v, pgcn );
  return p && cell >= 0 && cell < p->nr_of_cells ? &p->cells[cell] : NULL;
}

static uint32_t bcd_seconds( uint32_t t )
{
#define BCD(x) (((x) >> 4) * 10 + ((x) & 15))
  return BCD( t >> 24 & 0xff ) * 3600 + BCD( t >> 16 & 0xff ) * 60 + BCD( t >> 8 & 0xff );
#undef BCD
}

int DVDCellNavNextVobu_rec( dvd_vob_reader_t *v, int pgcn, int cell, uint32_t s, uint32_t *next, uint32_t *len )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  const dvd_vobu_rec_t *r;
  dvd_vobu_nav_t n;

  if( !c )
    return 0;
  if( !(r = DVDFindVobu_rec( v, s, NULL )) ) {
    if( !DVDReadVobuNav_rec( v, s, &n ) || !DVDRegisterVobu_rec( v, &n, s, 1, next, len ) ) {
      Log3( v->ctx, "VTS_%02d: no VOBU at sector %" PRIu32, v->title, s );
      return 0;
    }
    if( *next == DVD_VOBU_SELF ) {
      if( c->last_vobu_start_sector == s ) {
        *next = DVD_VOBU_END;
      } else {
        Log3( v->ctx, "VTS_%02d: the VOBU at %" PRIu32 " names itself as the next one", v->title, s );
        *next = s + *len;
      }
    }
    return 1;
  }
  if( r->next == DVD_VOBU_NO_NAV )
    return 0;
  *next = r->next & 0x10000000 ? DVD_VOBU_END : (r->next & 0xfffffff) + s;
  *len  = r->vobu_ea + 1;
  return 1;
}

int DVDCellSeamlessAngleOk_rec( dvd_vob_reader_t *v, int pgcn, int cell )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  uint8_t *state;
  uint32_t lvs, s, n, l;
  int ok = 0;

  if( !c || (CELL_FLAGS( c ) & 0xf4) )
    return 0;
  state = &v->ctx->vob_css[v->title].cell_state[pgcn][cell];
  if( *state & SEAMLESS_CHECKED )
    return !!(*state & SEAMLESS_OK);
  lvs = c->last_vobu_start_sector;
  s   = c->first_sector;
  for( int k = 0; k < 5; k++ ) {
    if( !DVDCellNavNextVobu_rec( v, pgcn, cell, s, &n, &l ) || n != s + l )
      goto end;
    s = n;
    if( s >= lvs )
      break;
  }
  ok = DVDCellNavNextVobu_rec( v, pgcn, cell, lvs, &n, &l );
end:
  if( !ok )
    Log3( v->ctx, "VTS_%02d: a seamless angle cell's VOBUs do not follow each other", v->title );
  *state |= SEAMLESS_CHECKED | (ok ? SEAMLESS_OK : 0);
  return ok;
}

int DVDCellNextVobu_rec( dvd_vob_reader_t *v, int pgcn, int cell, uint32_t s, uint32_t *next, uint32_t *len )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  int trusted;
  uint32_t n;

  if( !c )
    return 0;
  if( !DVDVobuStart_rec( v, s ) ) {
    Log3( v->ctx, "VTS_%02d: sector %" PRIu32 " starts no VOBU", v->title, s );
    return 0;
  }
  trusted = DVDVobuMapTrusted_rec( v->ctx, v->title );
  if( s < c->last_vobu_start_sector && !(CELL_FLAGS( c ) & 0xf4) ) {
    int ok;
    if( !(CELL_FLAGS( c ) & 1) )
      ok = DVDNextVobu_rec( v, s, &n );
    else
      ok = trusted && DVDCellSeamlessAngleOk_rec( v, pgcn, cell ) && DVDNextVobu_rec( v, s, &n );
    if( ok && DVDVobuMapTrusted_rec( v->ctx, v->title ) ) {
      *next = n;
      *len  = n - s;
      return 1;
    }
  }
  return DVDCellNavNextVobu_rec( v, pgcn, cell, s, next, len );
}

int DVDCellVobuTimes_rec( dvd_vob_reader_t *v, int pgcn, int cell, uint32_t s, uint32_t *sp, uint32_t *ep,
                          int read_anyway )
{
  const dvd_vobu_rec_t *r = DVDFindVobu_rec( v, s, NULL );
  dvd_vobu_nav_t n;
  uint32_t next, len;

  if( !r && !read_anyway && DVDCellRejected_rec( v, pgcn, cell, 1, cell ? cell - 1 : -1 ) &&
      DVDCellCheckFailed_rec( v, pgcn, cell ) )
    return 0;
  if( !r )
    r = DVDFindVobu_rec( v, s, NULL );
  if( r ) {
    if( r->next == DVD_VOBU_NO_NAV )
      return 0;
    *sp = r->vobu_s_ptm;
    *ep = r->vobu_e_ptm;
    return 1;
  }
  if( !DVDReadVobuNav_rec( v, s, &n ) )
    return 0;
  *sp = n.vobu_s_ptm;
  *ep = n.vobu_e_ptm;
  DVDRegisterVobu_rec( v, &n, s, 1, &next, &len );
  return 1;
}

int DVDCellDuration_rec( dvd_vob_reader_t *v, int pgcn, int cell, uint64_t *dur, int read_anyway )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  uint32_t s1, e1, s2, e2;

  if( !c || !DVDCellVobuTimes_rec( v, pgcn, cell, c->first_sector, &s1, &e1, read_anyway ) ||
      !DVDCellVobuTimes_rec( v, pgcn, cell, c->last_vobu_start_sector, &s2, &e2, read_anyway ) )
    return 0;
  if( s1 > e1 || e2 < s2 || e2 < e1 )
    return 0;
  *dur = e2 - s1;
  return 1;
}

/* A VOBU of the cell recorded as no navigation pack. */
static int cell_has_bad_vobu( dvd_vob_reader_t *v, const cell_rec_t *c )
{
  uint32_t s = c->first_sector;
  do {
    int64_t nk;
    const dvd_vobu_rec_t *r = DVDFindVobu_rec( v, s, &nk );
    if( r && r->next == DVD_VOBU_NO_NAV )
      return 1;
    s = (uint32_t)nk;
  } while( s <= c->last_vobu_start_sector );
  return 0;
}

static int cmp_u32( const void *a, const void *b )
{
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

int DVDCellCheckFailed_rec( dvd_vob_reader_t *v, int pgcn, int cell )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  dvd_cell_check_t mode = (dvd_cell_check_t)v->ctx->cell_check;
  uint32_t *list = NULL, s, s1, e1, s2, e2;
  int nb = 0, alloc = 0, failed = 1;

  if( !c || c->last_vobu_start_sector < c->first_sector || cell_has_bad_vobu( v, c ) )
    return 1;
  if( DVDVobuBad_rec( v, c->first_sector ) ||
      (c->last_vobu_start_sector != c->first_sector && DVDVobuBad_rec( v, c->last_vobu_start_sector )) )
    return 1;
  for( s = c->first_sector; s < c->last_vobu_start_sector; ) {
    if( nb == alloc ) {
      uint32_t *l = realloc( list, (alloc = alloc ? 2 * alloc : 16) * sizeof(*l) );
      if( !l )
        goto end;
      list = l;
    }
    list[nb++] = s;
    if( !DVDNextVobu_rec( v, s, &s ) )
      goto end;
  }
  if( nb < CHECK_SAMPLE || mode == DVD_CHECK_ALL ) {
    for( int k = 0; k < nb; k++ )
      if( DVDVobuBad_rec( v, list[k] ) )
        goto end;
  } else if( mode == DVD_CHECK_SPREAD ) {
    for( int k = 0; k < CHECK_SAMPLE; k++ )
      if( DVDVobuBad_rec( v, list[(int64_t)k * nb / CHECK_SAMPLE] ) )
        goto end;
  } else {
    uint32_t pick[CHECK_SAMPLE];
    int left = nb;
    for( int k = 0; k < CHECK_SAMPLE; k++ ) {
      int at = (int)(cell_random( v->ctx ) % (uint64_t)left);
      pick[k] = list[at];
      memmove( list + at, list + at + 1, (left - at - 1) * sizeof(*list) );
      left--;
    }
    qsort( pick, CHECK_SAMPLE, sizeof(*pick), cmp_u32 );
    for( int k = 0; k < CHECK_SAMPLE; k++ )
      if( DVDVobuBad_rec( v, pick[k] ) )
        goto end;
  }
  if( DVDCellVobuTimes_rec( v, pgcn, cell, c->first_sector, &s1, &e1, 1 ) &&
      DVDCellVobuTimes_rec( v, pgcn, cell, c->last_vobu_start_sector, &s2, &e2, 1 ) && s1 <= e1 )
    failed = !(e1 <= e2 && s2 <= e2);
end:
  Log3( v->ctx, "cell check, VTS_%02d program chain %d cell %d (%d VOBUs, %s): %s", v->title, pgcn + 1, cell + 1,
        nb, nb < CHECK_SAMPLE || mode == DVD_CHECK_ALL ? "all read" :
        mode == DVD_CHECK_SPREAD ? "50 evenly spread" : "50 at random", failed ? "failed" : "passed" );
  free( list );
  return failed;
}

/* A long single cell made of tiny VOBUs of one size: filler. */
static int single_cell_fake( dvd_vob_reader_t *v, const cell_rec_t *c )
{
  uint32_t first = c->first_sector, n, s, size;
  unsigned count = 0, steps = 0;

  if( bcd_seconds( c->playback_time ) <= 1799 || first == c->last_vobu_start_sector )
    return 0;
  if( !DVDNextVobu_rec( v, first, &n ) )
    return 1;
  size = n - first;
  if( size >= 12 )
    return 0;
  s = n;
  if( s == c->last_vobu_start_sector || s > c->last_sector )
    return 0;
  do {
    if( !DVDNextVobu_rec( v, s, &s ) )
      return 1;
    count++;
    steps += 9;
  } while( s != c->last_vobu_start_sector && s <= c->last_sector );
  if( count < 1000 )
    return 0;
  s = first;
  for( unsigned k = steps / 10; k; k-- ) {
    if( !DVDNextVobu_rec( v, s, &n ) )
      break;
    if( n - s != size )
      return 0;
    s = n;
  }
  return 1;
}

int DVDCellRejected_rec( dvd_vob_reader_t *v, int pgcn, int cell, int quick, int prev )
{
  const pgc_rec_t *p = pgc_of( v, pgcn );
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  uint32_t t;
  uint64_t dur;

  if( !c || c->first_sector > c->last_vobu_start_sector || cell_has_bad_vobu( v, c ) )
    return 1;
  if( !(c->playback_time & 0xffffff3f) )
    return 1;
  if( c->last_vobu_start_sector >= v->blocks || c->last_sector >= v->blocks || c->first_sector >= v->blocks )
    return 1;
  t = c->playback_time & 0xffffff00;
  if( t <= 0x100 && !quick ) {
    if( c->first_sector == c->last_vobu_start_sector ) {
      if( prev >= 0 && prev + 1 == cell && !CELL_CMD_NR( c ) && prev + 2 != p->nr_of_cells &&
          !DVDCellCheckFailed_rec( v, pgcn, cell ) )
        return 0;
      return 1;
    }
    if( CELL_CMD_NR( c ) )
      return 1;
  } else if( t <= 0x500 && CELL_CMD_NR( c ) ) {
    return 1;
  }
  if( p->nr_of_cells == 1 && single_cell_fake( v, c ) )
    return 1;
  if( (bcd_seconds( c->playback_time ) < 1801 && t > 0x500) || quick || !DVDCellCheckFailed_rec( v, pgcn, cell ) ) {
    if( quick )
      return 0;
    return !DVDCellDuration_rec( v, pgcn, cell, &dur, 1 );
  }
  return 1;
}

int DVDIlvuWalk_rec( dvd_vob_reader_t *v, uint32_t from, uint32_t to )
{
  dvd_reader_t *ctx = v->ctx;

  if( to < from )
    return 0;
  while( from != to ) {
    const dvd_vobu_rec_t *r;
    uint32_t u, next, len;
    if( to < from || !DVDVobuStart_rec( v, from ) )
      return 0;
    if( !(r = DVDFindVobu_rec( v, from, NULL )) ) {
      dvd_vobu_nav_t n;
      if( !DVDReadVobuNav_rec( v, from, &n ) || !DVDRegisterVobu_rec( v, &n, from, 0, &next, &len ) ||
          (~n.category & 0x6000) || n.ilvu_ea > 0x7ffffffe )
        return 0;
      u = n.ilvu_ea;
    } else {
      if( ~r->next & 0x60000000 )
        return 0;
      u = r->ilvu_ea;
    }
    from = from + 1 + u;
    if( !ctx->vob_css[v->title].map_distrusted ) {
      /* the last VOBU of the unit just crossed, by the map: recorded */
      int at = map_lower( ctx, v->title, from );
      if( at < ctx->vob_css[v->title].nb_map && ctx->vob_css[v->title].map[at] == from && at ) {
        uint32_t q = ctx->vob_css[v->title].map[at - 1];
        dvd_vobu_nav_t n;
        if( !DVDFindVobu_rec( v, q, NULL ) && DVDReadVobuNav_rec( v, q, &n ) )
          DVDRegisterVobu_rec( v, &n, q, 1, &next, &len );
      }
    }
  }
  return 1;
}

/* ---- the program chain check ---- */

#define BLOCK_MODE(f)   ((f) >> 6)

/* Two cells of an interleaved angle block lie as angles do. */
static int pair_ok( const cell_rec_t *p, const cell_rec_t *q )
{
  const cell_rec_t *lo, *hi;
  uint32_t end;

  if( p->first_sector == q->first_sector && p->last_vobu_start_sector == q->last_vobu_start_sector &&
      p->last_sector == q->last_sector )
    return 1;
  if( (p->first_sector < q->first_sector && q->first_sector < p->last_vobu_start_sector) ||
      (q->first_sector < p->first_sector && p->first_sector < q->last_vobu_start_sector) )
    return 1;
  if( p->last_sector == p->first_ilvu_end_sector && q->last_sector == q->first_ilvu_end_sector )
    return 1;
  lo  = p->first_sector <= q->first_sector ? p : q;
  hi  = lo == p ? q : p;
  end = lo->last_sector > lo->first_ilvu_end_sector ? lo->last_sector : lo->first_ilvu_end_sector;
  if( p->last_sector < p->first_ilvu_end_sector ) {
    if( q->last_sector < q->first_ilvu_end_sector && end + 1 <= hi->first_sector )
      return 1;
    return 0;
  }
  return !p->first_ilvu_end_sector && !q->first_ilvu_end_sector && end + 1 <= hi->first_sector;
}

/* An interleaved angle block starting at cell x whose cells' places agree. */
static int angle_block_ok( const pgc_rec_t *p, int x )
{
  int n = 1, k = x;

  for( ;; ) {
    uint8_t g;
    if( ++k >= p->nr_of_cells || (CELL_FLAGS( &p->cells[k] ) & 0x35) != 0x14 )
      return 0;
    g = BLOCK_MODE( CELL_FLAGS( &p->cells[k] ) );
    n++;
    if( g == 3 )
      break;
    if( g != 2 )
      return 0;
  }
  for( int a = 0; a < n; a++ )
    for( int b = a + 1; b < n; b++ )
      if( !pair_ok( &p->cells[x + a], &p->cells[x + b] ) )
        return 0;
  return 1;
}

/* The cell of an interleaved angle block lying first (highest 0) / last
 * (highest 1) on the disc. */
static int angle_block_extreme( dvd_vob_reader_t *v, const pgc_rec_t *p, int i, int highest )
{
  uint8_t f = CELL_FLAGS( &p->cells[i] );
  int first, last, best = i;

  if( (f & 0x34) != 0x14 )
    return i;
  if( BLOCK_MODE( f ) == 1 ) {
    first = i;
    for( last = i + 1; last < p->nr_of_cells && BLOCK_MODE( CELL_FLAGS( &p->cells[last] ) ) == 2; last++ )
      ;
    if( last >= p->nr_of_cells || BLOCK_MODE( CELL_FLAGS( &p->cells[last] ) ) != 3 ) {
      Log3( v->ctx, "VTS_%02d program chain %d: the angle block of cell %d has no end", v->title, p->number + 1,
            i + 1 );
      return i;
    }
  } else if( BLOCK_MODE( f ) == 3 ) {
    last = i;
    for( first = i - 1; first >= 0 && BLOCK_MODE( CELL_FLAGS( &p->cells[first] ) ) == 2; first-- )
      ;
    if( first < 0 || BLOCK_MODE( CELL_FLAGS( &p->cells[first] ) ) != 1 ) {
      Log3( v->ctx, "VTS_%02d program chain %d: the angle block of cell %d has no start", v->title, p->number + 1,
            i + 1 );
      return i;
    }
  } else {
    Log3( v->ctx, "VTS_%02d program chain %d: cell %d is inside an angle block", v->title, p->number + 1, i + 1 );
    return i;
  }
  for( int k = first; k <= last; k++ )
    if( highest ? p->cells[k].first_sector > p->cells[best].first_sector
                : p->cells[k].first_sector < p->cells[best].first_sector )
      best = k;
  return best;
}

int DVDPgcRejected_rec( dvd_vob_reader_t *v, int pgcn, int ov )
{
  const pgc_rec_t *p = pgc_of( v, pgcn );
  int n;

  if( !p || !(n = p->nr_of_cells) )
    return 0;
  if( ov && n >= 2 ) {
    int all = 1, n2 = n, mono = 1;
    for( int k = 1; k < n && all; k++ )
      all = CELL_FLAGS( &p->cells[k] ) & 2;
    if( all && n >= 12 )
      return 1;
    if( n > 3 && (DVDCellRejected_rec( v, pgcn, n - 1, 0, n - 2 ) ||
                  ((p->cells[n - 1].playback_time & 0xffffff00) < 0x301 &&
                   p->cells[n - 1].vob_id_nr != p->cells[n - 2].vob_id_nr && p->cells[n - 1].cell_nr == 1)) )
      n2 = n - 1;
    for( int k = 1; k < n2 && mono; k++ )
      mono = p->cells[k - 1].first_sector <= p->cells[k].first_sector;
    if( all && !mono && n >= 7 )
      return 2;
  }
  if( n >= 4 ) {
    uint32_t secs = 0, sectors = 0;
    for( int k = 0; k < n; k++ ) {
      if( !DVDCellRejected_rec( v, pgcn, k, 1, k - 1 ) )
        secs += bcd_seconds( p->cells[k].playback_time );
      sectors += p->cells[k].last_sector - p->cells[k].first_sector + 1;
    }
    if( !secs )
      return 3;
    if( sectors / secs < 50 )
      return 4;
  }
  if( !p->nr_of_programs )
    return -1;
  if( !p->program_map[0] || p->program_map[0] > n )
    return 5;
  for( int k = 1; k < p->nr_of_programs; k++ ) {
    if( !p->program_map[k] || p->program_map[k] > n )
      return 5;
    if( p->program_map[k] < p->program_map[k - 1] )
      return 6;
  }
  if( p->nr_of_programs < 3 )
    return -1;
  /* programs 2.. : seamless cells without cell commands follow each other on the disc */
  for( int pg = 1; pg < p->nr_of_programs; pg++ ) {
    for( int a = p->program_map[pg] - 1, b = a + 1; b < n; a++, b++ ) {
      uint8_t fa = CELL_FLAGS( &p->cells[a] ), fb = CELL_FLAGS( &p->cells[b] ), fx;
      int x = a, y = b;
      if( !(fb & 8) || CELL_CMD_NR( &p->cells[a] ) || CELL_CMD_NR( &p->cells[b] ) )
        break;
      fx = fa;
      if( !((fa | fb) & 1) ) {
        int join;
        if( !(fa & 4) )
          join = !(fa & 0x30) && fa < 0x40 &&
                 ((!(fb & 4) && !(fb & 0x30) && fb < 0x40) ||
                  ((fb & 4) && (fb & 0x30) == 0x10 && (fb & 0xc0) == 0x40));
        else
          join = (fa & 0x30) == 0x10 && fa >= 0xc0 && !(fb & 4) && !(fb & 0x30) && fb <= 0x3f;
        if( join ) {
          x = angle_block_extreme( v, p, a, 1 );
          y = angle_block_extreme( v, p, b, 0 );
          if( p->cells[x].last_sector + 1 != p->cells[y].first_sector )
            return 7;
          fx = CELL_FLAGS( &p->cells[x] );
        }
      }
      if( fx < 0x10 &&
          ((fx & 4) ? (CELL_FLAGS( &p->cells[y] ) & 0xf4) == 0 : (CELL_FLAGS( &p->cells[y] ) & 0xf4) == 4) &&
          p->cells[x].last_sector + 1 != p->cells[y].first_sector &&
          !DVDIlvuWalk_rec( v, p->cells[x].last_sector + 1, p->cells[y].first_sector ) )
        return 8;
      if( ov && (CELL_FLAGS( &p->cells[x] ) & 0xf5) == 0x54 && !angle_block_ok( p, x ) )
        return 9;
    }
  }
  return -1;
}

/* ---- a title's cell list ---- */

#define BLOCK_TYPE(f)   ((f) >> 4 & 3)

int DVDAngleCell_rec( dvd_vob_reader_t *v, int pgcn, int i, int angle, int angles )
{
  const pgc_rec_t *p = pgc_of( v, pgcn );
  uint8_t f;
  int k = i;

  if( !p || i < 0 || i >= p->nr_of_cells )
    return -1;
  f = CELL_FLAGS( &p->cells[i] );
  if( !BLOCK_TYPE( f ) )
    return i;
  if( BLOCK_TYPE( f ) != 1 || BLOCK_MODE( f ) != 1 )
    Log3( v->ctx, "VTS_%02d program chain %d cell %d: an angle cell that does not start an angle block", v->title,
          pgcn + 1, i + 1 );
  for( ;; ) {
    uint8_t g = CELL_FLAGS( &p->cells[k] );
    if( BLOCK_MODE( g ) == 3 ) {
      int size = k - i + 1, pick = angle < size - 1 ? angle : size - 1;
      if( size != angles )
        Log1( v->ctx, "VTS_%02d program chain %d: the angle block at cell %d has %d cells, the title %d angles",
              v->title, pgcn + 1, i + 1, size, angles );
      return i + pick < p->nr_of_cells ? i + pick : -1;
    }
    if( BLOCK_TYPE( g ) != 1 ) {
      Log1( v->ctx, "VTS_%02d program chain %d: broken angle block around cell %d", v->title, pgcn + 1, k + 1 );
      return -1;
    }
    if( k + 1 >= p->nr_of_cells )
      break;
    k++;
  }
  Log1( v->ctx, "VTS_%02d program chain %d: broken angle block around cell %d", v->title, pgcn + 1, i + 1 );
  return -1;
}

/* The cell after cell i (after its whole angle block), -1 when none. */
static int next_cell( dvd_vob_reader_t *v, const pgc_rec_t *p, int pgcn, int i )
{
  int k = i;

  if( !BLOCK_TYPE( CELL_FLAGS( &p->cells[i] ) ) )
    return i + 1 < p->nr_of_cells ? i + 1 : -1;
  for( ;; ) {
    uint8_t g = CELL_FLAGS( &p->cells[k] );
    if( BLOCK_MODE( g ) == 3 )
      return k + 1 < p->nr_of_cells ? k + 1 : -1;
    if( BLOCK_TYPE( g ) != 1 ) {
      Log1( v->ctx, "VTS_%02d program chain %d: broken angle block around cell %d", v->title, pgcn + 1, k + 1 );
      return -1;
    }
    if( k + 1 >= p->nr_of_cells )
      break;
    k++;
  }
  Log1( v->ctx, "VTS_%02d program chain %d: broken angle block around cell %d", v->title, pgcn + 1, i + 1 );
  return -1;
}

int DVDCellList_rec( dvd_vob_reader_t *v, int pgcn, int first, int last, int angle, int angles, int *cells, int *nb )
{
  const pgc_rec_t *p = pgc_of( v, pgcn );
  int c = first;

  *nb = 0;
  if( !p )
    return 0;
  for( ;; ) {
    int e;
    if( c > last )
      return 1;
    if( (e = DVDAngleCell_rec( v, pgcn, c, angle, angles )) < 0 )
      return 0;
    cells[(*nb)++] = e;
    if( (c = next_cell( v, p, pgcn, e )) < 0 )
      return 1;
  }
}

typedef struct { uint8_t first, last; } cell_run_t;

/* A run kept at an end: one of its cells accepted (quick check) and 5
 * seconds (stopping at both); -1 when its cells are not the chain's. */
static int run_good( dvd_vob_reader_t *v, const pgc_rec_t *p, int pgcn, cell_run_t r )
{
  int all_rejected = 1;
  uint32_t sum = 0;

  if( (unsigned)(r.first - 1) >= p->nr_of_cells || (unsigned)(r.last - 1) >= p->nr_of_cells )
    return -1;
  for( int x = r.first - 1;; x++ ) {
    if( !DVDCellRejected_rec( v, pgcn, x, 1, -1 ) )
      all_rejected = 0;
    sum += bcd_seconds( p->cells[x].playback_time );
    if( x == r.last - 1 || (!all_rejected && sum >= 5) )
      break;
  }
  return !all_rejected && sum > 4;
}

static uint32_t run_seconds( const pgc_rec_t *p, cell_run_t r )
{
  uint32_t sum = 0;
  for( int x = r.first - 1; x <= r.last - 1; x++ )
    sum += bcd_seconds( p->cells[x].playback_time );
  return sum;
}

int DVDCellWalk_rec( dvd_vob_reader_t *v, int pgcn, const uint8_t *played, int nb_played, int angle, int angles,
                     int *cells, int *nb, int *first, int *last )
{
  const pgc_rec_t *p = pgc_of( v, pgcn );
  cell_run_t *runs;
  int n = 0, ret;

  *nb = 0;
  if( !p )
    return 0;
  if( !nb_played ) {
    Log3( v->ctx, "VTS_%02d program chain %d: the navigation played no cell", v->title, pgcn + 1 );
    return 0;
  }
  if( !(runs = malloc( nb_played * sizeof(*runs) )) )
    return -1;
  for( int k = 0; k < nb_played; k++ ) {
    uint8_t c = played[k];
    if( n && runs[n - 1].last + 1 == c )
      runs[n - 1].last = c;
    else
      runs[n++] = (cell_run_t){ c, c };
  }
  /* the start, then the end: runs without an accepted cell or 5 seconds */
  while( n > 1 && run_good( v, p, pgcn, runs[0] ) <= 0 )
    memmove( runs, runs + 1, --n * sizeof(*runs) );
  while( n > 1 && run_good( v, p, pgcn, runs[n - 1] ) <= 0 )
    n--;
  /* the longer end */
  while( n > 1 ) {
    if( run_seconds( p, runs[0] ) < run_seconds( p, runs[n - 1] ) )
      memmove( runs, runs + 1, --n * sizeof(*runs) );
    else
      n--;
  }
  if( (unsigned)(runs[0].first - 1) < p->nr_of_cells && (unsigned)(runs[0].last - 1) < p->nr_of_cells ) {
    *first = runs[0].first - 1;
    *last  = runs[0].last - 1;
    ret = DVDCellList_rec( v, pgcn, *first, *last, angle, angles, cells, nb );
  } else {
    ret = 0;
  }
  free( runs );
  return ret;
}

/* ---- CellTrim: the cell list without the navigation ---- */

typedef struct {
  dvd_vob_reader_t *v;
  const pgc_rec_t  *p;
  int               pgcn, angle, angles;
  int              *cells, *nb;
  dvd_cell_trim_t  *res;
} trim_t;

/* The cell command cmd_nr (from 1) of the chain may jump away: a Goto /
 * Link / Jump that is not a plain "go on" (DVD-Video command groups: link,
 * jump and set-with-link instructions). */
static int cmd_jumps( const pgc_rec_t *p, unsigned cmd_nr )
{
  const uint8_t *c;
  unsigned u, u7, u5, op;
  int b;

  if( !cmd_nr || cmd_nr > p->nr_of_cell || !p->commands )
    return 0;
  c  = p->commands + 8 * (p->nr_of_pre + p->nr_of_post + cmd_nr - 1);
  u  = (unsigned)c[0] << 8 | c[1];
  u7 = u & 0xff0f;
  u5 = u & 0xf0ff;
  if( u7 == 0x2001 && !c[7] )
    return 0;
  if( (u5 == 0x7001 || u5 == 0x6001) && !c[7] )
    return 0;
  if( u7 == 0x2001 || u7 == 0x2005 || u7 == 0x2006 || u7 == 0x2007 || u7 == 0x3008 ) {
    op = c[1] >> 4 & 7;
    if( !(c[1] & 0x80) )
      b = !((op == 3 || op == 5 || op == 7) && c[5] == c[3]);
    else
      b = op != 1 || (c[4] << 8 | c[5]) != 0;
  } else {
    b = 0;
  }
  if( u5 == 0x6001 || (u5 >= 0x6004 && u5 <= 0x6007) || u5 == 0x7001 || (u5 >= 0x7004 && u5 <= 0x7007) )
    return 1;
  return b;
}

/* The run of cells around cell i on one timeline between cell commands
 * that jump away. */
static void segment_around( const pgc_rec_t *p, int i, int *first, int *last )
{
  int k = i;

  while( k && (CELL_FLAGS( &p->cells[k] ) & 0x32) != 2 && !cmd_jumps( p, CELL_CMD_NR( &p->cells[k - 1] ) ) )
    k--;
  *first = k;
  k = i;
  while( !cmd_jumps( p, CELL_CMD_NR( &p->cells[k] ) ) && k + 1 < p->nr_of_cells &&
         (CELL_FLAGS( &p->cells[k + 1] ) & 0x32) != 2 )
    k++;
  *last = k;
}

static uint32_t cells_seconds( const pgc_rec_t *p, int first, int last )
{
  uint32_t sum = 0;
  for( int k = first; k <= last; k++ )
    sum += bcd_seconds( p->cells[k].playback_time );
  return sum;
}

/* From cell b toward cell a, the first cell that is not filler; a when none
 * (simple: the first one passing the full test). */
static int pick_end( trim_t *T, int a, int b, int simple )
{
  const pgc_rec_t *p = T->p;
  int dir = a < b ? 1 : -1, i = a < b ? b - a : a - b;

  if( a == b )
    return a;
  if( simple ) {
    for( int j = i; j >= 1; j-- )
      if( !DVDCellRejected_rec( T->v, T->pgcn, a + dir * j, 0, -1 ) )
        return a + dir * j;
    return a;
  }
  while( i ) {
    int c = a + dir * i, step = 1;
    if( bcd_seconds( p->cells[c].playback_time ) > 2 && !DVDCellRejected_rec( T->v, T->pgcn, c, 0, -1 ) ) {
      if( (p->cells[c].playback_time & 0xffffff00) > 0x3000 || i < 4 ||
          bcd_seconds( p->cells[a + dir * (i - 1)].playback_time ) > 2 ||
          bcd_seconds( p->cells[a + dir * (i - 2)].playback_time ) > 2 )
        return c;
      step = 3;                         /* a short cell after two of at most 2 s: skipped with them */
    }
    i -= step;
  }
  return a;
}

/* The longest run of cells first..last lying in disc order; 1 when it is
 * not mostly fake against the chain's declared length (else, unless quiet,
 * the fake share is given back). */
static int best_run( trim_t *T, int first, int last, int *rf, int *rl, int quiet )
{
  const pgc_rec_t *p = T->p;
  uint32_t best = 0, declared, run, pct;
  int start = first, cur = first;

  for( ;; ) {
    int end = cur, next = -1;
    if( cur != last && cur + 1 >= p->nr_of_cells ) {
      Log3( T->v->ctx, "VTS_%02d program chain %d: cell %d lies after cell %d", T->v->title, T->pgcn + 1,
            first + 1, last + 1 );
    } else if( cur != last ) {
      next = cur + 1;
      if( p->cells[cur].first_sector <= p->cells[next].first_sector ) {
        cur = next;
        continue;
      }
    }
    if( (run = cells_seconds( p, start, end )) > best ) {
      *rf  = start;
      *rl  = end;
      best = run;
    }
    if( next < 0 )
      break;
    start = cur = next;
  }
  if( !best ) {
    if( !quiet )
      T->res->fake_percent = 100;
    return 0;
  }
  declared = bcd_seconds( p->playback_time );
  run = cells_seconds( p, *rf, *rl );
  pct = declared && declared > run ? (uint32_t)((uint64_t)(declared - run) * 100 / declared) : 0;
  if( !declared )
    Log3( T->v->ctx, "VTS_%02d program chain %d declares no length [untested on real discs]", T->v->title,
          T->pgcn + 1 );
  if( pct < 26 )
    return 1;
  if( !quiet )
    T->res->fake_percent = (int)pct;
  return 0;
}

/* Cells first..last as the cell list. */
static int trim_cells( trim_t *T, int first, int last )
{
  T->res->first = first;
  T->res->last  = last;
  return DVDCellList_rec( T->v, T->pgcn, first, last, T->angle, T->angles, T->cells, T->nb );
}

/* First way (see DVDCellTrim_rec()): 1 done, 0 no such shape. */
static int trim_edges( trim_t *T, int nb_chapters )
{
  const pgc_rec_t *p = T->p;
  int n = p->nr_of_cells, F = 0, L = n - 1, gs = -1, ge = -1, ret;

  if( nb_chapters > 4 )
    return 0;
  if( n >= 3 && n <= 5 && DVDCellRejected_rec( T->v, T->pgcn, F, 0, -1 ) &&
      DVDCellRejected_rec( T->v, T->pgcn, L, 0, -1 ) ) {
    int S = 1, E = n - 2, prev = -1, ok;
    if( p->cells[F].vob_id_nr != p->cells[S].vob_id_nr && p->cells[L].vob_id_nr != p->cells[E].vob_id_nr ) {
      int same = p->cells[F].vob_id_nr == p->cells[L].vob_id_nr && p->cells[F].cell_nr == p->cells[L].cell_nr;
      ok = p->cells[F].cell_nr == 1 && p->cells[L].cell_nr == 1 ? 1 : same;
      for( int x = S; ok && x != E; x++ ) {
        if( p->cells[x].vob_id_nr != p->cells[E].vob_id_nr || DVDCellRejected_rec( T->v, T->pgcn, x, 0, prev ) ||
            CELL_CMD_NR( &p->cells[x] ) ) {
          ok = 0;
          break;
        }
        prev = x;
      }
      if( !DVDCellRejected_rec( T->v, T->pgcn, E, 0, prev ) && ok && (ret = trim_cells( T, S, E )) != 0 )
        return ret;
    }
  }
  if( n < 5 || !DVDCellRejected_rec( T->v, T->pgcn, F, 0, -1 ) || !DVDCellRejected_rec( T->v, T->pgcn, L, 0, -1 ) )
    return 0;
  for( int c = F; c <= L; ) {
    int s, e;
    segment_around( p, c, &s, &e );
    if( cells_seconds( p, s, e ) > 9 ) {
      if( gs >= 0 )
        return 0;                       /* two long runs */
      gs = s;
      ge = e;
    }
    if( e == L || e + 1 >= n )
      break;
    c = e + 1;
  }
  if( gs < 0 )
    return 0;
  gs = pick_end( T, ge, gs, 1 );
  return trim_cells( T, gs, pick_end( T, gs, ge, 1 ) );
}

/* The entry cell (index) of the first program starting after cell i (next
 * 1) or of the program holding it (next 0); -1 none. */
static int program_entry( const pgc_rec_t *p, int i, int next )
{
  int k = 1, np = p->nr_of_programs, at;

  if( !np )
    return -1;
  for( ; k <= np; k++ )
    if( p->program_map[k - 1] > i + 1 )
      break;
  at = next ? k : k - 1;                /* the program number, from 1 */
  if( !at || at > np || !p->program_map[at - 1] || p->program_map[at - 1] > p->nr_of_cells )
    return -1;
  return p->program_map[at - 1] - 1;
}

/* Second way (see DVDCellTrim_rec()): 1 done (no cells when it finds none),
 * 0 failed. */
static int trim_runs( trim_t *T, int nb_chapters, int first, int mid )
{
  const pgc_rec_t *p = T->p;
  dvd_vob_reader_t *v = T->v;
  int n = p->nr_of_cells, rf, rl;

  if( nb_chapters < 5 ) {
    int fi = 0, li = n - 1;
    if( !n )
      goto clear;
    while( DVDCellRejected_rec( v, T->pgcn, fi, 0, -1 ) || DVDCellCheckFailed_rec( v, T->pgcn, fi ) )
      if( ++fi > n - 1 )
        goto clear;
    while( DVDCellRejected_rec( v, T->pgcn, li, 0, -1 ) || DVDCellCheckFailed_rec( v, T->pgcn, li ) ) {
      if( li == fi )
        goto clear;
      li--;
    }
    if( best_run( T, fi, li, &rf, &rl, 1 ) )
      return trim_cells( T, rf, rl );
  } else {
    int S, E, A, B, C, D, x, y;
    if( first < 0 || mid < 0 ) {
      Log3( v->ctx, "VTS_%02d program chain %d: a chapter without a cell", v->title, T->pgcn + 1 );
      return 0;
    }
    for( S = mid; S != first; S-- ) {
      if( S - 1 < 0 ) {
        Log3( v->ctx, "VTS_%02d program chain %d: no cell before cell %d", v->title, T->pgcn + 1, S + 1 );
        return 0;
      }
      if( cmd_jumps( p, CELL_CMD_NR( &p->cells[S - 1] ) ) )
        break;
    }
    for( E = mid; E + 1 < n; )
      if( cmd_jumps( p, CELL_CMD_NR( &p->cells[++E] ) ) )
        break;
    if( !best_run( T, S, E, &rf, &rl, 0 ) )
      goto clear;
    if( (A = program_entry( p, S, 1 )) < 0 ) {
      Log1( v->ctx, "VTS_%02d program chain %d: the middle chapter's cells lie in the last program: they cannot be "
            "trimmed [untested on real discs]", v->title, T->pgcn + 1 );
      return 0;
    }
    /* back from A over runs of 30 seconds or more, not past S */
    B = S;
    for( int at = A; at != S; ) {
      int s, e, s2, e2;
      segment_around( p, at, &s, &e );
      B = s;
      if( s == S || s - 1 < 0 )
        break;
      segment_around( p, s - 1, &s2, &e2 );
      if( cells_seconds( p, s2, e2 ) < 30 )
        break;
      B = S;
      if( s2 == S )
        break;
      at = s2;
    }
    /* back from A over cells passing the full check, not past B */
    for( C = A; C != B && !DVDCellCheckFailed_rec( v, T->pgcn, C - 1 ); C-- )
      ;
    /* back from E over short new-timeline cells, not past C */
    for( D = E; D != C && (CELL_FLAGS( &p->cells[D] ) & 0xf2) == 2 && bcd_seconds( p->cells[D].playback_time ) < 2; ) {
      if( !D ) {
        Log3( v->ctx, "VTS_%02d program chain %d: no cell before cell 1", v->title, T->pgcn + 1 );
        return 0;
      }
      D--;
    }
    x = pick_end( T, A, C, 0 );
    if( (y = program_entry( p, D, 0 )) < 0 ) {
      Log1( v->ctx, "VTS_%02d program chain %d: cell %d lies in no program [untested on real discs]", v->title,
            T->pgcn + 1, D + 1 );
      return 0;
    }
    y = pick_end( T, y, D, 0 );
    if( best_run( T, x, y, &rf, &rl, 0 ) )
      return trim_cells( T, rf, rl );
  }
clear:
  *T->nb = 0;
  return 1;
}

int DVDCellTrim_rec( dvd_vob_reader_t *v, int pgcn, int nb_chapters, int first_cell, int middle_cell, int angle,
                     int angles, int *cells, int *nb, dvd_cell_trim_t *result )
{
  trim_t T = { v, pgc_of( v, pgcn ), pgcn, angle, angles, cells, nb, result };
  int ret;

  *nb = 0;
  result->first = result->last = result->fake_percent = -1;
  if( !T.p )
    return 0;
  ret = trim_edges( &T, nb_chapters );
  return ret ? ret : trim_runs( &T, nb_chapters, first_cell, middle_cell );
}

/* ---- a title's segments and chapter places ---- */

static int grow_array( void **p, int *alloc, int n, size_t elem )
{
  void *q;
  if( n < *alloc )
    return 0;
  if( !(q = realloc( *p, (size_t)(*alloc = *alloc ? 2 * *alloc : 16) * elem )) )
    return -1;
  *p = q;
  return 0;
}

static int add_extent( dvd_segment_t *g, uint32_t sector, uint32_t count )
{
  uint32_t logical = g->nb_extents ? g->extents[g->nb_extents - 1].logical + g->extents[g->nb_extents - 1].count : 0;
  if( grow_array( (void **)&g->extents, &g->alloc_extents, g->nb_extents, sizeof(*g->extents) ) < 0 )
    return -1;
  g->extents[g->nb_extents++] = (dvd_extent_t){ logical, sector, count };
  return 0;
}

static int add_segment( dvd_title_segments_t *o )
{
  if( grow_array( (void **)&o->segments, &o->alloc_segments, o->nb_segments, sizeof(*o->segments) ) < 0 )
    return -1;
  memset( &o->segments[o->nb_segments++], 0, sizeof(*o->segments) );
  return 0;
}

static int add_cell_to_runs( dvd_segment_t *g, int cell )
{
  unsigned c = cell + 1;

  if( g->nb_runs ) {
    uint16_t *r = g->runs[g->nb_runs - 1];
    if( c >= r[0] && c <= r[1] )
      return 0;
    if( r[1] + 1u == c ) {
      r[1]++;
      return 0;
    }
  }
  if( grow_array( (void **)&g->runs, &g->alloc_runs, g->nb_runs, sizeof(*g->runs) ) < 0 )
    return -1;
  g->runs[g->nb_runs][0] = g->runs[g->nb_runs][1] = (uint16_t)c;
  g->nb_runs++;
  return 0;
}

/* The start of the VOBU holding the segment's last sector. */
static uint32_t segment_last_vobu( dvd_vob_reader_t *v, int pgcn, const dvd_segment_t *g )
{
  dvd_reader_t *ctx = v->ctx;
  const dvd_extent_t *e = &g->extents[g->nb_extents - 1];
  const cell_rec_t *c = cell_of( v, pgcn, g->last_cell );
  uint32_t x = e->sector + e->count - 1;

  if( !ctx->vob_css[v->title].map_distrusted ) {
    int at = map_lower( ctx, v->title, x );
    if( at < ctx->vob_css[v->title].nb_map && at )
      return ctx->vob_css[v->title].map[at - 1];
  }
  if( c->first_sector <= x && x <= c->last_sector ) {
    uint32_t s = x > c->last_vobu_start_sector ? c->last_vobu_start_sector : c->first_sector, n, l;
    for( ;; ) {
      if( !DVDCellNextVobu_rec( v, pgcn, g->last_cell, s, &n, &l ) )
        break;
      if( x < n )
        return s;
      s = n;
    }
  }
  Log3( ctx, "VTS_%02d program chain %d: the last VOBU of a segment is not found (sector %" PRIu32 ")", v->title,
        pgcn + 1, x );
  return c->last_vobu_start_sector;
}

/* The presentation start / end of the VOBU at s (recorded, else read). */
static int vobu_times( dvd_vob_reader_t *v, uint32_t s, uint32_t *sp, uint32_t *ep )
{
  const dvd_vobu_rec_t *r = DVDFindVobu_rec( v, s, NULL );
  dvd_vobu_nav_t n;
  uint32_t next, len;

  if( r ) {
    if( r->next == DVD_VOBU_NO_NAV )
      return 0;
    *sp = r->vobu_s_ptm;
    *ep = r->vobu_e_ptm;
    return 1;
  }
  if( !DVDReadVobuNav_rec( v, s, &n ) )
    return 0;
  *sp = n.vobu_s_ptm;
  *ep = n.vobu_e_ptm;
  DVDRegisterVobu_rec( v, &n, s, 1, &next, &len );
  return 1;
}

/* From the segment's first VOBU's start to the end of the VOBU at s (90 kHz). */
static int segment_time( dvd_vob_reader_t *v, const dvd_segment_t *g, uint32_t s, int64_t *time )
{
  uint32_t s1, e1, s2, e2;

  *time = 0;
  if( !g->nb_extents || g->extents[0].sector == s )
    return 1;
  if( !vobu_times( v, g->extents[0].sector, &s1, &e1 ) ) {
    Log3( v->ctx, "VTS_%02d: no navigation pack at sector %" PRIu32, v->title, g->extents[0].sector );
    return 0;
  }
  if( !vobu_times( v, s, &s2, &e2 ) ) {
    Log3( v->ctx, "VTS_%02d: no navigation pack at sector %" PRIu32, v->title, s );
    return 0;
  }
  if( e1 < s1 || s2 < e1 || e2 < s2 )
    return 0;
  *time = e2 - s1;
  return 1;
}

/* A VOBU of the cell that needs reading one by one (angle blocks,
 * interleaved cells, a cell's last VOBU). */
static int vobu_needs_scan( dvd_vob_reader_t *v, int pgcn, int cell, uint32_t s )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  uint32_t n;

  if( s < c->last_vobu_start_sector && !(CELL_FLAGS( c ) & 0xf4) ) {
    if( CELL_FLAGS( c ) & 1 )
      return !(DVDVobuMapTrusted_rec( v->ctx, v->title ) && DVDCellSeamlessAngleOk_rec( v, pgcn, cell ) &&
               DVDNextVobu_rec( v, s, &n ));
    DVDNextVobu_rec( v, s, &n );
    return 0;
  }
  return 1;
}

/* The navigation is invalid inside a cell: where (the segments' spans so
 * far), recorded. 0, < 0 the segments' times are broken. */
static int nav_invalid( dvd_vob_reader_t *v, int pgcn, dvd_title_segments_t *o )
{
  uint64_t pos = 0;

  for( int k = 0; k < o->nb_segments; k++ ) {
    const dvd_segment_t *g = &o->segments[k];
    uint32_t last, s1, e1, s2, e2;
    if( !g->nb_extents || g->extents[0].sector == (last = segment_last_vobu( v, pgcn, g )) )
      continue;
    if( !vobu_times( v, g->extents[0].sector, &s1, &e1 ) || !vobu_times( v, last, &s2, &e2 ) ||
        s1 > e1 || e1 > s2 || s2 > e2 ) {
      Log3( v->ctx, "VTS_%02d program chain %d: segment %d: presentation times out of order", v->title, pgcn + 1,
            k + 1 );
      return -1;
    }
    pos += e2 - s1;
  }
  if( grow_array( (void **)&o->nav_invalid, &o->alloc_nav_invalid, o->nb_nav_invalid, sizeof(*o->nav_invalid) ) < 0 )
    return -2;
  o->nav_invalid[o->nb_nav_invalid++] = (uint32_t)(pos / 90000);
  return 0;
}

void DVDFreeTitleSegments_rec( dvd_title_segments_t *o )
{
  if( !o )
    return;
  for( int k = 0; k < o->nb_segments; k++ ) {
    free( o->segments[k].extents );
    free( o->segments[k].runs );
  }
  free( o->segments );
  free( o->nav_invalid );
  memset( o, 0, sizeof(*o) );
}

int DVDTitleSegments_rec( dvd_vob_reader_t *v, int pgcn, const int *cells, int nb_cells, dvd_title_segments_t *o )
{
  dvd_reader_t *ctx = v->ctx;
  const pgc_rec_t *p = pgc_of( v, pgcn );

  memset( o, 0, sizeof(*o) );
  if( !p )
    return 0;
  for( int i = 0; i < nb_cells; i++ ) {
    const cell_rec_t *c = &p->cells[cells[i]];
    uint32_t s = c->first_sector, n;
    if( s < c->last_vobu_start_sector && !(CELL_FLAGS( c ) & 0xf4) ) {
      if( !(CELL_FLAGS( c ) & 1) ) {
        DVDNextVobu_rec( v, s, &n );
        continue;
      }
      if( DVDVobuMapTrusted_rec( ctx, v->title ) && DVDCellSeamlessAngleOk_rec( v, pgcn, cells[i] ) &&
          DVDNextVobu_rec( v, s, &n ) )
        continue;
    }
    if( s >= v->blocks )
      goto outside;
    o->cells_to_scan++;
    for( ;; ) {
      o->vobus_to_scan += vobu_needs_scan( v, pgcn, cells[i], s );
      if( s == c->last_vobu_start_sector || s > c->last_sector )
        break;
      if( !DVDNextVobu_rec( v, s, &n ) ) {
        DVDNextVobu_rec( v, s, &n );
        Log3( ctx, "VTS_%02d program chain %d: no VOBU after sector %" PRIu32, v->title, pgcn + 1, s );
        return 0;
      }
      s = n;
      if( s >= v->blocks )
        goto outside;
    }
  }
  o->counted = 1;

  for( int i = 0; i < nb_cells; i++ ) {
    int ci = cells[i];
    const cell_rec_t *c = &p->cells[ci];
    uint8_t fl = CELL_FLAGS( c );
    int ns = !!(fl & 2), new_seg = ns, il = 0;
    uint32_t s = c->first_sector, next = 0;

    if( i ) {
      int pi = cells[i - 1];
      if( ci <= pi ) {
        new_seg = 1;
      } else {
        for( int k = pi + 1; k < ci; k++ )
          if( !BLOCK_TYPE( CELL_FLAGS( &p->cells[k] ) ) && (CELL_FLAGS( &p->cells[k] ) & 2) )
            ns = 1;
        new_seg = ns;
      }
    }
    if( fl & 4 ) {
      if( (BLOCK_TYPE( fl ) == 0 && BLOCK_MODE( fl ) == 0) || (BLOCK_TYPE( fl ) == 1 && BLOCK_MODE( fl ) >= 1) )
        il = DVDIlvuWalk_rec( v, c->first_sector, c->last_sector + 1 );
      else
        Log3( ctx, "VTS_%02d program chain %d: interleaved cell %d outside an angle block", v->title, pgcn + 1,
              ci + 1 );
    }
    while( next != DVD_VOBU_END && s < c->last_sector ) {
      dvd_segment_t *g;
      uint32_t len = 0;
      int done = 0;
      if( s >= v->blocks )
        goto outside;
      vobu_needs_scan( v, pgcn, ci, s );
      if( il ) {
        const dvd_vobu_rec_t *e = DVDFindVobu_rec( v, s, NULL );
        if( e && !ctx->vob_css[v->title].map_distrusted ) {
          uint32_t len0 = e->ilvu_ea + 1, target = s + len0, n2, l2;
          int at = map_lower( ctx, v->title, target );
          if( at < ctx->vob_css[v->title].nb_map && ctx->vob_css[v->title].map[at] == target && at ) {
            uint32_t q = ctx->vob_css[v->title].map[at - 1];
            if( DVDCellNavNextVobu_rec( v, pgcn, ci, q, &n2, &l2 ) && q + l2 == target ) {
              next = n2;
              len  = len0;
              done = 1;
            }
          }
        }
      }
      if( !done && !DVDCellNextVobu_rec( v, pgcn, ci, s, &next, &len ) )
        return 0;
      if( !o->nb_segments || new_seg ) {
        if( add_segment( o ) < 0 || add_extent( &o->segments[o->nb_segments - 1], s, len ) < 0 )
          return -1;
      } else {
        dvd_extent_t *e;
        g = &o->segments[o->nb_segments - 1];
        e = &g->extents[g->nb_extents - 1];
        if( e->sector + e->count == s )
          e->count += len;
        else if( add_extent( g, s, len ) < 0 )
          return -1;
      }
      g = &o->segments[o->nb_segments - 1];
      g->last_cell = ci;
      if( add_cell_to_runs( g, ci ) < 0 )
        return -1;
      if( next == DVD_VOBU_END ) {
        int r;
        next = s + len;
        if( next < c->last_sector && (r = nav_invalid( v, pgcn, o )) < 0 )
          return r == -2 ? -1 : 0;
      }
      new_seg = 0;
      s = next;
    }
  }
  for( int k = 0; k < o->nb_segments; k++ ) {
    dvd_segment_t *g = &o->segments[k];
    if( g->nb_extents )
      g->size = (int64_t)(g->extents[g->nb_extents - 1].logical + g->extents[g->nb_extents - 1].count) * SECTOR;
  }
  return 1;
outside:
  Log3( ctx, "VTS_%02d program chain %d: a VOBU outside the title VOBs", v->title, pgcn + 1 );
  return 0;
}

void DVDChapterPlaces_rec( dvd_vob_reader_t *v, int pgcn, const dvd_title_segments_t *o, dvd_chapter_place_t *ch,
                           int nb )
{
  const pgc_rec_t *p = pgc_of( v, pgcn );
  const dvd_chapter_place_t *prev = NULL;

  for( int k = 0; k < nb; k++ ) {
    dvd_chapter_place_t *c = &ch[k];
    uint32_t x;
    int seg = -1;
    int64_t off = 0, tm;
    c->segment = -1;
    if( c->cell < 0 || !p || c->cell >= p->nr_of_cells )
      continue;
    x = p->cells[c->cell].first_sector;
    for( int g = 0; g < o->nb_segments && seg < 0; g++ ) {
      const dvd_segment_t *sg = &o->segments[g];
      int64_t before = 0;
      for( int e = 0; e < sg->nb_extents; e++ ) {
        const dvd_extent_t *ex = &sg->extents[e];
        if( ex->sector <= x && x < ex->sector + ex->count ) {
          seg = g;
          off = (before + x - ex->sector) * SECTOR;
          break;
        }
        before += ex->count;
      }
    }
    if( seg < 0 )
      continue;
    if( prev && (seg < prev->segment || (seg == prev->segment && off <= prev->offset)) )
      continue;
    c->segment = seg;
    c->offset  = off;
    c->time    = 0;
    for( int g = 0; g < seg; g++ ) {
      if( !segment_time( v, &o->segments[g], segment_last_vobu( v, pgcn, &o->segments[g] ), &tm ) )
        Log3( v->ctx, "VTS_%02d program chain %d: segment %d has no length", v->title, pgcn + 1, g + 1 );
      else
        c->time += tm;
    }
    if( !segment_time( v, &o->segments[seg], x, &tm ) )
      Log3( v->ctx, "VTS_%02d program chain %d: chapter %d has no time", v->title, pgcn + 1, k + 1 );
    else
      c->time += tm;
    prev = c;
  }
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
