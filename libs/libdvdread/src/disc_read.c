/*
 * A DVD-Video disc's structure read once and checked (dvdread/disc_read.h).
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
#include "dvdread/dvd_filesystem.h"
#include "dvdread/dvd_reader.h"
#include "dvdread/ifo_read.h"
#include "dvdread/vob_read.h"
#include "dvdread_internal.h"
#include "logger.h"

#define SECTOR          DVD_VIDEO_LB_LEN
#define MAX_TITLE_SETS  99
#define TT_SRPT_MAX     101             /* title table entries read */
#define TEXT_MAX        0x400           /* bytes of one text read (it ends at a TAB) */

/* VMGI_MAT / VTSI_MAT fields */
#define LAST_SECTOR     0x0c            /* vmg_last_sector / vts_last_sector */
#define NR_OF_TITLE_SETS 0x3e           /* VMG: vmg_nr_of_title_sets */
#define PROVIDER_ID     0x40            /* VMG: provider_identifier */
#define TT_SRPT         0xc4            /* VMG: tt_srpt; VTS: vtstt_vobs */
#define VTS_PTT_SRPT    0xc8
#define VTS_PGCIT       0xcc
#define TXTDT_MGI       0xd4            /* VMG: txtdt_mgi */
#define M_VOBU_ADMAP    0xdc            /* vmgm_vobu_admap / vtsm_vobu_admap */
#define VTS_VOBU_ADMAP  0xe4
#define HEADER_SIZE     0x400

static uint32_t be32( const uint8_t *p )
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16( const uint8_t *p )
{
  return (uint16_t)(p[0] << 8 | p[1]);
}

static void ifo_file( int index, char *buf, size_t size )
{
  if( index )
    snprintf( buf, size, "VTS_%02d_0.IFO", index );
  else
    snprintf( buf, size, "VIDEO_TS.IFO" );
}

/* A finding about information file index (-1: none) or its table. */
static void note( dvd_reader_t *ctx, dvd_report_level_t level, int index, const char *table, int64_t offset,
                  const char *fmt, ... )
{
  char file[16] = "", text[320];
  va_list ap;
  dvd_report_t r = { level, NULL, table, NULL, offset, -1, NULL, 0, text, 0, 0 };

  if( index >= 0 ) {
    ifo_file( index, file, sizeof(file) );
    r.file = file;
  }
  va_start( ap, fmt );
  vsnprintf( text, sizeof(text), fmt, ap );
  va_end( ap );
  DVDReadReport( ctx->priv, &ctx->logcb, &r );
}

/* ---- the title table ---- */

/* A title table field, 0 when it lies outside the table. */
static unsigned tt8( const dvd_disc_rec_t *d, uint32_t o )
{
  return o + 1 <= d->tt_srpt_size ? d->tt_srpt[o] : 0;
}

static unsigned tt16( const dvd_disc_rec_t *d, uint32_t o )
{
  return o + 2 <= d->tt_srpt_size ? be16( d->tt_srpt + o ) : 0;
}

static uint32_t tt32( const dvd_disc_rec_t *d, uint32_t o )
{
  return o + 4 <= d->tt_srpt_size ? be32( d->tt_srpt + o ) : 0;
}

/* Each entry with a title set, a title number in it and chapters names a
 * title set the disc has; the start sectors the entries give a title set
 * agree. 0, -1 when the disc must be refused. */
static int check_title_table( dvd_reader_t *ctx, dvd_disc_rec_t *d )
{
  int n = tt16( d, 0 );

  for( int i = 0; i < n; i++ ) {
    uint32_t o = 8 + 12 * i;
    unsigned tsn = tt8( d, o + 6 );
    uint32_t sec = tt32( d, o + 8 );

    if( !tt8( d, o + 6 ) || !tt8( d, o + 7 ) || !tt16( d, o + 2 ) )
      continue;                       /* without a title set, a title number in it or chapters */
    if( tsn > MAX_TITLE_SETS || (int)tsn > d->nr_of_title_sets ) {
      note( ctx, DVD_REPORT_ERROR, 0, "TT_SRPT", -1, "title %d of the title table is in title set %u, which the "
            "disc does not have (it has %d)", i + 1, tsn, d->nr_of_title_sets );
      return -1;
    }
    if( !d->title_set_start[tsn - 1] )
      d->title_set_start[tsn - 1] = sec;
    else if( d->title_set_start[tsn - 1] != sec )
      note( ctx, DVD_REPORT_WARN, 0, "TT_SRPT", -1, "the title table starts title set %u at sector %" PRIu32
            " (title %d) and at %" PRIu32, tsn, sec, i + 1, d->title_set_start[tsn - 1] );
  }
  return 0;
}

