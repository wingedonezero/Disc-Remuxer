/*
 * Title VOB sectors read for ripping: the sector check and the damaged
 * ranges (dvdread/vob_read.h).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvdread/dvd_filesystem.h"
#include "dvdread/dvd_reader.h"
#include "dvdread/ifo_read.h"
#include "dvdread/vob_read.h"
#include "dvdread_internal.h"
#include "logger.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SECTOR    DVD_VIDEO_LB_LEN
#define MAX_PARTS 9                    /* VTS_nn_1..9.VOB */

typedef struct {
  char     name[16];                  /* "VTS_01_1.VOB" */
  void    *file;                      /* a folder's file, opened (NULL on an image) */
  uint64_t size;
  int64_t  sector;                    /* image: where the file starts on it, -1: not known */
} vob_part_t;

struct dvd_vob_reader_s {
  dvd_reader_t *ctx;
  int           title;
  vob_part_t    parts[MAX_PARTS];
  int           nb_parts;
  /* image: the image, the title VOBs' first sector on it, its sectors */
  void         *image;
  int64_t       start;
  int64_t       image_sectors;
  uint64_t      bytes;                /* folder: the files' bytes */
  /* the damaged range being read */
  dvd_vob_damage_t open;
};

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
      /* the file whose sectors hold it (one piece on the image) */
      int64_t at = v->start + sector;
      if( p->sector >= 0 && at >= p->sector && (uint64_t)(at - p->sector) * SECTOR < p->size ) {
        snprintf( file, 16, "%s", p->name );
        *offset = (at - p->sector) * SECTOR;
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

dvd_vob_reader_t *DVDOpenTitleVOBs_rec( ifo_handle_t *ifo, int title )
{
  struct ifo_handle_private_s *ifop;
  dvd_reader_t *ctx;
  dvd_vob_reader_t *v;
  char path[48], full[PATH_MAX + 1];
  dvdstat_t st;
  uint8_t b[4];

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
  }

  /* an image: the title VOBs from where the IFO header puts them */
  snprintf( path, sizeof(path), "/VIDEO_TS/VTS_%02d_0.IFO", title );
  if( ctx->fs->image_open && DVDStatFile_( ctx, path, &st ) >= 0 && st.sector >= 0 ) {
    int64_t size;
    if( !ifoRead_rec_bytes( ifo, 0xc4, b, 4 ) )     /* vtstt_vobs */
      goto fail;
    v->start = st.sector + ((int64_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
    if( v->start == st.sector || !(v->image = ctx->fs->image_open( ctx->fs )) )
      goto fail;
    if( (size = ctx->fs->file_seek( v->image, 0, SEEK_END )) < 0 )
      goto fail;
    v->image_sectors = (size + SECTOR - 1) / SECTOR;
    if( v->start >= v->image_sectors )
      goto fail;
    return v;
  }

  /* a folder: the files joined */
  for( int i = 0; i < v->nb_parts; i++ ) {
    vob_part_t *p = &v->parts[i];
    snprintf( path, sizeof(path), "/VIDEO_TS/%s", p->name );
    if( DVDFindFile_( ctx, path, full ) < 0 || !(p->file = ctx->fs->file_open( ctx->fs, full )) ) {
      v->nb_parts = i;
      break;
    }
    v->bytes += p->size;
  }
  if( !v->nb_parts )
    goto fail;
  return v;

fail:
  Log3( ctx, "VTS_%02d: the title VOBs cannot be opened", title );
  DVDCloseTitleVOBs_rec( v );
  return NULL;
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

  if( v->image )
    return v->start + sector < v->image_sectors ? fs_read( fs, v->image, (v->start + sector) * SECTOR, buf, SECTOR )
                                                : -1;
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
  return 1;
}

void DVDEndTitleVOBDamage_rec( dvd_vob_reader_t *v, dvd_vob_damage_t *ended )
{
  if( ended )
    memset( ended, 0, sizeof(*ended) );
  if( v )
    end_damage( v, ended );
}

void DVDCloseTitleVOBs_rec( dvd_vob_reader_t *v )
{
  if( !v )
    return;
  end_damage( v, NULL );
  for( int i = 0; i < MAX_PARTS; i++ )
    if( v->parts[i].file )
      v->ctx->fs->file_close( v->parts[i].file );
  if( v->image )
    v->ctx->fs->file_close( v->image );
  free( v );
}
