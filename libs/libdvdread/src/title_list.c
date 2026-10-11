/*
 * The title list of a DVD-Video disc (dvdread/title_list.h).
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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dvdread/disc_read.h"
#include "dvdread/ifo_read.h"
#include "dvdread/title_list.h"
#include "dvdread/vob_read.h"
#include "dvdread_internal.h"
#include "logger.h"

/* the list's lines: what a user should see, details, warnings, debug lines */
enum { DVD_NOTE_INFO, DVD_NOTE_DETAIL, DVD_NOTE_WARN, DVD_NOTE_DEBUG, DVD_NOTE_ERROR };

#define HEADER_SIZE         0x400
#define SUSPICIOUS_PGCS     80          /* program chains of a suspicious title set (more than) */
#define MAX_ANGLES          8
#define MAX_AUDIO           8           /* VTSI_MAT audio streams */
#define MAX_SUBP            32          /* VTSI_MAT subpicture streams */
#define VTS_VIDEO_ATTR      0x200       /* VTSI_MAT vts_video_attr */
#define VTS_AUDIO_COUNT     0x202       /* VTSI_MAT nr_of_vts_audio_streams */
#define VTS_AUDIO_ATTR      0x204       /* VTSI_MAT vts_audio_attr[8] */
#define VTS_SUBP_COUNT      0x254       /* VTSI_MAT nr_of_vts_subp_streams */
#define FAKE_SECONDS        300         /* a fake's real length differs by more than this ... */
#define FAKE_PERCENT        30          /* ... and more than this much of its declared length */
#define OVERLAP_GAP         0x400       /* sectors between two cells of a title on shared title VOBs (at most) */
#define COMPLEX_VOBUS       100         /* VOBUs to scan one by one before that is told (more than) */
#define NAV_INVALID_TOLD    14          /* "navigation invalid" warnings per title, then the debug log */

#define FLAGS(c)  ((c)->category[0])    /* block_mode 0xc0, block_type 0x30, seamless_play 8,
                                           interleaved 4, stc_discontinuity 2, seamless_angle 1 */

#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef struct {
  dvd_disc_rec_t            *disc;
  const dvd_nav_cells_t     *scan;
  int                        nb_scan;
  const dvd_title_options_t *o;
  dvd_title_list_t          *out;
  uint8_t                   *headers[100];      /* each title set's IFO header, read once */
  int                        suspicious[100];   /* 0 not decided, 1 its titles are left out, 2 they are not */
  int                        shared_mixed;      /* the last title built: not reached by the navigation, its cells
                                                   on shared title VOBs do not follow each other */
  const char                *no_cells;         /* the last title built: why it has no cells (NULL: it has) */
} titles_t;

/* A line of the list in the disc reader's log. */
static void note( titles_t *T, int level, const char *fmt, ... )
{
  static const dvd_logger_level_t levels[] = { DVD_LOGGER_LEVEL_NOTICE, DVD_LOGGER_LEVEL_INFO, DVD_LOGGER_LEVEL_WARN,
                                               DVD_LOGGER_LEVEL_DEBUG, DVD_LOGGER_LEVEL_ERROR };
  dvd_reader_t *ctx = T->disc->ctx;
  char text[512];
  va_list va;

  if( !ctx )
    return;
  va_start( va, fmt );
  vsnprintf( text, sizeof(text), fmt, va );
  va_end( va );
  DVDReadLog( ctx->priv, &ctx->logcb, levels[level], "%s", text );
}

