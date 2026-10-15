/*
 * DVD-Video: the VOBUs of a title set's title VOBs (dvd_vobu.h): what their
 * navigation packs say, recorded per sector (title sets at a known place of
 * an image together, by absolute sector); where VOBUs start and follow, by
 * the VOBU address map while it is trusted, else the maps of title sets
 * sharing the title VOBs, else the navigation packs. On the title set's own
 * reading of its title VOBs (libdvdread, vob_read.h); the findings go into
 * the disc's findings book, the lines into the log the way the DVD
 * libraries' do ("DVD: ").
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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/nav_read.h>
#include <dvdread/nav_types.h>
#include <dvdread/vob_read.h>

#include "base/mem.h"

#include "dvd_internal.h"

#define SECTOR      DVD_VIDEO_LB_LEN

/* the VOBUs' lines, as libdvdread's debug lines */
#define Log3( v, ... ) ff_dvd_lib_log( (v)->d, DVD_LOGGER_LEVEL_DEBUG, __VA_ARGS__ )

static uint32_t be32( const uint8_t *p )
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Where title set t's title VOBs start on an image (0: not known), and their
 * length by the IFO (libdvdread's placement). */
static int64_t title_start( const DVDDisc *d, int t )
{
  uint32_t blocks;
  return DVDTitleVOBPlace_rec( d->reader, t, &blocks );
}

static uint32_t title_blocks( const DVDDisc *d, int t )
{
  uint32_t blocks;
  DVDTitleVOBPlace_rec( d->reader, t, &blocks );
  return blocks;
}

/* ---- VOBUs ---- */

typedef DVDVobuStore vobu_store_t;

/* A finding about the title set's VOBUs (an IFO's table or the navigation
 * packs: the disc's findings). */
static void vobu_report( DVDCells *v, dvd_report_level_t level, const char *file, const char *table,
                         int64_t sector, const char *fmt, ... )
{
  char text[256];
  va_list list;

  dvd_report_t report = { .level = level, .file = file, .table = table, .offset = -1, .sector = sector,
                          .text = text };

  va_start( list, fmt );
  vsnprintf( text, sizeof(text), fmt, list );
  va_end( list );
  ff_dvd_report( v->d, &report );
}

/* The store a title set's records are in, and the key of its sector. */
static vobu_store_t *vobu_store( DVDCells *v )
{
  return title_start( v->d, v->title ) ? &v->d->vobus : &v->vobus;
}

static uint32_t vobu_key( const DVDCells *v, uint32_t sector )
{
  return (uint32_t)title_start( v->d, v->title ) + sector;
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
    dvd_vobu_rec_t *n = dr_realloc( st->recs, alloc * sizeof(*n) );
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

const dvd_vobu_rec_t *DVDFindVobu_rec( DVDCells *v, uint32_t sector, int64_t *next_key )
{
  vobu_store_t *st = vobu_store( v );
  uint32_t key = vobu_key( v, sector ), start = (uint32_t)title_start( v->d, v->title );
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

int DVDMarkVobuBad_rec( DVDCells *v, uint32_t sector )
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
static void parse_nav( DVDCells *v, const uint8_t *b, dvd_vobu_nav_t *n )
{
  uint8_t copy[SECTOR];
  pci_t pci;
  dsi_t dsi;

  memcpy( copy, b, SECTOR );
  navRead_PCI_ctx( v->d->reader, &pci, copy + 0x2d );
  navRead_DSI_ctx( v->d->reader, &dsi, copy + DSI_START_BYTE );
  n->vobu_s_ptm = pci.pci_gi.vobu_s_ptm;
  n->vobu_e_ptm = pci.pci_gi.vobu_e_ptm;
  n->vobu_ea    = dsi.dsi_gi.vobu_ea;
  n->category   = dsi.sml_pbi.category;
  n->ilvu_ea    = dsi.sml_pbi.ilvu_ea;
  n->next_vobu  = dsi.vobu_sri.next_vobu;
}

int DVDReadVobuNav_rec( DVDCells *v, uint32_t sector, dvd_vobu_nav_t *n )
{
  uint8_t b[SECTOR];

  if( DVDReadTitleVOBRaw_rec( v->r, sector, b ) == 0 ) {
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
    Log3( v, "VTS_%02d: title VOB sector %" PRIu32 " is no navigation pack (it starts %02x %02x %02x %02x "
          "%02x %02x %02x %02x)", v->title, sector, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7] );
  }
  DVDMarkVobuBad_rec( v, sector );
  return 0;
}

int DVDNoteVobuSector_rec( DVDCells *v, uint32_t sector, const uint8_t *buf )
{
  dvd_vobu_nav_t n;
  uint32_t next, len;

  if( !DVDIsNavPack_rec( buf ) )
    return DVDMarkVobuBad_rec( v, sector );
  parse_nav( v, buf, &n );
  DVDRegisterVobu_rec( v, &n, sector, 1, &next, &len );
  return 0;
}

int DVDRegisterVobu_rec( DVDCells *v, const dvd_vobu_nav_t *n, uint32_t sector, int allow_self,
                         uint32_t *next_out, uint32_t *len_out )
{
  uint32_t nx = n->next_vobu & 0x7fffffff, ea = n->vobu_ea;
  int64_t next = nx == 0x3fffffff ? -2 : (int64_t)sector + nx;

  if( next == sector ) {
    if( !allow_self ) {
      Log3( v, "VTS_%02d: the VOBU at %" PRIu32 " names itself as the next one", v->title, sector );
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
      DVDTitleVOBSectorPlace_rec( v->r, sector, file, &offset );
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
      ff_dvd_report( v->d, &r );
    } else {
      dvd_vobu_nav_t n2;
      uint32_t nn, nl;
      next = (int64_t)sector + ea + 1;
      if( !DVDFindVobu_rec( v, (uint32_t)next, NULL ) &&
          (!DVDReadVobuNav_rec( v, (uint32_t)next, &n2 ) ||
           !DVDRegisterVobu_rec( v, &n2, (uint32_t)next, 0, &nn, &nl )) ) {
        Log3( v, "VTS_%02d: the VOBU after %" PRIu32 " in its interleaved unit cannot be read", v->title,
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
    Log3( v, "VTS_%02d: the VOBU at %" PRIu32 " does not fit a record", v->title, sector );
  }
  return 1;
}

/* ---- VOBU starts ---- */

int DVDVobuMapTrusted_rec( const DVDDisc *d, int title )
{
  return d && title >= 1 && title <= 99 && !d->cells[title].map_distrusted;
}

int DVDVobuMap_rec( const DVDCells *v, const uint32_t **map, int *nb )
{
  *map = v->map;
  *nb  = v->nb_map;
  return !v->map_distrusted;
}

static int map_lower( const DVDDisc *d, int t, uint32_t sector )
{
  const uint32_t *m = d->cells[t].map;
  int lo = 0, hi = d->cells[t].nb_map;
  while( lo < hi ) {
    int mid = (lo + hi) / 2;
    if( m[mid] < sector )
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

static int map_has( const DVDDisc *d, int t, uint32_t sector )
{
  int at = map_lower( d, t, sector );
  return at < d->cells[t].nb_map && d->cells[t].map[at] == sector;
}

/* The VOBU after the one at sector by title set t's map. */
static int map_next( const DVDDisc *d, int t, uint32_t sector, uint32_t *next )
{
  int at;

  if( d->cells[t].map_distrusted )
    return 0;
  at = map_lower( d, t, sector );
  if( at == d->cells[t].nb_map || d->cells[t].map[at] != sector || at + 1 == d->cells[t].nb_map )
    return 0;
  *next = d->cells[t].map[at + 1];
  return 1;
}

/* Title set o's title VOBs hold title set t's (same place, or around it)
 * and o's map is trusted. */
static int shares_title_vobs( const DVDDisc *d, int t, int o, uint32_t t_blocks )
{
  int64_t tb = title_start( d, t ), ob = title_start( d, o );

  return o != t && !d->cells[o].map_distrusted && ob && tb &&
         (ob == tb || (ob <= tb && tb + t_blocks <= ob + title_blocks( d, o )));
}

/* sector starts a VOBU by its navigation pack (recorded, else read). */
static int start_by_nav( DVDCells *v, uint32_t sector )
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

int DVDVobuStart_rec( DVDCells *v, uint32_t sector )
{
  DVDDisc *d = v->d;

  if( !d->cells[v->title].map_distrusted ) {
    if( map_has( d, v->title, sector ) )
      return 1;
    char ifo[16];
    snprintf( ifo, sizeof(ifo), "VTS_%02d_0.IFO", v->title );
    vobu_report( v, DVD_REPORT_WARN, ifo, "VTS_VOBU_ADMAP", sector, "title VOB sector %" PRIu32 " starts no VOBU "
                 "of the VOBU address map: its VOBU starts are not trusted (the VOBs must be scanned)", sector );
    d->cells[v->title].map_distrusted = 1;
    return start_by_nav( v, sector );
  }
  if( title_start( d, v->title ) ) {
    /* title sets sharing these title VOBs: two or more maps agreeing are
     * enough (the sector asked as it is, relative to each one's own VOBs) */
    int hits = 0;
    for( int o = 1; o < 100; o++ ) {
      if( !shares_title_vobs( d, v->title, o, v->blocks ) )
        continue;
      if( !map_has( d, o, sector ) )
        return start_by_nav( v, sector );
      hits++;
    }
    if( hits >= 2 )
      return 1;
  }
  return start_by_nav( v, sector );
}

int DVDNextVobu_rec( DVDCells *v, uint32_t sector, uint32_t *next )
{
  DVDDisc *d = v->d;
  const dvd_vobu_rec_t *r;
  dvd_vobu_nav_t n, n2;
  uint32_t nx, nl, after;

  if( !d->cells[v->title].map_distrusted )
    return map_next( d, v->title, sector, next );
  if( title_start( d, v->title ) ) {
    /* the maps of title sets sharing these title VOBs, when two or more agree */
    int64_t tb = title_start( d, v->title );
    int hits = 0, agree = 1;
    uint32_t cand = 0;
    for( int o = 1; o < 100 && agree; o++ ) {
      int64_t ob = title_start( d, o );
      uint32_t on;
      if( !shares_title_vobs( d, v->title, o, v->blocks ) )
        continue;
      if( !map_next( d, o, (uint32_t)(sector - ob + tb), &on ) ) {
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

int DVDVobuBad_rec( DVDCells *v, uint32_t sector )
{
  const dvd_vobu_rec_t *r;
  uint8_t b[SECTOR];
  dvd_vobu_nav_t n;
  uint32_t next, len;

  if( sector >= v->blocks )
    return 1;
  if( (r = DVDFindVobu_rec( v, sector, NULL )) )
    return r->next == DVD_VOBU_NO_NAV;
  if( DVDReadTitleVOBRaw_rec( v->r, sector, b ) < 0 || !DVDIsNavPack_rec( b ) ) {
    DVDMarkVobuBad_rec( v, sector );
    return 1;
  }
  parse_nav( v, b, &n );
  DVDRegisterVobu_rec( v, &n, sector, 1, &next, &len );
  return 0;
}