/* ---- text data ---- */

static void text_node_free( dvd_text_node_t *n )
{
  for( int i = 0; i < n->nb_texts; i++ )
    free( n->texts[i] );
  free( n->texts );
  free( n->text_kinds );
  free( n->children );
  free( n );
}

static void text_free( dvd_text_t *t )
{
  if( !t )
    return;
  for( int i = 0; i < t->nb_nodes; i++ )
    text_node_free( t->nodes[i] );
  free( t->nodes );
  free( t );
}

static int add_ptr( void ***array, int *nb, void *p )
{
  void **a = realloc( *array, (*nb + 1) * sizeof(*a) );
  if( !a )
    return -1;
  a[(*nb)++] = p;
  *array = a;
  return 0;
}

/* A text of kind under node n (the first of a kind is kept): 1, 0 when the
 * character set is not read (the text data is dropped), -1 out of memory. */
static int text_add( dvd_reader_t *ctx, dvd_text_node_t *n, uint8_t kind, uint8_t charset, const uint8_t *raw )
{
  const uint8_t *tab = memchr( raw, 0x09, TEXT_MAX + 1 );
  int len = tab - raw, k = 0, dropped = 0, pos;
  char **texts, *s;
  uint8_t *kinds;

  if( len && (charset | 0x10) != 0x11 ) {
    note( ctx, DVD_REPORT_INFO, 0, "TXTDT_MGI", -1, "text data in character set 0x%02x is not read", charset );
    return 0;
  }
  if( !(s = malloc( len + 1 )) )
    return -1;
  for( int i = 0; i < len; i++ ) {
    if( raw[i] >= 0x20 && raw[i] <= 0x7a )
      s[k++] = raw[i];
    else
      dropped++;
  }
  s[k] = 0;
  if( dropped )
    note( ctx, DVD_REPORT_INFO, 0, "TXTDT_MGI", -1, "text data item 0x%02x: %d byte(s) outside 0x20-0x7A dropped "
          "(kept: \"%s\")", kind, dropped, s );
  for( pos = 0; pos < n->nb_texts && n->text_kinds[pos] < kind; pos++ )
    ;
  if( pos < n->nb_texts && n->text_kinds[pos] == kind ) {
    free( s );                        /* the first text of a kind is kept */
    return 1;
  }
  if( !(texts = realloc( n->texts, (n->nb_texts + 1) * sizeof(*texts) )) ) {
    free( s );
    return -1;
  }
  n->texts = texts;
  if( !(kinds = realloc( n->text_kinds, n->nb_texts + 1 )) ) {
    free( s );
    return -1;
  }
  n->text_kinds = kinds;
  memmove( n->texts + pos + 1, n->texts + pos, (n->nb_texts - pos) * sizeof(*n->texts) );
  memmove( n->text_kinds + pos + 1, n->text_kinds + pos, n->nb_texts - pos );
  n->texts[pos]      = s;
  n->text_kinds[pos] = kind;
  n->nb_texts++;
  return 1;
}

static const char *text_of( const dvd_text_node_t *n, uint8_t kind )
{
  for( int i = 0; n && i < n->nb_texts; i++ )
    if( n->text_kinds[i] == kind )
      return n->texts[i];
  return NULL;
}

/* A field of the text data table, 0 when outside it. */
static unsigned tx8( const uint8_t *b, uint32_t size, uint64_t o )
{
  return o + 1 <= size ? b[o] : 0;
}

static unsigned tx16( const uint8_t *b, uint32_t size, uint64_t o )
{
  return o + 2 <= size ? be16( b + o ) : 0;
}

/* The text data tree of table b; *out NULL when it does not hold together.
 * 0, -1 out of memory. */
