/*
 * DVD-Video: title VOB sectors read for ripping (dvd_vob.h): where an
 * image's title and menu VOBs start, the title VOBs opened (an image's at
 * their start place, a folder's files joined, the file view through the
 * VOB files when a read fails), the sector check, the damaged ranges and
 * CSS (the title key found and confirmed through libdvdcss, the sectors
 * descrambled). The files are read through the file callbacks and the
 * file lookup libdvdread reads the disc with (dvd_nav.c: our io); the
 * findings go into the disc's findings book, the lines into the log the
 * way the DVD libraries' do ("DVD: ").
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dvdcss/dvdcss.h>

#include <dvdread/dvd_filesystem.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>

#include "base/mem.h"

#include "dvd_internal.h"
#include "dvd_vob.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SECTOR      DVD_VIDEO_LB_LEN
#define MAX_PARTS   9                  /* VTS_nn_1..9.VOB */
#define CLEAR       0x80               /* CSS leaves the first 128 bytes of a sector clear */
#define CHECK_LIMIT 8192               /* sectors after the key's start searched for one the key check can use */

/* the lines, as libdvdread's details and debug lines */
#define Log2( d, ... ) ff_dvd_lib_log( (d), DVD_LOGGER_LEVEL_INFO, __VA_ARGS__ )
#define Log3( d, ... ) ff_dvd_lib_log( (d), DVD_LOGGER_LEVEL_DEBUG, __VA_ARGS__ )

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
  DVDDisc      *d;
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
  ff_dvd_report( v->d, &r );
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
  ff_dvd_lib_log( v->d, level == DVD_REPORT_ERROR ? DVD_LOGGER_LEVEL_ERROR :
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
      Log2( v->d, "VTS_%02d: the CSS title key is right on sector %" PRIu32 " (%s)", v->title, s,
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

/* The title set's title VOBs lie inside title set o's (the same sectors of
 * an image) and o has its key. */
static int key_shared( const DVDDisc *d, int title, int o )
{
  int64_t fb = d->cells[title].start, ob = d->cells[o].start;
  uint32_t fn = d->cells[title].start_blocks, on = d->cells[o].start_blocks;

  return o != title && d->cells[o].key_state > 0 && fb && ob &&
         (ob == fb || (ob < fb && fn + fb <= ob + on));
}

/* The title set's key, looked for from its scrambled sector on: 0 found
 * (or taken), -1 none (never looked for again). */
static int find_key( dvd_vob_reader_t *v, uint32_t sector )
{
  DVDDisc *d = v->d;
  uint8_t *key = d->cells[v->title].key;

  for( int o = 1; o < 100; o++ )
    if( key_shared( d, v->title, o ) ) {
      memcpy( key, d->cells[o].key, 5 );
      d->cells[v->title].key_state = 1;
      Log2( d, "VTS_%02d: CSS title key taken from VTS_%02d (its title VOBs hold these)", v->title, o );
      return 0;
    }
  css_report( v, DVD_REPORT_INFO, "sector %" PRIu32 " is scrambled: looking for the CSS title key", sector );
  d->cells[v->title].key_state = -1;
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
    Log3( d, "VTS_%02d: CSS title key %02x %02x %02x %02x %02x", v->title, key[0], key[1], key[2], key[3], key[4] );
    if( !check_key( v, sector, key ) )
      return -1;
    d->cells[v->title].key_state = 1;
    css_report( v, DVD_REPORT_INFO, "CSS title key found" );
    return 0;
  }
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
static int64_t place_vobs( DVDDisc *d, ifo_handle_t *ifo, int title, int menu, void *image, int64_t ifo_at )
{
  dvd_reader_filesystem_h *fs = d->read_fs;
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
  if( DVDStatFile_( d->reader, path, &st ) >= 0 )
    fs_at = st.sector;
  if( fs_at < 0 || fs_at == header ) {
    if( fs_at < 0 )
      Log3( d, "%s: the file system has no single place for %s: the %s start where the IFO header puts them",
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
  ff_dvd_report( d, &r );
  return chosen;
}

/* The title VOBs' start (decided once) and their length by the IFO (up to
 * its BUP, the IFO's place when the BUP has none of its own). */
static int64_t place_title_vobs( DVDDisc *d, ifo_handle_t *ifo, int title, void *image, int64_t ifo_at )
{
  char path[48];
  dvdstat_t bup;
  int64_t bup_at;
  uint8_t raw[4];

  if( d->cells[title].placed )
    return d->cells[title].start;
  d->cells[title].placed = 1;
  if( !(d->cells[title].start = place_vobs( d, ifo, title, 0, image, ifo_at )) ||
      !ifoRead_rec_bytes( ifo, 0xc4, raw, 4 ) )
    return d->cells[title].start;
  /* the length by the IFO header's vtstt_vobs, whichever start was chosen */
  snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.BUP", title );
  bup_at = DVDStatFile_( d->reader, path, &bup ) >= 0 && bup.sector >= 0 ? bup.sector : ifo_at;
  d->cells[title].start_blocks = (uint32_t)(bup_at - ifo_at) - be32( raw );
  return d->cells[title].start;
}

int64_t DVDTitleVOBStart_rec( DVDDisc *d, ifo_handle_t *ifo, int title )
{
  char path[48];
  dvdstat_t st;
  void *image;
  int64_t start;

  if( !ifo || title < 1 || title > 99 )
    return 0;
  if( d->cells[title].placed )
    return d->cells[title].start;
  snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.IFO", title );
  if( !d->read_fs || !d->read_fs->image_open || DVDStatFile_( d->reader, path, &st ) < 0 || st.sector < 0 ||
      !(image = d->read_fs->image_open( d->read_fs )) )
    return 0;
  start = place_title_vobs( d, ifo, title, image, st.sector );
  d->read_fs->file_close( image );
  return start;
}

int64_t DVDMenuVOBStart_rec( DVDDisc *d, ifo_handle_t *ifo, int title )
{
  char path[48];
  dvdstat_t st;
  void *image;

  if( !ifo || title < 0 || title > 99 )
    return 0;
  if( d->cells[title].menu_placed )
    return d->cells[title].menu_start;
  if( title )
    snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.IFO", title );
  else
    snprintf( path, sizeof(path), "/VIDEO_TS/VIDEO_TS.IFO" );
  if( !d->read_fs || !d->read_fs->image_open || DVDStatFile_( d->reader, path, &st ) < 0 || st.sector < 0 ||
      !(image = d->read_fs->image_open( d->read_fs )) )
    return 0;
  d->cells[title].menu_placed = 1;
  d->cells[title].menu_start  = place_vobs( d, ifo, title, 1, image, st.sector );
  d->read_fs->file_close( image );
  return d->cells[title].menu_start;
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
  dvd_reader_filesystem_h *fs = v->d->read_fs;
  uint32_t rem = v->blocks, off = 0;
  char path[48], full[PATH_MAX + 1];

  for( int k = 1; rem; k++ ) {
    vob_part_t *p = k <= v->nb_parts ? &v->parts[k - 1] : NULL;
    view_piece_t piece = { -1, off, rem };
    uint32_t n = rem;

    if( !p ) {
      if( k < 3 ) {
        Log3( v->d, "VTS_%02d: no file view: VTS_%02d_%d.VOB is missing", v->title, v->title, k );
        return 0;
      }
    } else {
      int64_t at = p->nb_pieces > 0 && p->pieces[0][0] > 0 ? p->pieces[0][0] : 0;
      if( (at && at != v->start + off) || p->size % SECTOR ) {
        Log3( v->d, "VTS_%02d: no file view: %s %s", v->title, p->name,
              p->size % SECTOR ? "is not whole sectors" : "does not start where the title VOBs have it" );
        return 0;
      }
      snprintf( path, sizeof(path), "/VIDEO_TS/%s", p->name );
      if( !p->file && (DVDFindFile_( v->d->reader, path, full ) < 0 || !(p->file = fs->file_open( fs, full ))) )
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
      return fs_read( v->d->read_fs, v->image, (v->start + s) * SECTOR, buf, SECTOR );
    return fs_read( v->d->read_fs, v->parts[p->part].file, (int64_t)(s - p->first) * SECTOR, buf, SECTOR );
  }
  return -1;
}

/* The two sectors from s on read the same through the file view and from
 * the start place. */
static int same_two( dvd_vob_reader_t *v, uint32_t s )
{
  uint8_t a[2 * SECTOR], b[2 * SECTOR];

  return view_read( v, s, a ) == 0 && view_read( v, s + 1, a + SECTOR ) == 0 &&
         fs_read( v->d->read_fs, v->image, (v->start + s) * SECTOR, b, sizeof(b) ) == 0 && !memcmp( a, b, sizeof(a) );
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

dvd_vob_reader_t *DVDOpenTitleVOBs_rec( DVDDisc *d, ifo_handle_t *ifo, int title )
{
  dvd_vob_reader_t *v;
  char path[48], full[PATH_MAX + 1], why[96] = "";
  dvdstat_t st;
  uint8_t b[4];
  int found;

  if( !ifo || title < 1 || title > 99 )
    return NULL;
  if( !d->read_fs || !(v = dr_calloc( 1, sizeof(*v) )) )
    return NULL;
  v->d     = d;
  v->title = title;

  /* the title VOB files, up to the first missing one */
  for( int k = 1; k <= MAX_PARTS; k++ ) {
    vob_part_t *p = &v->parts[v->nb_parts];
    snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_%d.VOB", title, k );
    if( DVDFindFile_( d->reader, path, full ) < 0 )
      break;
    st.sector = -1;
    if( d->read_fs->stat( d->read_fs, full, &st ) < 0 )
      break;
    snprintf( p->name, sizeof(p->name), "VTS_%02d_%d.VOB", title, k );
    p->size   = st.size;
    p->sector = st.sector;
    v->nb_parts++;
    /* where its pieces lie on an image */
    if( d->read_fs->file_pieces ) {
      int np = d->read_fs->file_pieces( d->read_fs, full, NULL, 0 );
      if( np > 0 && (p->pieces = dr_calloc( np, sizeof(*p->pieces) )) )
        p->nb_pieces = d->read_fs->file_pieces( d->read_fs, full, p->pieces, np );
    } else if( st.sector >= 0 && (p->pieces = dr_calloc( 1, sizeof(*p->pieces) )) ) {
      p->pieces[0][0] = st.sector;
      p->pieces[0][1] = (int64_t)((st.size + SECTOR - 1) / SECTOR);
      p->nb_pieces    = 1;
    }
  }

  /* an image: the title VOBs from where the IFO header puts them */
  snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.IFO", title );
  if( d->read_fs->image_open && DVDStatFile_( d->reader, path, &st ) >= 0 && st.sector >= 0 ) {
    int64_t size;
    if( !ifoRead_rec_bytes( ifo, 0xc4, b, 4 ) || !be32( b ) ) {   /* vtstt_vobs */
      snprintf( why, sizeof(why), "the IFO header gives no place for them (vtstt_vobs)" );
      goto fail;
    }
    if( !(v->image = d->read_fs->image_open( d->read_fs )) ) {
      snprintf( why, sizeof(why), "the image cannot be opened" );
      goto fail;
    }
    if( !(v->start = place_title_vobs( d, ifo, title, v->image, st.sector )) ) {
      snprintf( why, sizeof(why), "where they start on the image cannot be found" );
      goto fail;
    }
    if( (size = d->read_fs->file_seek( v->image, 0, SEEK_END )) < 0 ) {
      snprintf( why, sizeof(why), "the image's size cannot be read" );
      goto fail;
    }
    v->image_sectors = (size + SECTOR - 1) / SECTOR;
    if( v->start >= v->image_sectors ) {
      snprintf( why, sizeof(why), "they start at sector %" PRId64 ", past the image's %" PRId64 " sectors",
                v->start, v->image_sectors );
      goto fail;
    }
    v->blocks = d->cells[title].start_blocks;
    return opened( v, ifo );
  }

  /* a folder: the files joined */
  found = v->nb_parts;
  for( int i = 0; i < v->nb_parts; i++ ) {
    vob_part_t *p = &v->parts[i];
    snprintf( path, sizeof(path), "/VIDEO_TS/%s", p->name );
    if( DVDFindFile_( d->reader, path, full ) < 0 || !(p->file = d->read_fs->file_open( d->read_fs, full )) ) {
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
  {
    char text[512];
    dvd_report_t r = { .level = DVD_REPORT_ERROR, .file = path, .offset = -1, .sector = -1, .text = text };
    snprintf( text, sizeof(text), "title set %d: its title VOBs cannot be opened: %s", title, why );
    ff_dvd_report( d, &r );
  }
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
  dvd_reader_filesystem_h *fs = v->d->read_fs;
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
      Log2( v->d, "VTS_%02d: title VOB sector %" PRIu32 " cannot be read where the title VOBs start: read "
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
    Log3( v->d, "VTS_%02d: title VOB sector %" PRIu32 " cannot be read", v->title, sector );
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
    int state = v->d->cells[v->title].key_state;
    if( state < 0 || (!state && find_key( v, sector ) < 0) )
      return -2;
    dvdcss_descramble( v->d->cells[v->title].key, buf );
    buf[h + 6] &= 0xcf;
  }
  return 1;
}

int DVDTitleVOBKey_rec( const DVDDisc *d, int title, uint8_t key[5] )
{
  if( !d || title < 1 || title > 99 )
    return 0;
  if( d->cells[title].key_state > 0 && key )
    memcpy( key, d->cells[title].key, 5 );
  return d->cells[title].key_state;
}

void DVDEndTitleVOBDamage_rec( dvd_vob_reader_t *v, dvd_vob_damage_t *ended )
{
  if( ended )
    memset( ended, 0, sizeof(*ended) );
  if( v )
    end_damage( v, ended );
}

/* ---- for the VOBU and cell rules ---- */

uint32_t DVDTitleVOBBlocks_rec( const dvd_vob_reader_t *v )
{
  return v ? v->blocks : 0;
}

int DVDReadTitleVOBRaw_rec( dvd_vob_reader_t *v, uint32_t sector, uint8_t *buf )
{
  return read_sector( v, sector, buf );
}

int64_t DVDTitleVOBPlace_rec( const DVDDisc *d, int title, uint32_t *blocks )
{
  *blocks = d->cells[title].start_blocks;
  return d->cells[title].start;
}

void DVDCloseTitleVOBs_rec( dvd_vob_reader_t *v )
{
  if( !v )
    return;
  end_damage( v, NULL );
  for( int i = 0; i < MAX_PARTS; i++ ) {
    if( v->parts[i].file )
      v->d->read_fs->file_close( v->parts[i].file );
    dr_free( v->parts[i].pieces );
  }
  if( v->image )
    v->d->read_fs->file_close( v->image );
  dr_free( v );
}

/* ---- placement while libdvdread reads the disc's structure ---- */

int64_t ff_dvd_vob_place( void *opaque, ifo_handle_t *ifo, int title, int menu )
{
  return menu ? DVDMenuVOBStart_rec( opaque, ifo, title ) : DVDTitleVOBStart_rec( opaque, ifo, title );
}
