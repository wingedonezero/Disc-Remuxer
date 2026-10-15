/*
 * DVD-Video: the packets of a title VOB sector and the stream probe of a
 * title (dvd_stream_probe.h: DVDPackPacket_rec(), DVDProbeTitleStreams_rec()),
 * its sectors read through the title set's own reading of its title VOBs
 * (dvd_vob.h).
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
#include <stdlib.h>
#include <string.h>

#include <dvdread/dvd_reader.h>
#include "dvd_vob.h"

#include "base/mem.h"

#include "dvd_stream_probe.h"

#define SECTOR      DVD_VIDEO_LB_LEN
#define ZONE0       (200LL << 20)       /* the segment's start; longer segments get the two zones below */
#define ZONE_SIZE   (100LL << 20)       /* from the middle; at the end */
#define CC_LIMIT    (8LL << 20)
#define AUDIO_FIRST (48LL << 20)
#define CC_KEY      0xffe0              /* kept with the keys: captions found */

static uint16_t rb16( const uint8_t *p )
{
  return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t rb32( const uint8_t *p )
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* ---- the packets of a sector ---- */

/* PES header. Packets of other streams than video, private stream 1 and
 * MPEG audio are not checked against the bytes available. */
static int parse_pes( dvd_pack_packet_t *p, const uint8_t *b, int avail )
{
  int sid, f;

  if( avail < 6 || b[0] || b[1] || b[2] != 1 )
    return 0;
  sid = b[3];
  if( (sid & 0xfe) != 0xbe && avail < 9 )
    return 0;
  p->len      = rb16( b + 4 ) ? rb16( b + 4 ) + 6 : 0;
  p->key      = sid;
  p->first_au = -1;
  p->extra    = 0;
  if( (sid & 0xfe) == 0xbe ) {        /* padding, private stream 2 */
    p->payload = 6;
    return 1;
  }
  if( sid == 0xe0 || sid == 0xe1 || sid == 0xe2 || sid == 0xfd || sid == 0xbd || (sid & 0xe0) == 0xc0 ) {
    f = b[7];
    if( ((f & 0xc0) == 0xc0 || (f & 0xc0) == 0x80) && avail < 14 )
      return 0;                       /* a PTS that does not fit */
    if( (f & 1) && !(f & 0x3e) ) {    /* PES extension, no other optional field */
      int e = 9 + 5 * ((f >> 6) & 1) + 5 * (f >> 7), q;

      if( avail < e + 1 )
        return 0;
      if( b[e] < 0x40 ) {
        q = e + ((b[e] & 0x10) ? 3 : 1);
        if( b[e] & 1 ) {
          if( avail < q + 2 )
            return 0;
          if( b[q] == 0x81 && b[q + 1] < 0x80 )
            p->key = b[q + 1] << 8 | sid;
        }
      }
    }
    if( avail < b[8] + 9 )
      return 0;
  }
  p->payload = b[8] + 9;
  return 1;
}

/* PES header and, for private stream 1, the sub-stream header (audio 4
 * bytes {id, frame count, first access unit pointer}, MLP 5 bytes,
 * sub-pictures 1 byte; LPCM keeps its 3-byte audio frame header). */
static int parse_packet( dvd_pack_packet_t *p, const uint8_t *b, int avail )
{
  int off, b0, cls;

  if( !parse_pes( p, b, avail ) )
    return 0;
  if( p->key != 0xbd )
    return 1;
  off = p->payload;
  if( avail - off < 2 )
    return 0;
  b0  = b[off];
  cls = b0 & 0xf8;
  if( cls >= 0x20 && cls <= 0x38 ) {
    p->first_au = -2;
    off += 1;
  } else if( cls == 0x80 || cls == 0x88 || cls == 0xc0 || cls == 0xa0 ) {
    int nf, ptr, f;

    if( cls == 0xa0 )
      p->extra = 3;
    if( avail - off < 5 )
      return 0;
    nf  = b[off + 1];
    ptr = rb16( b + off + 2 );
    f   = ptr ? ptr - 1 : -1;
    p->first_au = (nf || cls == 0x88) ? f : -1;
    off += 4;
  } else if( cls == 0xb0 ) {
    int v;

    if( avail - off < 6 )
      return 0;
    v = rb16( b + off + 1 );
    p->first_au = v >= 3 ? v - 3 : -1;
    off += 5;
  } else {
    off += 1;
  }
  p->key     = b0 << 8 | 0xbd;
  p->payload = off;
  return 1;
}

/* The pack header's length (14 + stuffing; the stuffing counts only when
 * its first byte is not 0), 0 when the sector does not start with one. */
static int pack_header_len( const uint8_t *s )
{
  if( rb32( s ) != 0x1ba || (s[4] & 0xc0) != 0x40 )
    return 0;
  return (s[13] & 7) && s[14] ? 14 + (s[13] & 7) : 14;
}

int DVDPackPacket_rec( const uint8_t *s, int *off, dvd_pack_packet_t *p )
{
  int at = *off, pos, lim, n = 0;

  p->why = "not a valid pack";
  if( !at && !(at = pack_header_len( s )) )
    return 0;
  if( !parse_packet( p, s + at, SECTOR - at ) )
    return 0;
  p->at = at;
  if( p->extra ) {
    int d = p->len - p->payload;
    if( d == p->extra ) {             /* nothing but the LPCM header: like padding */
      p->extra = 0;
      p->key   = 0xbe;
    } else if( d < p->extra ) {
      p->why = "an LPCM packet is shorter than its audio frame header";
      return 0;
    } else {
      if( p->first_au >= 0 && p->first_au < p->extra ) {
        p->why = "an LPCM packet's first audio frame starts inside its audio frame header";
        return 0;
      }
      if( p->first_au >= 0 )
        p->first_au -= p->extra;
      p->payload += p->extra;
    }
  }
  if( at >= 0x7f7 && !p->len && p->key == 0xbe ) {
    *off = SECTOR;
    return 1;
  }
  pos = at + p->len;
  lim = p->key == 0xbf ? SECTOR : pos;
  if( lim >= SECTOR ) {
    *off = lim;
    return 1;
  }
  for( ;; ) {
    dvd_pack_packet_t pad;
    int k;

    for( k = pos + 1; k < SECTOR && !s[k]; k++ )
      ;
    if( !s[pos] && k == SECTOR )
      break;
    for( k = pos; k < SECTOR && s[k] == 0xff; k++ )
      ;
    if( k == SECTOR || pos > 0x7f9 )
      break;
    if( rb32( s + pos ) != 0x1be )
      goto next;
    if( !parse_pes( &pad, s + pos, SECTOR - pos ) )
      goto next;
    if( pos >= 0x7f7 && !pad.len ) {
      if( pad.key == 0xbe )
        pos = SECTOR;
      goto next;
    }
    if( !pad.len )
      goto next;
    pos += pad.len;
    if( pos > SECTOR )
      break;
    n++;
    if( pos == SECTOR )
      break;
  }
  *off = SECTOR;
  return 1;
next:
  if( n && pos >= 0x601 && pos <= 0x7ff ) {   /* after padding near the end: is it a packet? */
    dvd_pack_packet_t t;
    int q = pos;
    if( !parse_packet( &t, s + pos, SECTOR - pos ) || !DVDPackPacket_rec( s, &q, &t ) )
      pos = SECTOR;
  }
  *off = pos;
  return 1;
}

/* ---- the stream probe ---- */

typedef struct {
  dvd_vob_reader_t   *v;
  const dvd_extent_t *extents;
  int                 nb_extents;
  int64_t             start[3], len[3], done[3];
  uint16_t           *keys;           /* the keys seen: sorted, each once */
  int                 nb_keys, alloc_keys;
  uint32_t            failed;
} probe_t;

static int has_key( const probe_t *g, uint16_t k )
{
  int lo = 0, hi = g->nb_keys - 1;
  while( lo <= hi ) {
    int mid = (lo + hi) / 2;
    if( g->keys[mid] == k )
      return 1;
    if( g->keys[mid] < k )
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return 0;
}

static int add_key( probe_t *g, uint16_t k )
{
  int at = 0;

  if( has_key( g, k ) )
    return 0;
  if( g->nb_keys == g->alloc_keys ) {
    uint16_t *n = dr_realloc( g->keys, (g->alloc_keys + 16) * sizeof(*n) );
    if( !n )
      return -1;
    g->keys = n;
    g->alloc_keys += 16;
  }
  while( at < g->nb_keys && g->keys[at] < k )
    at++;
  memmove( g->keys + at + 1, g->keys + at, (g->nb_keys - at) * sizeof(*g->keys) );
  g->keys[at] = k;
  g->nb_keys++;
  return 0;
}

/* The DVD caption user data in packet p's payload (the first match decides). */
static int caption_header( const uint8_t *sec, const dvd_pack_packet_t *p )
{
  static const uint8_t pat[8] = { 0x00, 0x00, 0x01, 0xb2, 'C', 'C', 0x01, 0xf8 };
  const uint8_t *pl = sec + p->at + p->payload;
  int n = (p->len < SECTOR - p->at ? p->len : SECTOR - p->at) - p->payload;

  if( n <= 7 )
    return 0;
  for( int i = 0; i + (int)sizeof(pat) <= n; i++ )
    if( !memcmp( pl + i, pat, sizeof(pat) ) )
      return n - i > 8 && !(pl[i + 8] & 0x40);
  return 0;
}

/* One sector's first two packets: their keys kept; 1 when one is want (cc:
 * a video packet with the caption user data), -1 no memory. */
static int walk( probe_t *g, const uint8_t *sec, uint16_t want, int cc )
{
  dvd_pack_packet_t p;
  int off = 0, found = 0;

  for( int n = 0; n < 2 && off < SECTOR; n++ ) {
    if( !DVDPackPacket_rec( sec, &off, &p ) )
      break;
    if( add_key( g, p.key ) < 0 )
      return -1;
    if( (uint16_t)p.key != want )
      continue;
    if( !cc ) {
      found = 1;
    } else if( caption_header( sec, &p ) ) {
      if( add_key( g, CC_KEY ) < 0 )
        return -1;
      found = 1;
    }
  }
  return found;
}

/* The title VOB sector of the segment's block; -1 when no extent holds it. */
static int64_t block_sector( const probe_t *g, uint32_t block )
{
  int lo = 0, hi = g->nb_extents - 1;

  while( lo <= hi ) {                 /* the extent holding block (the extents follow each other) */
    int mid = (lo + hi) / 2;
    const dvd_extent_t *e = &g->extents[mid];
    if( block < e->logical )
      hi = mid - 1;
    else if( block >= e->logical + e->count )
      lo = mid + 1;
    else
      return e->sector + (block - e->logical);
  }
  return -1;
}

/* Zone zone read on from where it stopped, up to max bytes of it, until a
 * sector has want: 0 done, -1 no memory, -2 / -3 a sector cannot be read. */
static int scan( probe_t *g, uint16_t want, int cc, int zone, int64_t max )
{
  int64_t lim = g->len[zone] < max ? g->len[zone] : max;
  int64_t pos = g->start[zone] + g->done[zone], end = g->start[zone] + lim;
  uint8_t buf[SECTOR];
  int ret;

  if( g->done[zone] >= g->len[zone] || pos >= end )
    return 0;
  for( ; pos < end; pos += SECTOR ) {
    int64_t sector = block_sector( g, (uint32_t)(pos / SECTOR) );
    if( sector < 0 || (ret = DVDReadTitleVOBSector_rec( g->v, (uint32_t)sector, buf, NULL )) < 0 ) {
      g->failed = (uint32_t)(pos / SECTOR);
      return sector >= 0 && ret == -2 ? -3 : -2;
    }
    g->done[zone] += SECTOR;
    if( ret && (ret = walk( g, buf, want, cc )) != 0 )
      return ret < 0 ? -1 : 0;
  }
  return 0;
}

/* An audio stream's key by its coding mode, 0 when the mode is not known. */
static uint16_t audio_key( int stream, int coding )
{
  switch( coding ) {
  case 0:  return (0x80 + stream) << 8 | 0xbd;   /* AC-3 */
  case 2:
  case 3:  return 0xc0 + stream;                  /* MPEG audio */
  case 4:  return (0xa0 + stream) << 8 | 0xbd;   /* LPCM */
  case 6:  return (0x88 + stream) << 8 | 0xbd;   /* DTS */
  default: return 0;
  }
}

int DVDProbeTitleStreams_rec( dvd_vob_reader_t *v, const dvd_extent_t *extents, int nb_extents,
                              dvd_probe_audio_t *audio, int nb_audio, int *captions, uint32_t *failed_block )
{
  probe_t g = { .v = v, .extents = extents, .nb_extents = nb_extents };
  const dvd_extent_t *e = nb_extents ? &extents[nb_extents - 1] : NULL;
  int64_t size = e ? (int64_t)(e->logical + e->count) * SECTOR : 0;
  int ret = 0;

  *captions = 0;
  g.len[0] = size < ZONE0 ? size : ZONE0;
  if( size > ZONE0 ) {
    int64_t h = size / 2 - size / 2 % SECTOR;
    g.start[1] = h > ZONE0 ? h : ZONE0;
    g.len[1]   = g.start[1] + ZONE_SIZE <= size ? ZONE_SIZE : size - g.start[1];
    g.start[2] = g.start[1] + g.len[1] > size - ZONE_SIZE ? g.start[1] + g.len[1] : size - ZONE_SIZE;
    g.len[2]   = ZONE_SIZE < size - g.start[2] ? ZONE_SIZE : size - g.start[2];
  }

  if( (ret = scan( &g, 0xe0, 1, 0, CC_LIMIT )) < 0 )
    goto end;
  *captions = has_key( &g, CC_KEY );

  for( int i = 0; i < nb_audio; i++ ) {
    dvd_probe_audio_t *a = &audio[i];
    uint16_t key = audio_key( a->stream, a->coding );

    if( !key ) {
      a->result = DVD_PROBE_UNKNOWN;
      continue;
    }
    a->result = DVD_PROBE_FOUND;
    if( has_key( &g, key ) )
      continue;
    if( (ret = scan( &g, key, 0, 0, AUDIO_FIRST )) < 0 )
      goto end;
    if( has_key( &g, key ) )
      continue;
    if( !a->listed ) {
      a->result = DVD_PROBE_NOT_LISTED;
      continue;
    }
    for( int z = 0; z < 3 && !has_key( &g, key ); z++ )
      if( (ret = scan( &g, key, 0, z, z ? ZONE_SIZE : ZONE0 )) < 0 )
        goto end;
    a->result = has_key( &g, key ) ? DVD_PROBE_FOUND_LATER : DVD_PROBE_EMPTY;
  }
  ret = 1;
end:
  if( ret < -1 )
    *failed_block = g.failed;
  dr_free( g.keys );
  return ret;
}