static int parse_text( dvd_reader_t *ctx, const uint8_t *b, uint32_t L, dvd_text_t **out )
{
  dvd_text_node_t root = { 0 }, *cur = &root;
  dvd_text_t *t;
  uint32_t X, cnt;
  uint8_t charset, raw[TEXT_MAX + 1];
  const char *why = NULL;
  int ret = 0;

  *out = NULL;
  if( L < 0x10 || !(b[0x0e] | b[0x0f]) )
    return 0;
  charset = tx8( b, L, 0x17 );
  X = L >= 0x1c ? be32( b + 0x18 ) : 0;
  if( X >= L ) {
    note( ctx, DVD_REPORT_WARN, 0, "TXTDT_MGI", -1, "text data: its language unit (byte %" PRIu32 ") lies outside "
          "it", X );
    return 0;
  }
  if( (uint64_t)tx16( b, L, (uint64_t)X + 2 ) + 1 + X > L ) {
    note( ctx, DVD_REPORT_WARN, 0, "TXTDT_MGI", -1, "text data: its language unit runs past its end" );
    return 0;
  }
  if( !(t = calloc( 1, sizeof(*t) )) )
    return -1;
  t->language[0] = tx8( b, L, 0x14 );
  t->language[1] = tx8( b, L, 0x15 );
  t->language[2] = tx8( b, L, 0x16 );
  for( int k = 0; k < 99; k++ )
    t->title_offsets[k] = tx16( b, L, (uint64_t)X + 6 + 2 * k );
  if( (uint64_t)X + 0xce > L || !(cnt = be16( b + X + 0xcc )) )
    goto drop;
  if( (uint64_t)X + 0xd1 > L || b[X + 0xd0] != 1 ) {
    why = "its first entry is not the disc's";
    goto drop;
  }
  for( uint32_t i = 0; i != cnt; i++ ) {
    uint64_t e = (uint64_t)X + 0xd0 + 4 * i;
    uint8_t kind = e + 1 <= L ? b[e] : 0;

    if( !kind ) {                     /* the end, when its offset is 0 */
      if( tx16( b, L, e + 2 ) )
        Log2( ctx, "VIDEO_TS.IFO: text data: entry %" PRIu32 " of kind 0 is not an end", i );
      break;
    }
    if( kind < 0x30 ) {
      dvd_text_node_t *n = calloc( 1, sizeof(*n) );
      if( !n || add_ptr( (void ***)&t->nodes, &t->nb_nodes, n ) < 0 ) {
        free( n );
        ret = -1;
        goto drop;
      }
      n->offset = 0xd0 + 4 * i;
      n->kind   = kind;
      while( kind <= cur->kind )
        cur = cur->parent;
      n->parent = cur;
      if( add_ptr( (void ***)&cur->children, &cur->nb_children, n ) < 0 ) {
        ret = -1;
        goto drop;
      }
      cur = n;
    } else {
      uint64_t s = (uint64_t)tx16( b, L, e + 2 ) + X + 0xcc;
      uint64_t len;
      int r;
      if( s >= L ) {
        why = "a text lies outside it";
        goto drop;
      }
      len = L - s < TEXT_MAX + 1 ? L - s : TEXT_MAX + 1;
      memcpy( raw, b + s, len );
      raw[TEXT_MAX] = 0x09;
      if( len < TEXT_MAX )
        memset( raw + len, 0x09, TEXT_MAX - len );
      if( (r = text_add( ctx, cur, kind, charset, raw )) <= 0 ) {
        ret = r;
        goto drop;
      }
    }
  }
  if( root.nb_children != 1 ) {
    why = "it has not exactly one disc entry";
    goto drop;
  }
  t->root = root.children[0];
  t->root->parent = NULL;
  free( root.children );
  *out = t;
  return 0;
drop:
  if( why )
    note( ctx, DVD_REPORT_WARN, 0, "TXTDT_MGI", -1, "text data: %s: it is not used", why );
  free( root.children );
  text_free( t );
  return ret < 0 ? -1 : 0;
}

/* ---- information files ---- */

/* An image: the application's file system has the whole image. */
static int is_image( const dvd_reader_t *ctx )
{
  return ctx->fs && ctx->fs->image_open;
}