static uint16_t rb16( const uint8_t *p )
{
  return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t rb32( const uint8_t *p )
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t bcd_seconds( uint32_t t )
{
  return ((t >> 24 & 0xf) + (t >> 28) * 10) * 3600 + ((t >> 16 & 0xf) + (t >> 20 & 0xf) * 10) * 60 +
         (t >> 8 & 0xf) + (t >> 12 & 0xf) * 10;
}

static void hms( char *buf, size_t size, uint32_t secs )
{
  snprintf( buf, size, "%" PRIu32 ":%02" PRIu32 ":%02" PRIu32, secs / 3600, secs / 60 % 60, secs % 60 );
}

/* Room for element n of *p (doubling, at least 16). */
static int grow( void **p, int *alloc, int n, size_t elem )
{
  int cap;
  void *q;

  if( n < *alloc )
    return 0;
  cap = MAX( 16, *alloc * 2 );
  if( !(q = realloc( *p, (size_t)cap * elem )) )
    return -1;
  *p = q;
  *alloc = cap;
  return 0;
}

/* ---- the disc ---- */

/* Title set vtsn's IFO header (NULL: the title set was not read). */
static const uint8_t *header( titles_t *T, unsigned vtsn )
{
  dvd_ifo_rec_t *f;

  if( vtsn > (unsigned)T->disc->nr_of_title_sets || !(f = &T->disc->ifos[vtsn])->ifo )
    return NULL;
  if( !T->headers[vtsn] && (T->headers[vtsn] = malloc( HEADER_SIZE )) &&
      !ifoRead_rec_bytes( f->ifo, 0, T->headers[vtsn], HEADER_SIZE ) )
    memset( T->headers[vtsn], 0, HEADER_SIZE );
  return T->headers[vtsn];
}

static int nb_pgcs( const titles_t *T, unsigned vtsn )
{
  const pgcit_rec_t *t = vtsn <= (unsigned)T->disc->nr_of_title_sets ? T->disc->ifos[vtsn].pgcit : NULL;
  return t ? t->nr_of_pgcs : 0;
}

/* Program chain pgcn (1-based) of title set vtsn, NULL when it is left out. */
static const pgc_rec_t *pgc_of( const titles_t *T, unsigned vtsn, unsigned pgcn )
{
  if( !pgcn || pgcn > (unsigned)nb_pgcs( T, vtsn ) )
    return NULL;
  return T->disc->ifos[vtsn].pgcit->pgcs[pgcn - 1];
}

static const pgc_rec_t *title_pgc( const titles_t *T, const dvd_title_t *t )
{
  return pgc_of( T, t->vtsn, t->pgcn + 1 );
}

static int overlap( const titles_t *T, int vtsn )
{
  return DVDDiscTitleVOBsOverlap_rec( T->disc, vtsn );
}

/* ---- VOBUs of a cell, cell checks: the title set's own reading ---- */

static dvd_vob_reader_t *reading( titles_t *T, int vtsn )
{
  return DVDDiscTitleVOBs_rec( T->disc, vtsn );
}

/* The cell of the title's angle in the angle block starting at cell i
 * (a cell in no block: itself); -1 when the block is broken. */
static int angle_cell( titles_t *T, int vtsn, const pgc_rec_t *p, int i, int angle, int angles )
{
  dvd_vob_reader_t *vr = reading( T, vtsn );
  return vr ? DVDAngleCell_rec( vr, p->number, i, angle, angles ) : -1;
}

/* The cell's real length in 90 kHz ticks: its last VOBU's end against its
 * first VOBU's start. */
static int cell_duration( titles_t *T, int vtsn, const pgc_rec_t *p, int i, uint64_t *dur, int read_anyway )
{
  dvd_vob_reader_t *vr = reading( T, vtsn );
  return vr && DVDCellDuration_rec( vr, p->number, i, dur, read_anyway );
}

/* The program chain is broken or a decoy: the reason (0-9), -1 when it
 * passes. */
static int pgc_rejected( titles_t *T, int vtsn, const pgc_rec_t *p )
{
  dvd_vob_reader_t *vr = reading( T, vtsn );
  return vr ? DVDPgcRejected_rec( vr, p->number, overlap( T, vtsn ) ) : 0;
}

/* ---- a title's cell list ---- */

/* The cells left out at the start and end of the title's program chain. */
static void say_left_out( titles_t *T, dvd_title_t *t, int first, int last )
{
  const pgc_rec_t *p = title_pgc( T, t );

  if( first )
    note( T, DVD_NOTE_INFO, "title %s: cells 1-%d left out at the start", t->name, first );
  if( last != p->nr_of_cells - 1 )
    note( T, DVD_NOTE_INFO, "title %s: cells %d-%d left out at the end", t->name, last + 2, p->nr_of_cells );
}

/* Room for the title's cells. */
static int cells_room( titles_t *T, dvd_title_t *t )
{
  int n = title_pgc( T, t )->nr_of_cells;
  int *c;

  t->nb_cells = 0;
  if( t->alloc_cells < n ) {
    if( !(c = realloc( t->cells, MAX( n, 1 ) * sizeof(*c) )) )
      return -1;
    t->cells       = c;
    t->alloc_cells = MAX( n, 1 );
  }
  return 0;
}

/* The cells the navigation played in the title's program chain: one run of
 * consecutive cells (CellWalk). 1 done, 0 not possible, -1 no memory. */
static int cell_walk( titles_t *T, dvd_title_t *t )
{
  const dvd_nav_cells_t *e = t->scan;
  const pgc_rec_t *p = title_pgc( T, t );
  dvd_vob_reader_t *vr = reading( T, t->vtsn );
  int first = -1, last = -1, ret;

  if( !e || !vr )
    return 0;
  if( e->nb_cells && ((p->number + 1) & 0xffff) != e->pgc )
    return 0;
  if( cells_room( T, t ) < 0 )
    return -1;
  ret = DVDCellWalk_rec( vr, p->number, e->cells, e->nb_cells, t->angle, t->nr_of_angles, t->cells, &t->nb_cells,
                         &first, &last );
  if( ret < 0 )
    return -1;
  if( first >= 0 )                    /* a run was chosen */
    say_left_out( T, t, first, last );
  return ret;
}

/* The cell list without the navigation: the program chain's cells, decoys
 * at the start and end trimmed (CellTrim). 1 done (no cells when none are
 * left), 0 failed, -1 no memory. */
static int cell_trim( titles_t *T, dvd_title_t *t )
{
  dvd_vob_reader_t *vr = reading( T, t->vtsn );
  int first = t->nb_chapters ? t->chapters[0].cell : -1;
  int mid   = t->nb_chapters ? t->chapters[t->nb_chapters / 2].cell : -1, ret;
  dvd_cell_trim_t res;

  if( !vr )
    return 0;
  if( cells_room( T, t ) < 0 )
    return -1;
  ret = DVDCellTrim_rec( vr, t->pgcn, t->nb_chapters, first, mid, t->angle, t->nr_of_angles, t->cells,
                         &t->nb_cells, &res );
  if( res.fake_percent >= 0 )
    note( T, DVD_NOTE_INFO, "title %s: fake cells make up %d%% of it: its cells are left out", t->name,
          res.fake_percent );
  if( res.first >= 0 )
    say_left_out( T, t, res.first, res.last );
  return ret;
}

/* ---- segments and chapter places ---- */

/* Each segment's cell runs as text. */
static int cell_maps( dvd_title_t *t )
{
  if( t->segs.nb_segments && !(t->cell_maps = calloc( t->segs.nb_segments, sizeof(*t->cell_maps) )) )
    return -1;
  for( int k = 0; k < t->segs.nb_segments; k++ ) {
    const dvd_segment_t *g = &t->segs.segments[k];
    size_t size = 3 + (size_t)g->nb_runs * 12, at = 0;
    char *s = malloc( size );

    if( !s )
      return -1;
    s[0] = 0;
    if( g->nb_runs > 1 )
      at += snprintf( s + at, size - at, "(" );
    for( int r = 0; r < g->nb_runs; r++ ) {
      if( g->runs[r][0] == g->runs[r][1] )
        at += snprintf( s + at, size - at, "%s%u", r ? "," : "", g->runs[r][0] );
      else
        at += snprintf( s + at, size - at, "%s%u-%u", r ? "," : "", g->runs[r][0], g->runs[r][1] );
    }
    if( g->nb_runs > 1 )
      snprintf( s + at, size - at, ")" );
    t->cell_maps[k] = s;
  }
  return 0;
}

/* A title with angles: its cells outside the angle blocks (those all its
 * angles play) as text, "1, 2"; none: NULL. */
static int shared_cells( titles_t *T, dvd_title_t *t )
{
  const pgc_rec_t *p = title_pgc( T, t );
  size_t size = 1 + (size_t)t->nb_cells * 5, at = 0;
  char *s;

  if( t->nr_of_angles < 2 || !p || !p->cells )
    return 0;
  if( !(s = malloc( size )) )
    return -1;
  s[0] = 0;
  for( int i = 0; i < t->nb_cells; i++ )
    if( t->cells[i] < p->nr_of_cells && (p->cells[t->cells[i]].category[0] >> 4 & 3) != 1 )
      at += snprintf( s + at, size - at, "%s%d", at ? ", " : "", t->cells[i] + 1 );
  if( !at ) {
    free( s );
    return 0;
  }
  t->shared_cells = s;
  return 0;
}

/* The chapters' places in the segments; unplaced ones dropped, a leading
 * one that cannot be placed dropped or (the next one not at the start) put
 * at the start. */
static int place_chapters( titles_t *T, dvd_title_t *t, dvd_vob_reader_t *vr )
{
  const pgc_rec_t *p = title_pgc( T, t );
  dvd_chapter_place_t *pl;
  int n;

  if( t->angle )
    for( int k = 0; k < t->nb_chapters; k++ )
      if( t->chapters[k].cell >= 0 )
        t->chapters[k].cell = angle_cell( T, t->vtsn, p, t->chapters[k].cell, t->angle, t->nr_of_angles );
  if( !t->nb_chapters )
    return 0;
  if( !(pl = malloc( t->nb_chapters * sizeof(*pl) )) )
    return -1;
  for( int k = 0; k < t->nb_chapters; k++ )
    pl[k].cell = t->chapters[k].cell;
  DVDChapterPlaces_rec( vr, t->pgcn, &t->segs, pl, t->nb_chapters );
  for( int k = 0; k < t->nb_chapters; k++ ) {
    dvd_title_chapter_t *ch = &t->chapters[k];
    ch->segment = pl[k].segment < 0 ? DVD_TITLE_CHAPTER_NONE : pl[k].segment;
    if( pl[k].segment >= 0 ) {
      ch->offset = pl[k].offset;
      ch->time   = pl[k].time;
    }
  }
  free( pl );
  n = t->nb_chapters;
  if( n > 2 ) {
    while( t->chapters[0].segment == DVD_TITLE_CHAPTER_NONE ) {
      if( t->chapters[1].segment != DVD_TITLE_CHAPTER_NONE ) {
        if( t->chapters[1].segment || t->chapters[1].offset ) {
          t->chapters[0].segment = 0;
          t->chapters[0].offset  = 0;
          t->chapters[0].time    = 0;
          t->chapters[0].cell    = t->cells[0];
        }
        break;
      }
      memmove( t->chapters, t->chapters + 1, --t->nb_chapters * sizeof(*t->chapters) );
      if( t->nb_chapters <= 2 )
        break;
    }
  }
  n = 0;
  for( int k = 0; k < t->nb_chapters; k++ )
    if( t->chapters[k].segment != DVD_TITLE_CHAPTER_NONE )
      t->chapters[n++] = t->chapters[k];
  t->nb_chapters = n;
  return 0;
}

/* The cells' VOBUs as extents in segments, then the chapters' places.
 * 1 done, 0 failed, -1 no memory. */
static int build_segments( titles_t *T, dvd_title_t *t )
{
  dvd_vob_reader_t *vr = reading( T, t->vtsn );
  dvd_title_segments_t o;
  int ret;

  if( !vr )
    return 0;
  ret = DVDTitleSegments_rec( vr, t->pgcn, t->cells, t->nb_cells, &o );
  if( o.counted && o.vobus_to_scan > COMPLEX_VOBUS )
    note( T, DVD_NOTE_INFO, "title %s: %u cells and %u VOBUs are read one by one (a complex multiplex); this takes "
          "a while", t->name, o.cells_to_scan, o.vobus_to_scan );
  for( int k = 0; k < o.nb_nav_invalid; k++ ) {
    uint32_t secs = o.nav_invalid[k];
    if( ++t->nav_invalid <= NAV_INVALID_TOLD )
      note( T, DVD_NOTE_WARN, "title %s: the navigation information is invalid around %" PRIu32 ":%02" PRIu32
            ":%02" PRIu32 " (check the output for audio / video sync)", t->name, secs / 3600, secs / 60 % 60,
            secs % 60 );
    else
      note( T, DVD_NOTE_DEBUG, "title %s: the navigation information is invalid around %" PRIu32 ":%02" PRIu32
            ":%02" PRIu32, t->name, secs / 3600, secs / 60 % 60, secs % 60 );
  }
  if( ret <= 0 ) {
    DVDFreeTitleSegments_rec( &o );
    return ret < 0 ? -1 : 0;
  }
  t->segs = o;                        /* the title's from here on */
  if( cell_maps( t ) < 0 || shared_cells( T, t ) < 0 || place_chapters( T, t, vr ) < 0 )
    return -1;
  t->built = 1;
  return 1;
}

/* Cells, length, segments and chapters of a title. 1 done (a title left
 * without segments is a fake or not the program chain's), 0 failed, -1 no
 * memory. */
static int build_title( titles_t *T, dvd_title_t *t )
{
  const pgc_rec_t *p = title_pgc( T, t );
  uint64_t real = 0;
  int mode = T->o->cell_mode, ret, suspect = 0;

  T->no_cells = NULL;
  if( !t->scan && pgc_rejected( T, t->vtsn, p ) >= 0 ) {
    t->nb_cells = 0;
    T->no_cells = "the navigation does not reach it and its program chain fails the program chain check";
  } else {
    if( !mode )
      mode = T->nb_scan ? DVD_CELL_MODE_WALK_REC : DVD_CELL_MODE_TRIM_REC;
    switch( mode ) {
    case DVD_CELL_MODE_WALK_REC:
      if( !t->scan ) {
        t->nb_cells = 0;
        T->no_cells = "the navigation does not reach this program chain";
        ret = 1;
      } else {
        ret = cell_walk( T, t );
      }
      break;
    case DVD_CELL_MODE_TRIM_REC:
      ret = cell_trim( T, t );
      break;
    default:
      if( t->scan && (ret = cell_walk( T, t )) != 0 )
        break;
      if( t->scan ) {
        note( T, DVD_NOTE_INFO, "title %s: the navigation's cells do not work (structure protection?): the cells "
              "are trimmed instead", t->name );
        if( pgc_rejected( T, t->vtsn, p ) >= 0 ) {
          t->nb_cells = 0;
          T->no_cells = "the navigation's cells do not work and its program chain fails the program chain check";
          ret = 1;
          break;
        }
      }
      ret = cell_trim( T, t );
    }
    if( ret <= 0 ) {
      if( !ret )
        note( T, DVD_NOTE_DEBUG, "title %s: no cell list", t->name );
      return ret;
    }
    if( !t->nb_cells && !T->no_cells )
      T->no_cells = "trimming its decoy cells leaves none";
  }

  /* length */
  t->declared = 0;
  for( int i = 0; i < t->nb_cells; i++ ) {
    uint64_t dur;
    t->declared += bcd_seconds( p->cells[t->cells[i]].playback_time );
    if( cell_duration( T, t->vtsn, p, t->cells[i], &dur, 0 ) )
      real += dur;
  }
  t->measured = t->nb_cells ? real / 90000 : 0;
  if( t->nb_cells ) {
    uint32_t diff = t->declared > t->measured ? t->declared - t->measured : t->measured - t->declared;
    if( !t->declared || (diff > FAKE_SECONDS && diff * 100 / t->declared > FAKE_PERCENT) )
      suspect = 1;
  }
  T->shared_mixed = 0;
  if( !suspect && t->nb_cells >= 2 && !t->scan && overlap( T, t->vtsn ) ) {
    /* on title VOBs shared with another title set: cells must follow each other */
    int over = 0, last = 0;
    for( int i = 1; i < t->nb_cells && !suspect; i++ ) {
      const cell_rec_t *c = &p->cells[t->cells[i]], *b = &p->cells[t->cells[i - 1]];
      if( FLAGS(c) & 8 )
        continue;
      if( c->first_sector <= b->last_sector ) {
        over++;
        last = i;
      } else if( c->first_sector - b->last_sector > OVERLAP_GAP ) {
        suspect = 1;
      }
    }
    if( !suspect && over >= 2 )
      suspect = 1;
    if( !suspect && over == 1 ) {
      int j = last, k;
      do {
        if( j == 1 ) {
          j = 0;
          break;
        }
        j--;
      } while( FLAGS(&p->cells[t->cells[j]]) & 8 );
      for( k = last; k < t->nb_cells - 1 && (FLAGS(&p->cells[t->cells[k + 1]]) & 8); k++ )
        ;
      if( k > t->nb_cells - 1 )
        k = t->nb_cells - 1;
      if( p->cells[t->cells[k]].last_sector >= p->cells[t->cells[j]].first_sector )
        suspect = 1;
    }
    T->shared_mixed = suspect;
  }
  if( suspect ) {
    note( T, DVD_NOTE_DEBUG, "title %s: declared %" PRIu32 " s, real %" PRIu32 " s: no segments", t->name,
          t->declared, t->measured );
    return 1;
  }
  if( !t->built && (ret = build_segments( T, t )) <= 0 )
    return ret;
  return 1;
}

/* ---- titles ---- */

void DVDFreeTitle_rec( dvd_title_t *t )
{
  if( !t )
    return;
  for( int k = 0; t->cell_maps && k < t->segs.nb_segments; k++ )
    free( t->cell_maps[k] );
  free( t->cell_maps );
  free( t->shared_cells );
  DVDFreeTitleSegments_rec( &t->segs );
  free( t->chapters );
  free( t->cells );
  free( t );
}

void DVDFreeTitleList_rec( dvd_title_list_t *l )
{
  for( int i = 0; i < l->nb_titles; i++ )
    DVDFreeTitle_rec( l->titles[i] );
  free( l->titles );
  l->titles    = NULL;
  l->nb_titles = 0;
}

static const dvd_nav_cells_t *scan_find( const titles_t *T, unsigned title, unsigned pgc )
{
  for( int i = 0; i < T->nb_scan; i++ )
    if( T->scan[i].title == (title & 0xff) && T->scan[i].pgc == (pgc & 0xff) )
      return &T->scan[i];
  return NULL;
}

/* the navigation reached title (1-based) in a program chain */
static int scan_reached( const titles_t *T, unsigned title )
{
  for( int i = 0; i < T->nb_scan; i++ )
    if( T->scan[i].title == (title & 0xff) && T->scan[i].pgc )
      return 1;
  return 0;
}

static unsigned tt_byte( titles_t *T, uint32_t o )
{
  if( !T->disc->tt_srpt || o + 1 > T->disc->tt_srpt_size ) {
    note( T, DVD_NOTE_DEBUG, "the title table ends before byte %" PRIu32, o );
    return 0;
  }
  return T->disc->tt_srpt[o];
}

/* The title's audio streams: those the program chain's audio_control
 * lists (present), in its order, then the title set's other ones. */
static void title_audio( titles_t *T, dvd_title_t *t, const uint8_t *h, const pgc_rec_t *p )
{
  unsigned n = rb16( h + VTS_AUDIO_COUNT ), mask = 0;

  t->nb_audio = 0;
  for( unsigned i = 0; i < n; i++ ) {
    uint8_t b = p->audio_control[2 * i];
    if( !(b & 0x80) )
      continue;
    mask |= 1 << (b & 7);
    t->audio[t->nb_audio++] = (dvd_stream_ref_t){ b & 7, i };
    if( (b & 7) != i )
      note( T, DVD_NOTE_DEBUG, "title %s: audio_control %u names stream %u", t->name, i, b & 7 );
  }
  for( unsigned k = 0; k < n; k++ )
    if( !(mask >> k & 1) )
      t->audio[t->nb_audio++] = (dvd_stream_ref_t){ k, k };
  t->audio_listed = mask;
}

/* The title's sub-picture streams and their canvas. */
static void title_subp( titles_t *T, dvd_title_t *t, const uint8_t *h, const pgc_rec_t *p )
{
  const uint8_t *v = h + VTS_VIDEO_ATTR;
  unsigned n = rb16( h + VTS_SUBP_COUNT ), ar = v[0] >> 2 & 3, standard = v[0] >> 4 & 3;

  if( standard == 0 ) {
    t->subp_width  = 720;
    t->subp_height = 480;
  } else if( standard == 1 ) {
    t->subp_width  = 720;
    t->subp_height = 576;
  } else {
    t->subp_width  = 352;
    t->subp_height = 480;
    note( T, DVD_NOTE_WARN, "VTS_%02d: video standard %u: the sub-pictures' canvas is taken as 352x480 [untested "
          "on real discs]", t->vtsn, standard );
  }
  t->nb_subp = 0;
  for( unsigned i = 0; i < n; i++ ) {
    const uint8_t *c = p->subp_control + 4 * i;
    uint8_t sid;
    if( !(c[0] & 0x80) )
      continue;
    if( ar == 0 ) {
      sid = c[0];
    } else if( ar == 3 ) {
      sid = c[1];
    } else {
      sid = c[0];
      note( T, DVD_NOTE_DEBUG, "VTS_%02d: aspect ratio code %u: the 4:3 sub-picture streams are used", t->vtsn, ar );
    }
    t->subp[t->nb_subp++] = (dvd_stream_ref_t){ sid & 0x1f, i };
    if( n == 1 && ar == 3 && (c[1] & 0x1f) == 1 && (c[2] & 0x1f) == 0 )
      t->subp[t->nb_subp++] = (dvd_stream_ref_t){ 0, i };
  }
}

/* A program chain's title of title table entry idx (0-based), angle a.
 * 1, 0 when it cannot be used, -1 no memory. */
static int title_init( titles_t *T, dvd_title_t *t, int idx, unsigned vtsn, const pgc_rec_t *p,
                       const dvd_title_chapter_t *ch, int nb_ch, int angle )
{
  uint32_t o = 8 + 12 * idx;
  const uint8_t *h = header( T, vtsn );
  unsigned angles;

  t->title   = idx;
  t->pb_ty   = tt_byte( T, o );
  t->vtsn    = tt_byte( T, o + 6 );
  t->vts_ttn = tt_byte( T, o + 7 );
  angles     = o + 2 <= T->disc->tt_srpt_size ? T->disc->tt_srpt[o + 1] : 1;
  if( !angles )
    angles = 1;
  if( angles > MAX_ANGLES ) {
    note( T, DVD_NOTE_WARN, "title %d has %u angles: only the first %d are read, the others are left out", idx + 1,
          angles, MAX_ANGLES );
    angles = MAX_ANGLES;
  }
  t->nr_of_angles = angles;
  if( angle >= (int)angles )
    return 0;
  t->angle = angle;
  t->vtsn  = vtsn;
  t->pgcn  = p->number;
  if( !(t->chapters = malloc( MAX( nb_ch, 1 ) * sizeof(*t->chapters) )) )
    return -1;
  memcpy( t->chapters, ch, nb_ch * sizeof(*ch) );
  t->nb_chapters = nb_ch;
  t->scan = scan_find( T, idx + 1, p->number + 1 );
  t->main_angle = 1;                  /* the angle block's first cell (the reference's way) */
  if( !h )
    return 0;
  if( rb16( h + VTS_AUDIO_COUNT ) > MAX_AUDIO ) {
    note( T, DVD_NOTE_WARN, "VTS_%02u has %u audio streams", vtsn, rb16( h + VTS_AUDIO_COUNT ) );
    return 0;
  }
  title_audio( T, t, h, p );
  if( rb16( h + VTS_SUBP_COUNT ) > MAX_SUBP ) {
    note( T, DVD_NOTE_WARN, "VTS_%02u has %u subpicture streams", vtsn, rb16( h + VTS_SUBP_COUNT ) );
    return 0;
  }
  title_subp( T, t, h, p );
  return 1;
}

static int list_add( dvd_title_list_t *l, dvd_title_t *t )
{
  dvd_title_t **a = realloc( l->titles, (l->nb_titles + 1) * sizeof(*a) );
  if( !a )
    return -1;
  a[l->nb_titles++] = t;
  l->titles = a;
  return 0;
}

static void title_skip( titles_t *T, unsigned vtsn, const pgc_rec_t *p )
{
  note( T, DVD_NOTE_DEBUG, "title skipped: VTS %u PGC %d", vtsn, p->number + 1 );
}

/* the cells' declared length differs from the real one: a fake */
static int fake_length( const dvd_title_t *t )
{
  uint32_t diff;
  if( !t->nb_cells )
    return 0;
  diff = t->declared > t->measured ? t->declared - t->measured : t->measured - t->declared;
  return !t->declared || (diff > FAKE_SECONDS && diff * 100 / t->declared > FAKE_PERCENT);
}

/* The program chain's main angle (angle 1, the angle block's first cell)
 * and the angles the navigation plays, in its order. */
static void main_angle_note( titles_t *T, const dvd_title_t *t )
{
  char order[64] = "";
  int n = 0, over = 0;

  for( ; t->scan && n < 9 && t->scan->angles[n]; n++ ) {
    snprintf( order + strlen( order ), sizeof(order) - strlen( order ), "%s%d", n ? ", " : "", t->scan->angles[n] );
    over |= t->scan->angles[n] > t->nr_of_angles;
  }
  if( over )
    note( T, DVD_NOTE_WARN, "title %s: the navigation plays angle(s) %s, the title has %d angles", t->name, order,
          t->nr_of_angles );
  if( n > 1 || (n == 1 && t->scan->angles[0] != 1) )
    note( T, DVD_NOTE_INFO, "title %s: angle 1 is the main angle (the angle block's first cell); the navigation "
          "plays angle(s) %s in that order", t->name, order );
  else
    note( T, DVD_NOTE_INFO, "title %s: angle 1 is the main angle (the angle block's first cell)", t->name );
}

/* The titles of one program chain of title table entry idx: the first
 * angle, then one per further angle. 0, -1 no memory. */
static int add_pgc_titles( titles_t *T, int idx, unsigned vtsn, const pgc_rec_t *p, const dvd_title_chapter_t *ch,
                           int nb_ch, int k )
{
  dvd_title_t *t;
  char d1[16], d2[16];
  int ret;

  if( !nb_ch )
    return 0;
  if( !(t = calloc( 1, sizeof(*t) )) )
    return -1;
  if( k )
    snprintf( t->name, sizeof(t->name), "%d/%d", idx + 1, k );
  else
    snprintf( t->name, sizeof(t->name), "%d", idx + 1 );
  if( (ret = title_init( T, t, idx, vtsn, p, ch, nb_ch, 0 )) <= 0 ) {
    if( !ret )
      note( T, DVD_NOTE_INFO, "title %s left out: its program chain cannot be used", t->name );
    title_skip( T, vtsn, p );
    goto drop;
  }
  /* shorter than the minimum length: left out (by the program chain's own
   * length before the title is built, by the title's after) */
  if( bcd_seconds( p->playback_time ) < (uint32_t)T->o->min_length ) {
    hms( d1, sizeof(d1), bcd_seconds( p->playback_time ) );
    note( T, DVD_NOTE_INFO, "title %s left out: its program chain lasts %s, less than the minimum length of %d s",
          t->name, d1, T->o->min_length );
    title_skip( T, vtsn, p );
    ret = 0;
    goto drop;
  }
  if( (ret = build_title( T, t )) <= 0 ) {
    if( !ret )
      note( T, DVD_NOTE_INFO, "title %s (VTS %u PGC %d) left out: navigation error", t->name, vtsn, p->number + 1 );
    title_skip( T, vtsn, p );
    goto drop;
  }
  if( !t->segs.nb_segments && !fake_length( t ) ) {
    if( !t->nb_cells )
      note( T, DVD_NOTE_INFO, "title %s (VTS %u PGC %d) left out: %s", t->name, vtsn, p->number + 1,
            T->no_cells ? T->no_cells : "no cells are left to play" );
    else if( T->shared_mixed )
      note( T, DVD_NOTE_INFO, "title %s (VTS %u PGC %d) left out: the navigation does not reach it and its cells on "
            "title VOBs shared with another title set do not follow each other (a fake title)", t->name, vtsn,
            p->number + 1 );
    else
      note( T, DVD_NOTE_INFO, "title %s (VTS %u PGC %d) left out: its cells hold no sectors that can be read",
            t->name, vtsn, p->number + 1 );
    title_skip( T, vtsn, p );
    ret = 0;
    goto drop;
  }
  hms( d1, sizeof(d1), t->declared );
  if( t->declared < (uint32_t)T->o->min_length ) {
    note( T, DVD_NOTE_INFO, "title %s left out: it lasts %s, less than the minimum length of %d s", t->name, d1,
          T->o->min_length );
    title_skip( T, vtsn, p );
    ret = 0;
    goto drop;
  }
  if( fake_length( t ) ) {
    hms( d2, sizeof(d2), t->measured );
    note( T, DVD_NOTE_INFO, "title %s left out: its declared length is %s, its real length %s (a fake title)",
          t->name, d1, d2 );
    title_skip( T, vtsn, p );
    ret = 0;
    goto drop;
  }
  note( T, DVD_NOTE_DETAIL, "title %s added (%d cell(s), %s)", t->name, t->nb_cells, d1 );
  if( (ret = list_add( T->out, t )) < 0 )
    goto drop;
  if( t->nr_of_angles > 1 )
    main_angle_note( T, t );
  for( int a = 1; a < t->nr_of_angles; a++ ) {
    dvd_title_t *ta = calloc( 1, sizeof(*ta) );
    if( !ta )
      return -1;
    memcpy( ta->name, t->name, sizeof(ta->name) );
    if( (ret = title_init( T, ta, idx, vtsn, p, ch, nb_ch, a )) < 0 || (ret > 0 && (ret = build_title( T, ta )) < 0) ) {
      DVDFreeTitle_rec( ta );
      return -1;
    }
    if( !ret || !ta->segs.nb_segments ) {
      note( T, DVD_NOTE_WARN, "title %s: angle %d cannot be added", t->name, a + 1 );
      DVDFreeTitle_rec( ta );
      return 0;
    }
    if( list_add( T->out, ta ) < 0 ) {
      DVDFreeTitle_rec( ta );
      return -1;
    }
    note( T, DVD_NOTE_INFO, "title %s: angle %d added", t->name, a + 1 );
  }
  return 0;
drop:
  DVDFreeTitle_rec( t );
  return ret < 0 ? -1 : 0;
}

/* The chapters of title table entry idx: its PTT entries, each at its
 * program's entry cell (a program chain the entries skip gets chapter 1).
 * 0 none (said), 1 done, -1 no memory. */
static int title_chapters( titles_t *T, int idx, unsigned f, dvd_title_chapter_t **out, int *nb_out )
{
  const uint8_t *data = T->disc->ifos[f].ptt_srpt;
  uint32_t size = data ? T->disc->ifos[f].ptt_srpt_size : 0, start, end;
  unsigned vtsn = tt_byte( T, 8 + 12 * idx + 6 ), ttn = tt_byte( T, 8 + 12 * idx + 7 ), n;
  dvd_title_chapter_t *ch = NULL;
  int nb = 0, alloc = 0;

  *out = NULL;
  *nb_out = 0;
  n = size >= 2 ? rb16( data ) : 0;
  if( !ttn || ttn > n ) {
    note( T, DVD_NOTE_WARN, "VTS_%02u has no title %u: title %d left out", vtsn, ttn, idx + 1 );
    return 0;
  }
  start = 4 * ttn + 8 <= size ? rb32( data + 4 * ttn + 4 ) : 0;
  if( ttn != n ) {
    end = 4 * ttn + 0xc <= size ? rb32( data + 4 * ttn + 8 ) : 0;
  } else {
    uint32_t lb = size >= 8 ? rb32( data + 4 ) : 0;
    end = lb + 1;
    if( size < 8 || (end & 3) == 1 ) {
      note( T, DVD_NOTE_WARN, "VTS_%02u_0.IFO: the chapter table's end is off by one: worked around", vtsn );
      end = lb;
    }
  }
  if( (start & 3) || end < start || (end & 3) || end - start <= 3 ) {
    note( T, DVD_NOTE_WARN, "VTS_%02u title %u: its chapter table (VTS_PTT_SRPT) is broken: title %d left out", vtsn,
          ttn, idx + 1 );
    return 0;
  }
  for( uint32_t k = 0; k < (end - start) >> 2; k++ ) {
    uint32_t at = start + 4 * k;
    unsigned pgcn = at + 2 <= size ? rb16( data + at ) : 0;
    unsigned pgn  = at + 4 <= size ? rb16( data + at + 2 ) : 0;
    unsigned from = nb && ch[nb - 1].pgcn < pgcn ? ch[nb - 1].pgcn + 1u : pgcn;
    for( unsigned q = from; q <= pgcn; q++ ) {
      unsigned g = q == pgcn ? pgn : 1;
      const pgc_rec_t *p = pgc_of( T, f, q );
      unsigned cell;
      if( !p || !g || g > p->nr_of_programs || !(cell = p->program_map[g - 1]) || cell > p->nr_of_cells ) {
        note( T, DVD_NOTE_WARN, "VTS_%02u title %u: no cell for program chain %u program %u", vtsn, ttn, q,
              p ? g : 0 );
        continue;
      }
      if( grow( (void **)&ch, &alloc, nb, sizeof(*ch) ) < 0 ) {
        free( ch );
        return -1;
      }
      ch[nb++] = (dvd_title_chapter_t){ .pgcn = q, .pgn = g, .cell = cell - 1, .segment = DVD_TITLE_CHAPTER_NONE };
    }
  }
  *out = ch;
  *nb_out = nb;
  return 1;
}

/* the title table entry has no title set, VTS title or chapters */
static int title_entry_empty( titles_t *T, int idx )
{
  uint32_t o = 8 + 12 * idx;
  unsigned ts = tt_byte( T, o + 6 ), ttn = tt_byte( T, o + 7 );
  unsigned ptts = o + 4 <= T->disc->tt_srpt_size ? rb16( T->disc->tt_srpt + o + 2 ) : 0;
  return !ts || !ttn || !ptts;
}

/* the title set's titles are decoys: shared title VOBs, many program chains,
 * each chain of several cells a new timeline at every cell or failing the
 * program chain check */
static int title_set_suspicious( titles_t *T, unsigned f )
{
  int n = nb_pgcs( T, f );

  if( T->suspicious[f] )
    return T->suspicious[f] == 1;
  T->suspicious[f] = 2;
  if( !overlap( T, f ) || n <= SUSPICIOUS_PGCS )
    return 0;
  for( int i = 0; i < n; i++ ) {
    const pgc_rec_t *p = T->disc->ifos[f].pgcit->pgcs[i];
    if( !p || p->nr_of_cells < 2 )
      continue;
    for( int c = 1; c < p->nr_of_cells; c++ )
      if( !(FLAGS(&p->cells[c]) & 2) ) {
        if( pgc_rejected( T, f, p ) < 0 )
          return 0;
        break;
      }
  }
  T->suspicious[f] = 1;
  return 1;
}

static int cmp_u16( const void *a, const void *b )
{
  return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

/* Title table entry idx (0-based). 0, -1 no memory. */
static int add_title( titles_t *T, int idx )
{
  unsigned vtsn = tt_byte( T, 8 + 12 * idx + 6 );
  dvd_title_chapter_t *ch = NULL, *sub = NULL;
  uint16_t *pgcns = NULL;
  int nb = 0, n_pgcs = 0, ret = 0;
  int mode = T->o->cell_mode ? T->o->cell_mode : T->nb_scan ? DVD_CELL_MODE_WALK_REC : DVD_CELL_MODE_TRIM_REC;

  if( mode == DVD_CELL_MODE_WALK_REC && !scan_reached( T, idx + 1 ) ) {
    note( T, DVD_NOTE_INFO, "title %d left out: not reached by the navigation", idx + 1 );
    return 0;
  }
  if( title_entry_empty( T, idx ) ) {
    note( T, DVD_NOTE_INFO, "title %d left out: its title table entry has no title set, VTS title or chapters",
          idx + 1 );
    return 0;
  }
  if( vtsn > (unsigned)T->disc->nr_of_title_sets )
    return 0;
  if( !T->disc->ifos[vtsn].ifo ) {
    note( T, DVD_NOTE_INFO, "title %d left out: its title set (VTS_%02u) could not be read", idx + 1, vtsn );
    return 0;
  }
  if( title_set_suspicious( T, vtsn ) ) {
    note( T, DVD_NOTE_INFO, "title %d left out: its title set (VTS_%02u) looks like decoys", idx + 1, vtsn );
    return 0;
  }
  if( (ret = title_chapters( T, idx, vtsn, &ch, &nb )) <= 0 )
    return ret;
  /* the program chains the chapters use, ascending */
  if( !(pgcns = malloc( MAX( nb, 1 ) * sizeof(*pgcns) )) || !(sub = malloc( MAX( nb, 1 ) * sizeof(*sub) )) ) {
    ret = -1;
    goto end;
  }
  for( int i = 0; i < nb; i++ ) {
    int seen = 0;
    for( int j = 0; j < n_pgcs && !seen; j++ )
      seen = pgcns[j] == ch[i].pgcn;
    if( !seen )
      pgcns[n_pgcs++] = ch[i].pgcn;
  }
  qsort( pgcns, n_pgcs, sizeof(*pgcns), cmp_u16 );
  for( int k = 0; k < n_pgcs; k++ ) {
    unsigned pgcn = pgcns[k];
    const pgc_rec_t *p = pgc_of( T, vtsn, pgcn );
    int ns = 0, why;
    if( !p )
      continue;
    why = pgc_rejected( T, vtsn, p );
    if( why >= 0 && !scan_find( T, idx + 1, pgcn ) ) {
      note( T, DVD_NOTE_INFO, "title %d: program chain %u left out: it fails the program chain check (reason %d)",
            idx + 1, pgcn, why );
      title_skip( T, vtsn, p );
      continue;
    }
    for( int i = 0; i < nb; i++ )
      if( ch[i].pgcn == pgcn )
        sub[ns++] = ch[i];
    if( (ret = add_pgc_titles( T, idx, vtsn, p, sub, ns, k )) < 0 )
      goto end;
  }
end:
  free( pgcns );
  free( sub );
  free( ch );
  return ret;
}

/* b plays a's sectors with a subset of its streams and no more chapters */
static int same_title( const titles_t *T, const dvd_title_t *a, const dvd_title_t *b )
{
  uint32_t ba = (uint32_t)T->disc->ifos[a->vtsn].title_vob_start, bb = (uint32_t)T->disc->ifos[b->vtsn].title_vob_start;

  if( a->segs.nb_segments != b->segs.nb_segments )
    return 0;
  for( int k = 0; k < a->segs.nb_segments; k++ ) {
    const dvd_segment_t *x = &a->segs.segments[k], *y = &b->segs.segments[k];
    if( x->nb_extents != y->nb_extents )
      return 0;
    for( int e = 0; e < x->nb_extents; e++ ) {
      const dvd_extent_t *p = &x->extents[e], *q = &y->extents[e];
      if( p->count != q->count )
        return 0;
      if( ba && bb ) {
        if( p->sector + ba != q->sector + bb )
          return 0;
      } else if( a->vtsn != b->vtsn || p->sector != q->sector ) {
        return 0;
      }
    }
  }
  for( int i = 0; i < b->nb_audio; i++ ) {
    int found = 0;
    for( int j = 0; j < a->nb_audio && !found; j++ )
      found = a->audio[j].stream == b->audio[i].stream;
    if( !found )
      return 0;
  }
  for( int i = 0; i < b->nb_subp; i++ ) {
    int found = 0;
    for( int j = 0; j < a->nb_subp && !found; j++ )
      found = a->subp[j].stream == b->subp[i].stream;
    if( !found )
      return 0;
  }
  return b->nb_chapters <= a->nb_chapters && (a->scan || !b->scan);
}

/* A title playing an earlier one's sectors with no more streams and
 * chapters is left out (said); the search starts over after each. */
static void remove_duplicates( titles_t *T )
{
  dvd_title_list_t *l = T->out;

  for( int i = 0; i < l->nb_titles; i++ ) {
    for( int j = 0; j < l->nb_titles; j++ ) {
      dvd_title_t *a = l->titles[i], *b = l->titles[j];
      if( a == b || !same_title( T, a, b ) )
        continue;
      note( T, DVD_NOTE_INFO, "title %d left out: it is the same as title %d (VTS_%02d)", b->title + 1,
            a->title + 1, b->vtsn );
      DVDFreeTitle_rec( l->titles[j] );
      memmove( l->titles + j, l->titles + j + 1, (l->nb_titles - j - 1) * sizeof(*l->titles) );
      l->nb_titles--;
      i = -1;
      break;
    }
  }
}

int DVDTitleList_rec( dvd_disc_rec_t *disc, const dvd_nav_cells_t *scan, int nb_scan, const dvd_title_options_t *o,
                      dvd_title_list_t *out )
{
  titles_t T = { disc, scan, nb_scan, o, out };
  unsigned n = disc->tt_srpt && disc->tt_srpt_size >= 2 ? rb16( disc->tt_srpt ) : 0;
  int ret = 0;

  memset( out, 0, sizeof(*out) );
  /* the full cell check's VOBUs */
  if( disc->ctx )
    DVDSetCellCheck_rec( disc->ctx, (dvd_cell_check_t)o->cell_check, o->seed );
  if( o->cell_mode < DVD_CELL_MODE_TRIM_REC ) {
    /* the titles the navigation reached, then the others */
    for( unsigned i = 0; i < n; i++ )
      if( scan_reached( &T, i + 1 ) && (ret = add_title( &T, i )) < 0 )
        goto end;
    for( unsigned i = 0; i < n; i++ )
      if( !scan_reached( &T, i + 1 ) && (ret = add_title( &T, i )) < 0 )
        goto end;
  } else {
    for( unsigned i = 0; i < n; i++ )
      if( (ret = add_title( &T, i )) < 0 )
        goto end;
  }
  remove_duplicates( &T );
end:
  if( ret < 0 )
    DVDFreeTitleList_rec( out );
  for( int i = 0; i < 100; i++ )
    free( T.headers[i] );
  return ret < 0 ? -1 : 0;
}

/* ---- the stream probe of a title ---- */

int DVDTitleStreams_rec( dvd_disc_rec_t *disc, dvd_title_t *t )
{
  dvd_title_options_t none = { 0 };
  titles_t T = { disc, NULL, 0, &none, NULL };
  dvd_probe_audio_t audio[8] = { { 0 } };
  const dvd_segment_t *g;
  const uint8_t *h;
  dvd_vob_reader_t *vr;
  uint32_t failed = 0;
  int64_t best = -1;
  int seg = 0, captions = 0, ret, n = t->nb_audio < 8 ? t->nb_audio : 8;

  t->audio_dropped = 0;
  t->captions = 0;
  if( !t->segs.nb_segments )
    return 1;
  for( int k = 0; k < t->segs.nb_segments; k++ )
    if( t->segs.segments[k].size > best ) {
      best = t->segs.segments[k].size;
      seg = k;
    }
  g = &t->segs.segments[seg];
  if( !(h = header( &T, t->vtsn )) ) {
    ret = -2;
    goto end;
  }
  for( int i = 0; i < n; i++ ) {
    audio[i].stream = t->audio[i].stream;
    audio[i].coding = h[VTS_AUDIO_ATTR + 8 * t->audio[i].attr] >> 5;
    audio[i].listed = t->audio_listed >> i & 1;
  }
  if( !(vr = reading( &T, t->vtsn )) )
    ret = -2;
  else
    ret = DVDProbeTitleStreams_rec( vr, g->extents, g->nb_extents, audio, n, &captions, &failed );
  if( ret < -1 ) {
    note( &T, DVD_NOTE_ERROR, "title %s: sector %" PRIu32 " of segment %d cannot be read: the title's streams "
          "cannot be found", t->name, failed, seg + 1 );
    goto end;
  }
  if( ret < 0 )
    goto end;
  if( (t->captions = captions) )
    note( &T, DVD_NOTE_DETAIL, "title %s: closed captions in the video: a caption track", t->name );
  for( int i = 0; i < n; i++ ) {
    int s = t->audio[i].stream, listed = audio[i].listed;

    if( audio[i].result == DVD_PROBE_UNKNOWN ) {
      note( &T, DVD_NOTE_WARN, "title %s: audio stream %d has coding mode %d, which is not known: left out", t->name,
            s, audio[i].coding );
      t->audio_dropped |= 1 << i;
      continue;
    }
    if( audio[i].result == DVD_PROBE_FOUND )
      continue;
    if( listed != (t->audio_listed >> s & 1) )
      note( &T, DVD_NOTE_DEBUG, "title %s: audio stream %d is audio track %d: whether audio_control lists it is taken "
            "from track %d's place (%s), not from the stream number (%s)", t->name, s, i, i,
            listed ? "listed" : "not listed", listed ? "not listed" : "listed" );
    if( audio[i].result == DVD_PROBE_NOT_LISTED ) {
      note( &T, DVD_NOTE_INFO, "title %s: audio stream %d has no data in the first 48 MiB and the program chain does "
            "not list it: left out", t->name, s );
      t->audio_dropped |= 1 << i;
    } else if( audio[i].result == DVD_PROBE_EMPTY ) {
      note( &T, DVD_NOTE_WARN, "title %s: audio stream %d (track %d) looks empty (no data in the first 200 MiB, the "
            "100 MiB from the middle and the last 100 MiB of segment %d): left out", t->name, s, i, seg + 1 );
      t->audio_dropped |= 1 << i;
    }
  }
  ret = 1;
end:
  free( T.headers[t->vtsn] );
  return ret;
}
