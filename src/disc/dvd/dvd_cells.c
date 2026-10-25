/*
 * DVD-Video: a title set's cells walked and checked (the cell checks, the
 * seamless-angle check, the program chain check), a title's cell list
 * (CellWalk, CellTrim), its segments and chapter places (dvd_cells.h). On
 * the title set's VOBUs (dvd_vobu.h: the VOBU records, the VOBU address
 * map, the navigation packs); what a
 * title set's cells found once is kept here (DVDCells), and the lines go
 * into the log the way the DVD libraries' do ("DVD: ").
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
#include <stdlib.h>
#include <string.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_types.h>
#include "dvd_vob.h"

#include "base/err.h"
#include "base/mem.h"

#include "dvd_cells.h"
#include "dvd_internal.h"

#define SECTOR      DVD_VIDEO_LB_LEN

/* a title set's cells' lines, as libdvdread's warning and debug lines */
#define Log1( v, ... ) ff_dvd_lib_log( (v)->d, DVD_LOGGER_LEVEL_WARN, __VA_ARGS__ )
#define Log3( v, ... ) ff_dvd_lib_log( (v)->d, DVD_LOGGER_LEVEL_DEBUG, __VA_ARGS__ )

/* The first of the map's VOBU starts at or after sector. */
static int map_lower( const uint32_t *m, int nb, uint32_t sector )
{
  int lo = 0, hi = nb;
  while( lo < hi ) {
    int mid = (lo + hi) / 2;
    if( m[mid] < sector )
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

/* ---- the title sets' cells ---- */

int ff_dvd_cells_open( DVDDisc *d )
{
  for( int t = 1; t < ff_dvd_nb_sets( d ) && t <= DVD_MAX_TITLE_SETS; t++ ) {
    const dvd_ifo_rec_t *f = &d->rec->ifos[t];
    const pgcit_rec_t *pgcit = f->pgcit;
    uint8_t **state = NULL;

    /* its VOBU address map as libdvdread read and checked it (a title set
     * that could not be read: none, trusted) */
    if( f->ifo ) {
      d->cells[t].map            = f->vobu_map;
      d->cells[t].nb_map         = f->nb_vobu_map;
      d->cells[t].map_distrusted = !f->map_trusted;
    }

    if( pgcit && pgcit->nr_of_pgcs ) {
      if( !(state = dr_calloc( pgcit->nr_of_pgcs, sizeof(*state) )) )
        return DR_ERR(ENOMEM);
      for( int p = 0; p < pgcit->nr_of_pgcs; p++ )
        if( pgcit->pgcs[p] && pgcit->pgcs[p]->nr_of_cells &&
            !(state[p] = dr_calloc( pgcit->pgcs[p]->nr_of_cells, 1 )) ) {
          while( p-- )
            dr_free( state[p] );
          dr_free( state );
          return DR_ERR(ENOMEM);
        }
    }
    d->cells[t].pgcit      = pgcit;
    d->cells[t].cell_state = state;
  }
  return 0;
}

void ff_dvd_cells_close( DVDDisc *d )
{
  /* the title VOBs' readings first (a damaged range still open is said) */
  for( int t = 1; t <= DVD_MAX_TITLE_SETS; t++ ) {
    DVDCloseTitleVOBs_rec( d->cells[t].own );
    d->cells[t].own = NULL;
  }
  for( int t = 1; t <= DVD_MAX_TITLE_SETS; t++ ) {
    DVDCells *c = &d->cells[t];
    for( int p = 0; c->cell_state && p < c->pgcit->nr_of_pgcs; p++ )
      dr_free( c->cell_state[p] );
    dr_free( c->cell_state );
    dr_free( c->vobus.recs );
    memset( c, 0, sizeof(*c) );
  }
  dr_freep( &d->vobus.recs );
  d->vobus.nb = d->vobus.alloc = 0;
}

DVDCells *ff_dvd_cells( DVDDisc *d, int vts )
{
  dvd_vob_reader_t *r;
  DVDCells *c;

  if( vts < 1 || vts > DVD_MAX_TITLE_SETS || !(r = ff_dvd_own_reading( d, vts )) )
    return NULL;
  c = &d->cells[vts];
  c->d      = d;
  c->r      = r;
  c->title  = vts;
  c->blocks = DVDTitleVOBBlocks_rec( r );
  return c;
}

/* ---- cells ---- */

#define CELL_FLAGS(c)   ((c)->category[0]) /* block_mode 0xc0, block_type 0x30, seamless_play 8, interleaved 4,
                                              stc_discontinuity 2, seamless_angle 1 */
#define CELL_CMD_NR(c)  ((c)->category[3]) /* cell_cmd_nr */
#define CHECK_SAMPLE    50                 /* VOBUs a full check reads when it does not read them all */
#define SEAMLESS_CHECKED 1                 /* cell state: the seamless-angle check ran ... */
#define SEAMLESS_OK      2                 /* ... and passed */

void DVDSetCellCheck_rec( DVDDisc *d, dvd_cell_check_t mode, uint64_t seed )
{
  if( !d )
    return;
  d->cell_check = mode;
  d->cell_rng   = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

/* The next number of the random sample (xorshift64*). */
static uint64_t cell_random( DVDDisc *d )
{
  uint64_t x = d->cell_rng ? d->cell_rng : 0x9e3779b97f4a7c15ULL;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  d->cell_rng = x;
  return x * 0x2545f4914f6cdd1dULL;
}

static const pgc_rec_t *pgc_of( const DVDCells *v, int pgcn )
{
  const pgcit_rec_t *t = v->pgcit;
  return t && pgcn >= 0 && pgcn < t->nr_of_pgcs ? t->pgcs[pgcn] : NULL;
}

static const cell_rec_t *cell_of( const DVDCells *v, int pgcn, int cell )
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

int DVDCellNavNextVobu_rec( DVDCells *v, int pgcn, int cell, uint32_t s, uint32_t *next, uint32_t *len )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  const dvd_vobu_rec_t *r;
  dvd_vobu_nav_t n;

  if( !c )
    return 0;
  if( !(r = DVDFindVobu_rec( v, s, NULL )) ) {
    if( !DVDReadVobuNav_rec( v, s, &n ) || !DVDRegisterVobu_rec( v, &n, s, 1, next, len ) ) {
      Log3( v, "VTS_%02d: no VOBU at sector %" PRIu32, v->title, s );
      return 0;
    }
    if( *next == DVD_VOBU_SELF ) {
      if( c->last_vobu_start_sector == s ) {
        *next = DVD_VOBU_END;
      } else {
        Log3( v, "VTS_%02d: the VOBU at %" PRIu32 " names itself as the next one", v->title, s );
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

int DVDCellSeamlessAngleOk_rec( DVDCells *v, int pgcn, int cell )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  uint8_t *state;
  uint32_t lvs, s, n, l;
  int ok = 0;

  if( !c || (CELL_FLAGS( c ) & 0xf4) )
    return 0;
  state = &v->cell_state[pgcn][cell];
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
    Log3( v, "VTS_%02d: a seamless angle cell's VOBUs do not follow each other", v->title );
  *state |= SEAMLESS_CHECKED | (ok ? SEAMLESS_OK : 0);
  return ok;
}

int DVDCellNextVobu_rec( DVDCells *v, int pgcn, int cell, uint32_t s, uint32_t *next, uint32_t *len )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  int trusted;
  uint32_t n;

  if( !c )
    return 0;
  if( !DVDVobuStart_rec( v, s ) ) {
    Log3( v, "VTS_%02d: sector %" PRIu32 " starts no VOBU", v->title, s );
    return 0;
  }
  trusted = DVDVobuMapTrusted_rec( v->d, v->title );
  if( s < c->last_vobu_start_sector && !(CELL_FLAGS( c ) & 0xf4) ) {
    int ok;
    if( !(CELL_FLAGS( c ) & 1) )
      ok = DVDNextVobu_rec( v, s, &n );
    else
      ok = trusted && DVDCellSeamlessAngleOk_rec( v, pgcn, cell ) && DVDNextVobu_rec( v, s, &n );
    if( ok && DVDVobuMapTrusted_rec( v->d, v->title ) ) {
      *next = n;
      *len  = n - s;
      return 1;
    }
  }
  return DVDCellNavNextVobu_rec( v, pgcn, cell, s, next, len );
}

int DVDCellVobuTimes_rec( DVDCells *v, int pgcn, int cell, uint32_t s, uint32_t *sp, uint32_t *ep,
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

int DVDCellDuration_rec( DVDCells *v, int pgcn, int cell, uint64_t *dur, int read_anyway )
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
static int cell_has_bad_vobu( DVDCells *v, const cell_rec_t *c )
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

int DVDCellCheckFailed_rec( DVDCells *v, int pgcn, int cell )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  dvd_cell_check_t mode = (dvd_cell_check_t)v->d->cell_check;
  uint32_t *list = NULL, s, s1, e1, s2, e2;
  int nb = 0, alloc = 0, failed = 1;

  if( !c || c->last_vobu_start_sector < c->first_sector || cell_has_bad_vobu( v, c ) )
    return 1;
  if( DVDVobuBad_rec( v, c->first_sector ) ||
      (c->last_vobu_start_sector != c->first_sector && DVDVobuBad_rec( v, c->last_vobu_start_sector )) )
    return 1;
  for( s = c->first_sector; s < c->last_vobu_start_sector; ) {
    if( nb == alloc ) {
      uint32_t *l = dr_realloc( list, (alloc = alloc ? 2 * alloc : 16) * sizeof(*l) );
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
      int at = (int)(cell_random( v->d ) % (uint64_t)left);
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
  Log3( v, "cell check, VTS_%02d program chain %d cell %d (%d VOBUs, %s): %s", v->title, pgcn + 1, cell + 1,
        nb, nb < CHECK_SAMPLE || mode == DVD_CHECK_ALL ? "all read" :
        mode == DVD_CHECK_SPREAD ? "50 evenly spread" : "50 at random", failed ? "failed" : "passed" );
  dr_free( list );
  return failed;
}

/* A long single cell made of tiny VOBUs of one size: filler. */
static int single_cell_fake( DVDCells *v, const cell_rec_t *c )
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

int DVDCellRejected_rec( DVDCells *v, int pgcn, int cell, int quick, int prev )
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

int DVDIlvuWalk_rec( DVDCells *v, uint32_t from, uint32_t to )
{
  const uint32_t *map;
  int nb_map;

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
    if( DVDVobuMap_rec( v, &map, &nb_map ) ) {
      /* the last VOBU of the unit just crossed, by the map: recorded */
      int at = map_lower( map, nb_map, from );
      if( at < nb_map && map[at] == from && at ) {
        uint32_t q = map[at - 1];
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
static int angle_block_extreme( DVDCells *v, const pgc_rec_t *p, int i, int highest )
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
      Log3( v, "VTS_%02d program chain %d: the angle block of cell %d has no end", v->title, p->number + 1,
            i + 1 );
      return i;
    }
  } else if( BLOCK_MODE( f ) == 3 ) {
    last = i;
    for( first = i - 1; first >= 0 && BLOCK_MODE( CELL_FLAGS( &p->cells[first] ) ) == 2; first-- )
      ;
    if( first < 0 || BLOCK_MODE( CELL_FLAGS( &p->cells[first] ) ) != 1 ) {
      Log3( v, "VTS_%02d program chain %d: the angle block of cell %d has no start", v->title, p->number + 1,
            i + 1 );
      return i;
    }
  } else {
    Log3( v, "VTS_%02d program chain %d: cell %d is inside an angle block", v->title, p->number + 1, i + 1 );
    return i;
  }
  for( int k = first; k <= last; k++ )
    if( highest ? p->cells[k].first_sector > p->cells[best].first_sector
                : p->cells[k].first_sector < p->cells[best].first_sector )
      best = k;
  return best;
}

int DVDPgcRejected_rec( DVDCells *v, int pgcn, int ov )
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

int DVDAngleCell_rec( DVDCells *v, int pgcn, int i, int angle, int angles )
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
    Log3( v, "VTS_%02d program chain %d cell %d: an angle cell that does not start an angle block", v->title,
          pgcn + 1, i + 1 );
  for( ;; ) {
    uint8_t g = CELL_FLAGS( &p->cells[k] );
    if( BLOCK_MODE( g ) == 3 ) {
      int size = k - i + 1, pick = angle < size - 1 ? angle : size - 1;
      if( size != angles )
        Log1( v, "VTS_%02d program chain %d: the angle block at cell %d has %d cells, the title %d angles",
              v->title, pgcn + 1, i + 1, size, angles );
      return i + pick < p->nr_of_cells ? i + pick : -1;
    }
    if( BLOCK_TYPE( g ) != 1 ) {
      Log1( v, "VTS_%02d program chain %d: broken angle block around cell %d", v->title, pgcn + 1, k + 1 );
      return -1;
    }
    if( k + 1 >= p->nr_of_cells )
      break;
    k++;
  }
  Log1( v, "VTS_%02d program chain %d: broken angle block around cell %d", v->title, pgcn + 1, i + 1 );
  return -1;
}

/* The cell after cell i (after its whole angle block), -1 when none. */
static int next_cell( DVDCells *v, const pgc_rec_t *p, int pgcn, int i )
{
  int k = i;

  if( !BLOCK_TYPE( CELL_FLAGS( &p->cells[i] ) ) )
    return i + 1 < p->nr_of_cells ? i + 1 : -1;
  for( ;; ) {
    uint8_t g = CELL_FLAGS( &p->cells[k] );
    if( BLOCK_MODE( g ) == 3 )
      return k + 1 < p->nr_of_cells ? k + 1 : -1;
    if( BLOCK_TYPE( g ) != 1 ) {
      Log1( v, "VTS_%02d program chain %d: broken angle block around cell %d", v->title, pgcn + 1, k + 1 );
      return -1;
    }
    if( k + 1 >= p->nr_of_cells )
      break;
    k++;
  }
  Log1( v, "VTS_%02d program chain %d: broken angle block around cell %d", v->title, pgcn + 1, i + 1 );
  return -1;
}

int DVDCellList_rec( DVDCells *v, int pgcn, int first, int last, int angle, int angles, int *cells, int *nb )
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
static int run_good( DVDCells *v, const pgc_rec_t *p, int pgcn, cell_run_t r )
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

/* all_runs: why the runs left cannot all be kept (dvd_cell_walk_t one_run),
 * 0 when they can. */
static int runs_one( const pgc_rec_t *p, const cell_run_t *runs, int n, dvd_cell_walk_t *res )
{
  for( int k = 1; k < n; k++ ) {
    if( runs[k].first <= runs[k - 1].last )
      return 1;
    if( runs[k].first - 1 > runs[k - 1].last &&
        run_seconds( p, (cell_run_t){ runs[k - 1].last + 1, runs[k].first - 1 } ) > 5 ) {
      res->gap_first = runs[k - 1].last;
      res->gap_last  = runs[k].first - 2;
      return 2;
    }
  }
  return 0;
}

int DVDCellWalk_rec( DVDCells *v, int pgcn, const uint8_t *played, int nb_played, int angle, int angles,
                     int all_runs, int *cells, int *nb, dvd_cell_walk_t *res )
{
  const pgc_rec_t *p = pgc_of( v, pgcn );
  cell_run_t *runs;
  int n = 0, ret, all = 0;

  *nb = 0;
  res->first = res->last = -1;
  res->nb_runs = 0;
  res->one_run = 0;
  res->gap_first = res->gap_last = -1;
  if( !p )
    return 0;
  if( !nb_played ) {
    Log3( v, "VTS_%02d program chain %d: the navigation played no cell", v->title, pgcn + 1 );
    return 0;
  }
  if( !(runs = dr_malloc( nb_played * sizeof(*runs) )) )
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
  /* all_runs: every run left, when they go forward over short skipped stretches */
  if( all_runs && n > 1 && !(res->one_run = runs_one( p, runs, n, res )) )
    all = 1;
  /* the longer end */
  while( !all && n > 1 ) {
    if( run_seconds( p, runs[0] ) < run_seconds( p, runs[n - 1] ) )
      memmove( runs, runs + 1, --n * sizeof(*runs) );
    else
      n--;
  }
  ret = 1;
  for( int k = 0; k < n && ret > 0; k++ ) {
    int got = 0;
    if( (unsigned)(runs[k].first - 1) >= p->nr_of_cells || (unsigned)(runs[k].last - 1) >= p->nr_of_cells ) {
      ret = 0;
      break;
    }
    if( !k )
      res->first = runs[k].first - 1;
    res->last = runs[k].last - 1;
    if( res->runs ) {
      res->runs[k][0] = runs[k].first - 1;
      res->runs[k][1] = runs[k].last - 1;
    }
    res->nb_runs = k + 1;
    ret = DVDCellList_rec( v, pgcn, runs[k].first - 1, runs[k].last - 1, angle, angles, cells + *nb, &got );
    *nb += got;
  }
  dr_free( runs );
  return ret;
}

/* ---- CellTrim: the cell list without the navigation ---- */

typedef struct {
  DVDCells         *v;
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
      Log3( T->v, "VTS_%02d program chain %d: cell %d lies after cell %d", T->v->title, T->pgcn + 1,
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
    Log3( T->v, "VTS_%02d program chain %d declares no length [untested on real discs]", T->v->title,
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
  DVDCells *v = T->v;
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
      Log3( v, "VTS_%02d program chain %d: a chapter without a cell", v->title, T->pgcn + 1 );
      return 0;
    }
    for( S = mid; S != first; S-- ) {
      if( S - 1 < 0 ) {
        Log3( v, "VTS_%02d program chain %d: no cell before cell %d", v->title, T->pgcn + 1, S + 1 );
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
      Log1( v, "VTS_%02d program chain %d: the middle chapter's cells lie in the last program: they cannot be "
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
        Log3( v, "VTS_%02d program chain %d: no cell before cell 1", v->title, T->pgcn + 1 );
        return 0;
      }
      D--;
    }
    x = pick_end( T, A, C, 0 );
    if( (y = program_entry( p, D, 0 )) < 0 ) {
      Log1( v, "VTS_%02d program chain %d: cell %d lies in no program [untested on real discs]", v->title,
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

int DVDCellTrim_rec( DVDCells *v, int pgcn, int nb_chapters, int first_cell, int middle_cell, int angle,
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
  if( !(q = dr_realloc( *p, (size_t)(*alloc = *alloc ? 2 * *alloc : 16) * elem )) )
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
static uint32_t segment_last_vobu( DVDCells *v, int pgcn, const dvd_segment_t *g )
{
  const dvd_extent_t *e = &g->extents[g->nb_extents - 1];
  const cell_rec_t *c = cell_of( v, pgcn, g->last_cell );
  uint32_t x = e->sector + e->count - 1;
  const uint32_t *map;
  int nb_map;

  if( DVDVobuMap_rec( v, &map, &nb_map ) ) {
    int at = map_lower( map, nb_map, x );
    if( at < nb_map && at )
      return map[at - 1];
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
  Log3( v, "VTS_%02d program chain %d: the last VOBU of a segment is not found (sector %" PRIu32 ")", v->title,
        pgcn + 1, x );
  return c->last_vobu_start_sector;
}

/* The presentation start / end of the VOBU at s (recorded, else read). */
static int vobu_times( DVDCells *v, uint32_t s, uint32_t *sp, uint32_t *ep )
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
static int segment_time( DVDCells *v, const dvd_segment_t *g, uint32_t s, int64_t *time )
{
  uint32_t s1, e1, s2, e2;

  *time = 0;
  if( !g->nb_extents || g->extents[0].sector == s )
    return 1;
  if( !vobu_times( v, g->extents[0].sector, &s1, &e1 ) ) {
    Log3( v, "VTS_%02d: no navigation pack at sector %" PRIu32, v->title, g->extents[0].sector );
    return 0;
  }
  if( !vobu_times( v, s, &s2, &e2 ) ) {
    Log3( v, "VTS_%02d: no navigation pack at sector %" PRIu32, v->title, s );
    return 0;
  }
  if( e1 < s1 || s2 < e1 || e2 < s2 )
    return 0;
  *time = e2 - s1;
  return 1;
}

/* A VOBU of the cell that needs reading one by one (angle blocks,
 * interleaved cells, a cell's last VOBU). */
static int vobu_needs_scan( DVDCells *v, int pgcn, int cell, uint32_t s )
{
  const cell_rec_t *c = cell_of( v, pgcn, cell );
  uint32_t n;

  if( s < c->last_vobu_start_sector && !(CELL_FLAGS( c ) & 0xf4) ) {
    if( CELL_FLAGS( c ) & 1 )
      return !(DVDVobuMapTrusted_rec( v->d, v->title ) && DVDCellSeamlessAngleOk_rec( v, pgcn, cell ) &&
               DVDNextVobu_rec( v, s, &n ));
    DVDNextVobu_rec( v, s, &n );
    return 0;
  }
  return 1;
}

/* The navigation is invalid inside a cell: where (the segments' spans so
 * far), recorded. 0, < 0 the segments' times are broken. */
static int nav_invalid( DVDCells *v, int pgcn, dvd_title_segments_t *o )
{
  uint64_t pos = 0;

  for( int k = 0; k < o->nb_segments; k++ ) {
    const dvd_segment_t *g = &o->segments[k];
    uint32_t last, s1, e1, s2, e2;
    if( !g->nb_extents || g->extents[0].sector == (last = segment_last_vobu( v, pgcn, g )) )
      continue;
    if( !vobu_times( v, g->extents[0].sector, &s1, &e1 ) || !vobu_times( v, last, &s2, &e2 ) ||
        s1 > e1 || e1 > s2 || s2 > e2 ) {
      Log3( v, "VTS_%02d program chain %d: segment %d: presentation times out of order", v->title, pgcn + 1,
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
    dr_free( o->segments[k].extents );
    dr_free( o->segments[k].runs );
  }
  dr_free( o->segments );
  dr_free( o->nav_invalid );
  memset( o, 0, sizeof(*o) );
}

int DVDTitleSegments_rec( DVDCells *v, int pgcn, const int *cells, int nb_cells, dvd_title_segments_t *o )
{
  const pgc_rec_t *p = pgc_of( v, pgcn );
  const uint32_t *map;
  int nb_map;

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
      if( DVDVobuMapTrusted_rec( v->d, v->title ) && DVDCellSeamlessAngleOk_rec( v, pgcn, cells[i] ) &&
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
        Log3( v, "VTS_%02d program chain %d: no VOBU after sector %" PRIu32, v->title, pgcn + 1, s );
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
        Log3( v, "VTS_%02d program chain %d: interleaved cell %d outside an angle block", v->title, pgcn + 1,
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
        if( e && DVDVobuMap_rec( v, &map, &nb_map ) ) {
          uint32_t len0 = e->ilvu_ea + 1, target = s + len0, n2, l2;
          int at = map_lower( map, nb_map, target );
          if( at < nb_map && map[at] == target && at ) {
            uint32_t q = map[at - 1];
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
  Log3( v, "VTS_%02d program chain %d: a VOBU outside the title VOBs", v->title, pgcn + 1 );
  return 0;
}

void DVDChapterPlaces_rec( DVDCells *v, int pgcn, const dvd_title_segments_t *o, dvd_chapter_place_t *ch,
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
        Log3( v, "VTS_%02d program chain %d: segment %d has no length", v->title, pgcn + 1, g + 1 );
      else
        c->time += tm;
    }
    if( !segment_time( v, &o->segments[seg], x, &tm ) )
      Log3( v, "VTS_%02d program chain %d: chapter %d has no time", v->title, pgcn + 1, k + 1 );
    else
      c->time += tm;
    prev = c;
  }
}