static int64_t file_sector( dvd_reader_t *ctx, const char *name )
{
  char path[48];
  dvdstat_t st;

  snprintf( path, sizeof(path), "/VIDEO_TS/%s", name );
  return DVDStatFile_( ctx, path, &st ) < 0 ? -1 : st.sector;
}

static int64_t file_size( dvd_reader_t *ctx, const char *name )
{
  char path[48];
  dvdstat_t st;

  snprintf( path, sizeof(path), "/VIDEO_TS/%s", name );
  return DVDStatFile_( ctx, path, &st ) < 0 ? -1 : (int64_t)st.size;
}

/* Information file index opened (ifoOpen_rec()), with where its files are
 * and where its VOBs start. -1 when it cannot be read. */
static int load_ifo( dvd_reader_t *ctx, int index, dvd_ifo_rec_t *f )
{
  uint8_t header[HEADER_SIZE];
  const uint8_t *cache;
  char name[16];
  uint32_t pgcit;
  int cache_size;

  if( !(f->ifo = ifoOpen_rec( ctx, index )) || !ifoRead_rec_bytes( f->ifo, 0, header, sizeof(header) ) )
    return -1;
  memcpy( f->header, header, sizeof(f->header) );
  ifoGet_rec_state( f->ifo, &f->bup_only, &cache, &cache_size );
  f->whole = cache != NULL;
  if( is_image( ctx ) ) {
    int64_t bup;
    snprintf( name, sizeof(name), index ? "VTS_%02d_0.IFO" : "VIDEO_TS.IFO", index );
    f->ifo_sector = file_sector( ctx, name );
    snprintf( name, sizeof(name), index ? "VTS_%02d_0.BUP" : "VIDEO_TS.BUP", index );
    bup = file_sector( ctx, name );
    f->bup_sector = bup >= 0 ? bup : f->ifo_sector;
    f->title_vob_sectors = (uint32_t)(f->bup_sector - f->ifo_sector) - be32( header + TT_SRPT );
    f->menu_vob_start = DVDMenuVOBStart_rec( f->ifo, index );
  } else {
    int64_t bytes = 0, n;
    if( !index ) {
      n = file_size( ctx, "VIDEO_TS.VOB" );
      bytes = n > 0 ? n : 0;
    }
    for( int part = 1; index && part <= 9; part++ ) {
      snprintf( name, sizeof(name), "VTS_%02d_%d.VOB", index, part );
      if( (n = file_size( ctx, name )) < 0 )
        break;
      bytes += n;
    }
    f->title_vob_sectors = (uint32_t)(bytes / SECTOR);
  }
  if( !index )
    return 0;
  pgcit = be32( header + VTS_PGCIT );
  if( pgcit == 0 || pgcit == 0xffffffff ) {
    Log2( ctx, "VTS_%02d_0.IFO has no program chain table", index );
    f->no_pgcit = 1;
    return 0;
  }
  if( is_image( ctx ) )
    f->title_vob_start = DVDTitleVOBStart_rec( f->ifo, index );
  return 0;
}

/* A VOBU address map at sector: 1 read, 0 none (sector 0) or not readable
 * (then *trusted is cleared), -1 out of memory. */
static int load_vobu_map( dvd_reader_t *ctx, dvd_ifo_rec_t *f, int index, uint32_t sector, const char *table,
                          uint32_t **map, int *nb, int *trusted )
{
  if( !sector )
    return 0;
  if( !ifoRead_VOBU_ADMAP_rec( f->ifo, sector, table, map, nb ) ) {
    note( ctx, DVD_REPORT_WARN, index, table, -1, "the VOBU address map at sector %" PRIu32 " cannot be read: its "
          "VOBU starts are not trusted", sector );
    *map = NULL;
    *nb = 0;
    *trusted = 0;
    return 0;
  }
  return 1;
}

static int on_map( const dvd_ifo_rec_t *f, uint32_t sector )
{
  int lo = 0, hi = f->nb_vobu_map;
  while( lo < hi ) {
    int mid = (lo + hi) / 2;
    if( f->vobu_map[mid] < sector )
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo < f->nb_vobu_map && f->vobu_map[lo] == sector;
}

/* Every cell's first sector and last VOBU start on the VOBU address map;
 * the first that is not makes the map untrusted. */
static void check_cells( dvd_reader_t *ctx, dvd_ifo_rec_t *f, int index, const pgcit_rec_t *pgcit, int *trusted )
{
  for( int i = 0; i < pgcit->nr_of_pgcs; i++ ) {
    const pgc_rec_t *p = pgcit->pgcs[i];
    for( int c = 0; p && c < p->nr_of_cells; c++ ) {
      uint32_t miss = p->cells[c].first_sector;
      if( on_map( f, miss ) )
        miss = p->cells[c].last_vobu_start_sector;
      if( !on_map( f, miss ) ) {
        note( ctx, DVD_REPORT_WARN, index, "VTS_VOBU_ADMAP", -1, "program chain %d cell %d: sector %" PRIu32 " is "
              "no VOBU start of the VOBU address map: its VOBU starts are not trusted (the VOBs must be scanned)",
              i + 1, c + 1, miss );
        *trusted = 0;
        return;
      }
    }
  }
}

/* Information file index's tables. 0, -1 when a needed table cannot be
 * read (the file is left out), -2 out of memory. */
static int load_tables( dvd_reader_t *ctx, int index, dvd_ifo_rec_t *f )
{
  ifo_handle_t *h = f->ifo;
  uint8_t header[HEADER_SIZE];
  pgcit_rec_t *pgcit = NULL;
  uint32_t *menu_map = NULL;
  int nb_menu_map = 0, trusted = 1, ret = 0;

  if( !ifoRead_rec_bytes( h, 0, header, sizeof(header) ) )
    return -1;
  /* the menus' VOBU address map: its last VOBU start */
  if( load_vobu_map( ctx, f, index, be32( header + M_VOBU_ADMAP ), index ? "VTSM_VOBU_ADMAP" : "VMGM_VOBU_ADMAP",
                     &menu_map, &nb_menu_map, &trusted ) > 0 && nb_menu_map )
    f->menu_vobu_last = menu_map[nb_menu_map - 1];
  free( menu_map );
  if( !index || f->no_pgcit )
    goto end;
  if( !ifoRead_VTS_PGCIT_rec( h, &pgcit ) ) {
    note( ctx, DVD_REPORT_ERROR, index, "VTS_PGCIT", -1, "its program chain table (VTS_PGCIT) cannot be read" );
    return -1;
  }
  if( !pgcit ) {
    f->no_pgcit = 1;
    goto end;
  }
  /* the table to the reader, which walks and checks the cells, and checked
   * against the cell address table */
  if( DVDKeepPGCIT_rec( ctx, index, pgcit ) < 0 ) {
    ifoFree_VTS_PGCIT_rec( &pgcit );
    return -2;
  }
  f->pgcit = pgcit;
  ifoCheckCellAddresses_rec( h, pgcit );
  /* the chapter table as recorded */
  if( !ifoRead_table_rec( h, h->vtsi_mat->vts_ptt_srpt, 4, "VTS_PTT_SRPT", &f->ptt_srpt, &f->ptt_srpt_size ) ) {
    note( ctx, DVD_REPORT_ERROR, index, "VTS_PTT_SRPT", -1, "its chapter table (VTS_PTT_SRPT) cannot be read" );
    return -1;
  }
  /* the title VOBs' VOBU address map, checked against the cells */
  if( load_vobu_map( ctx, f, index, be32( header + VTS_VOBU_ADMAP ), "VTS_VOBU_ADMAP", &f->vobu_map, &f->nb_vobu_map,
                     &trusted ) > 0 )
    check_cells( ctx, f, index, pgcit, &trusted );
end:
  /* the VOBU address map and whether it is trusted, to the reader (which
   * answers the VOBU questions from here on) */
  if( index && DVDSetVobuMap_rec( ctx, index, f->vobu_map, f->nb_vobu_map, trusted ) < 0 )
    ret = -2;
  f->map_trusted = trusted;
  return ret;
}

static void close_ifo( dvd_ifo_rec_t *f )
{
  if( f->own )
    DVDCloseTitleVOBs_rec( f->own );
  if( f->ifo )
    ifoClose( f->ifo );
  free( f->ptt_srpt );
  free( f->vobu_map );
  memset( f, 0, sizeof(*f) );
}

/* ---- the disc ---- */

dvd_disc_rec_t *DVDOpenDisc_rec( dvd_reader_t *ctx )
{
  static const uint8_t name_items[] = { 0x30, 0x39, 0x38, 0x3a, 0x3b };
  dvd_disc_rec_t *d;
  dvd_ifo_rec_t *vmg;
  uint8_t header[HEADER_SIZE];
  uint32_t next, txt;
  int ret = 0;

  if( !ctx || !(d = calloc( 1, sizeof(*d) )) )
    return NULL;
  d->ctx = ctx;
  vmg = &d->ifos[0];

  /* the VMG */
  if( load_ifo( ctx, 0, vmg ) < 0 || !ifoRead_rec_bytes( vmg->ifo, 0, header, sizeof(header) ) ) {
    note( ctx, DVD_REPORT_ERROR, 0, NULL, -1, "VIDEO_TS.IFO cannot be read: no DVD-Video" );
    goto fail;
  }
  d->nr_of_title_sets = be16( header + NR_OF_TITLE_SETS );
  memcpy( d->provider_id, header + PROVIDER_ID, 32 );
  if( d->nr_of_title_sets > MAX_TITLE_SETS ) {
    note( ctx, DVD_REPORT_ERROR, 0, "VMGI_MAT", NR_OF_TITLE_SETS, "the disc has %d title sets, more than %d",
          d->nr_of_title_sets, MAX_TITLE_SETS );
    goto fail;
  }

  /* every title set's information file */
  for( int i = 1; i <= d->nr_of_title_sets; i++ )
    if( load_ifo( ctx, i, &d->ifos[i] ) < 0 ) {
      close_ifo( &d->ifos[i] );
      note( ctx, DVD_REPORT_ERROR, i, NULL, -1, "title set %d cannot be read: the disc is read without it", i );
    }

  /* the title table */
  if( !ifoRead_TT_SRPT_rec( vmg->ifo, TT_SRPT_MAX, &d->tt_srpt, &d->tt_srpt_size ) ) {
    note( ctx, DVD_REPORT_ERROR, 0, "TT_SRPT", -1, "the title table (TT_SRPT) cannot be read" );
    goto fail;
  }
  if( d->tt_srpt_size >= 8 && be16( d->tt_srpt ) > TT_SRPT_MAX )
    note( ctx, DVD_REPORT_WARN, 0, "TT_SRPT", -1, "the title table has %u entries: the first %d are read",
          be16( d->tt_srpt ), TT_SRPT_MAX );
  if( check_title_table( ctx, d ) < 0 )
    goto fail;

  /* the text data and the disc's name in it */
  if( (txt = be32( header + TXTDT_MGI )) ) {
    uint8_t *b = NULL;
    uint32_t size = 0;
    if( !ifoRead_table_rec( vmg->ifo, txt, 0x10, "TXTDT_MGI", &b, &size ) )
      note( ctx, DVD_REPORT_WARN, 0, "TXTDT_MGI", -1, "the text data at sector %" PRIu32 " cannot be read", txt );
    else
      ret = b ? parse_text( ctx, b, size, &d->text ) : 0;
    free( b );
    if( b && ret < 0 )
      goto fail;
  }
  for( size_t i = 0; d->text && i < sizeof(name_items) && !d->text_name; i++ ) {
    const char *s = text_of( d->text->root, name_items[i] );
    if( s && !(d->text_name = strdup( s )) )
      goto fail;
  }

  /* where each title set starts, against the title table */
  next = be32( header + LAST_SECTOR ) + 1;
  for( int i = 1; i <= d->nr_of_title_sets; i++ ) {
    dvd_ifo_rec_t *f = &d->ifos[i];
    if( f->ifo ) {
      uint8_t h[HEADER_SIZE];
      if( is_image( ctx ) && f->ifo_sector > 0 && vmg->ifo_sector > 0 )
        next = f->ifo_sector - vmg->ifo_sector;
      if( d->title_set_start[i - 1] && next != d->title_set_start[i - 1] )
        note( ctx, DVD_REPORT_WARN, i, NULL, -1, "title set %d starts at sector %" PRIu32 " by %s, at %" PRIu32
              " by the title table", i, next, is_image( ctx ) ? "its IFO's position" : "the headers of the sets "
              "before it", d->title_set_start[i - 1] );
      next += (ifoRead_rec_bytes( f->ifo, 0, h, sizeof(h) ) ? be32( h + LAST_SECTOR ) : 0) + 1;
    } else if( i < MAX_TITLE_SETS && d->title_set_start[i] ) {
      next = d->title_set_start[i];
    }
  }

  /* each information file's own tables */
  for( int i = 0; i <= d->nr_of_title_sets; i++ ) {
    dvd_ifo_rec_t *f = &d->ifos[i];
    if( !f->ifo )
      continue;
    if( (ret = load_tables( ctx, i, f )) == -2 )
      goto fail;
    if( ret < 0 && !i ) {
      note( ctx, DVD_REPORT_ERROR, 0, NULL, -1, "VIDEO_TS.IFO cannot be read: no DVD-Video" );
      goto fail;
    }
    if( ret < 0 ) {
      close_ifo( f );
      note( ctx, DVD_REPORT_ERROR, i, NULL, -1, "title set %d cannot be read: the disc is read without it", i );
    }
  }
  /* the video manager's copy of each title set's attributes against the
   * title set's own */
  for( int i = 1; i <= d->nr_of_title_sets; i++ )
    if( d->ifos[i].ifo )
      ifoCheckVtsAttributes_rec( vmg->ifo, d->ifos[i].ifo, i );
  /* every information file read once more by ifoOpen(), all its tables
   * and their checks */
  for( int i = 0; i <= d->nr_of_title_sets; i++ ) {
    ifo_handle_t *h;
    if( d->ifos[i].ifo && (h = ifoOpen( ctx, i )) )
      ifoClose( h );
  }
  return d;
fail:
  DVDCloseDisc_rec( d );
  return NULL;
}

dvd_vob_reader_t *DVDDiscTitleVOBs_rec( dvd_disc_rec_t *d, int title )
{
  dvd_ifo_rec_t *f;

  if( !d || title < 1 || title > d->nr_of_title_sets )
    return NULL;
  f = &d->ifos[title];
  if( !f->own_opened && f->ifo ) {
    f->own_opened = 1;
    f->own = DVDOpenTitleVOBs_rec( f->ifo, title );
  }
  return f->own;
}

/* The title VOB sectors of title set title on the disc ([start, end)). */
static int title_vob_range( const dvd_disc_rec_t *d, int title, uint32_t *start, uint32_t *end )
{
  uint8_t h[HEADER_SIZE];
  uint32_t tv, last, ifol, b;

  if( title < 1 || title > d->nr_of_title_sets || !d->ifos[title].ifo ||
      !ifoRead_rec_bytes( d->ifos[title].ifo, 0, h, sizeof(h) ) )
    return 0;
  tv   = be32( h + TT_SRPT );           /* vtstt_vobs */
  last = be32( h + LAST_SECTOR );
  ifol = be32( h + 0x1c ) & 0x1ffff;    /* vtsi_last_sector */
  if( last <= ifol || (b = last - ifol) <= tv )
    return 0;
  *start = d->title_set_start[title - 1] + tv;
  *end   = d->title_set_start[title - 1] + b;
  return 1;
}

int DVDDiscTitleVOBsOverlap_rec( const dvd_disc_rec_t *d, int title )
{
  uint32_t s, e, s2, e2;

  if( !d || !title_vob_range( d, title, &s, &e ) || d->nr_of_title_sets < 1 )
    return 0;
  for( int k = 1; k <= d->nr_of_title_sets; k++ )
    if( k != title && title_vob_range( d, k, &s2, &e2 ) && ((s <= s2 && s2 < e) || (s2 <= s && s < e2)) )
      return 1;
  return 0;
}

void DVDCloseDisc_rec( dvd_disc_rec_t *d )
{
  if( !d )
    return;
  for( int i = 0; i <= MAX_TITLE_SETS; i++ )
    close_ifo( &d->ifos[i] );
  free( d->tt_srpt );
  text_free( d->text );
  free( d->text_name );
  free( d );
}
