/*
 * This file is part of libdvdread.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 */

#include "config.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "dvdread/dvd_reader.h"
#include "logger.h"

#ifdef _WIN32
    #define TTY_DEVICE "CON"
#else
    #define TTY_DEVICE "/dev/tty"
#endif

void DVDReadLog( void *priv, const dvd_logger_cb *logcb,
                 dvd_logger_level_t level, const char *fmt, ... )
{
    va_list list;
    va_start(list, fmt);
    if(logcb && logcb->pf_log)
        logcb->pf_log(priv, level, fmt, list);
    else
    {
        static FILE *tty_stream = NULL;
        if (!tty_stream)
            tty_stream = fopen(TTY_DEVICE, "w");

        FILE *stream = (level == DVD_LOGGER_LEVEL_ERROR) ? stderr : tty_stream;

        /* no controlling terminal so fall back to stderr */
        if (!stream)
            stream = stderr;

        fprintf(stream, "libdvdread: ");
        vfprintf(stream, fmt, list);
        fprintf(stream, "\n");
    }
    va_end(list);
}

static dvd_logger_level_t report_log_level( dvd_report_level_t level )
{
    return level == DVD_REPORT_ERROR ? DVD_LOGGER_LEVEL_ERROR :
           level == DVD_REPORT_WARN  ? DVD_LOGGER_LEVEL_WARN : DVD_LOGGER_LEVEL_INFO;
}

/* ---- the findings book ---- */

#define FINDINGS_SAID 20                /* rule breaks of one file said as warnings */

typedef struct
{
  dvd_report_level_t level;
  char     file[16];
  char     table[24];
  char    *text;
  int64_t  offset, sector;
} finding_t;

struct dvd_findings_s
{
  finding_t *list;
  int        nb, alloc;
  int        summarized;              /* the findings counted in a summary so far */
};

dvd_findings_t *DVDFindingsNew( void )
{
  return calloc( 1, sizeof(dvd_findings_t) );
}

void DVDFindingsFree( dvd_findings_t *b )
{
  if( !b )
    return;
  for( int i = 0; i < b->nb; i++ )
    free( b->list[i].text );
  free( b->list );
  free( b );
}

int DVDFindingsAdd( dvd_findings_t *b, const dvd_report_t *r, char *line, size_t size, dvd_logger_level_t *level )
{
  const char *file = r->file ? r->file : "", *table = r->table ? r->table : "";
  char where[96], at[48] = "", bytes[64] = "";
  int breaks = 0;
  finding_t *f;

  for( int i = 0; i < b->nb; i++ ) {
    const finding_t *o = &b->list[i];
    if( o->level == r->level && o->offset == r->offset && o->sector == r->sector && !strcmp( o->file, file ) &&
        !strcmp( o->table, table ) && !strcmp( o->text, r->text ) )
      return 0;
    breaks += !strcmp( o->file, file ) && o->level != DVD_REPORT_INFO;
  }
  if( b->nb == b->alloc ) {
    int cap = b->alloc ? 2 * b->alloc : 32;
    finding_t *n = realloc( b->list, cap * sizeof(*n) );
    if( !n )
      return 0;
    b->list  = n;
    b->alloc = cap;
  }
  f = &b->list[b->nb];
  memset( f, 0, sizeof(*f) );
  if( !(f->text = strdup( r->text )) )
    return 0;
  f->level  = r->level;
  f->offset = r->offset;
  f->sector = r->sector;
  snprintf( f->file, sizeof(f->file), "%s", file );
  snprintf( f->table, sizeof(f->table), "%s", table );
  b->nb++;

  /* the line: where (file, table, byte offset, the bytes), then what */
  if( r->offset >= 0 )
    snprintf( at, sizeof(at), " at byte 0x%" PRIx64, r->offset );
  for( size_t k = 0, p = 0; r->bytes && k < r->nb_bytes && k < 16 && p + 4 < sizeof(bytes); k++ )
    p += snprintf( bytes + p, sizeof(bytes) - p, "%s%02x", k ? " " : " (bytes ", r->bytes[k] );
  if( bytes[0] )
    strcat( bytes, ")" );
  snprintf( where, sizeof(where), "%s%s%s%s%s", file, file[0] && table[0] ? " " : "", table, at, bytes );
  snprintf( line, size, "%s: %s%s%s", file[0] ? "IFO check" : "Navigation check", where, where[0] ? ": " : "",
            r->text );
  /* rule breaks as warnings, the first FINDINGS_SAID of a file (the rest
   * in the detail lines, counted in the summary); data that cannot be used
   * always; harmless ones (unused bytes not zero, kept) as notices */
  *level = r->level == DVD_REPORT_ERROR || (r->level != DVD_REPORT_INFO && breaks < FINDINGS_SAID) ?
           DVD_LOGGER_LEVEL_WARN : r->level == DVD_REPORT_INFO ? DVD_LOGGER_LEVEL_NOTICE : DVD_LOGGER_LEVEL_INFO;
  return 1;
}

/* The order of the files in a summary: VIDEO_TS, the title sets by number,
 * then the navigation's findings (no file). */
static int file_rank( const char *file )
{
  int n;
  if( !file[0] )
    return 1000;
  if( !strncmp( file, "VIDEO_TS", 8 ) )
    return 0;
  return sscanf( file, "VTS_%d", &n ) == 1 ? n : 999;
}

void DVDFindingsSummary( dvd_findings_t *b, void (*say)( void *priv, dvd_logger_level_t level, const char *line ),
                         void *priv )
{
  char line[256];

  if( !b )
    return;
  for( int rank = 0; rank <= 1000; rank++ ) {
    for( int i = b->summarized; i < b->nb; i++ ) {
      const char *file = b->list[i].file;
      const char *check = file[0] ? "IFO check" : "Navigation check";
      int harmless = 0, breaks = 0, done = 0;

      if( file_rank( file ) != rank )
        continue;
      for( int j = b->summarized; j < i && !done; j++ )
        done = !strcmp( b->list[j].file, file );
      if( done )
        continue;
      for( int j = i; j < b->nb; j++ )
        if( !strcmp( b->list[j].file, file ) ) {
          if( b->list[j].level == DVD_REPORT_INFO )
            harmless++;
          else
            breaks++;
        }
      if( breaks ) {
        if( breaks > FINDINGS_SAID )
          snprintf( line, sizeof(line), "%s: %s%s%d finding(s) that break the DVD-Video rules (the first %d above, "
                    "the other %d in the detail lines)", check, file, file[0] ? ": " : "", breaks, FINDINGS_SAID,
                    breaks - FINDINGS_SAID );
        else
          snprintf( line, sizeof(line), "%s: %s%s%d finding(s) that break the DVD-Video rules (above)", check, file,
                    file[0] ? ": " : "", breaks );
        say( priv, DVD_LOGGER_LEVEL_NOTICE, line );
      }
      if( harmless ) {
        snprintf( line, sizeof(line), "%s: %s%s%d harmless finding(s) (above): kept", check, file,
                  file[0] ? ": " : "", harmless );
        say( priv, DVD_LOGGER_LEVEL_NOTICE, line );
      }
    }
  }
  b->summarized = b->nb;
}

void DVDFindingsCount( const dvd_findings_t *b, int *harmless, int *breaks )
{
  *harmless = *breaks = 0;
  for( int i = 0; b && i < b->nb; i++ )
    if( b->list[i].level == DVD_REPORT_INFO )
      (*harmless)++;
    else
      (*breaks)++;
}

void DVDReadReport( void *priv, const dvd_logger_cb *logcb,
                    const dvd_report_t *report )
{
    int reported = logcb && logcb->pf_report;

    if( logcb && logcb->findings ) {
        char line[1024];
        dvd_logger_level_t level;
        if( report->nb_sectors )        /* damaged data: whoever reads says it */
            DVDReadLog( priv, logcb, DVD_LOGGER_LEVEL_DEBUG, "%s%s%s", report->file ? report->file : "",
                        report->file ? ": " : "", report->text );
        else if( DVDFindingsAdd( logcb->findings, report, line, sizeof(line), &level ) )
            DVDReadLog( priv, logcb, level, "%s", line );
        return;
    }

    if( reported )
        logcb->pf_report( priv, report );
    DVDReadLog( priv, logcb,
                reported ? DVD_LOGGER_LEVEL_DEBUG : report_log_level( report->level ),
                "%s%s%s", report->file ? report->file : "", report->file ? ": " : "",
                report->text );
}

void DVDReadReportf( void *priv, const dvd_logger_cb *logcb,
                     dvd_report_level_t level, const char *file,
                     const char *table, int64_t sector, const char *fmt, ... )
{
    char text[512];
    va_list list;
    dvd_report_t report = { level, file, table, NULL, -1, sector, NULL, 0, text };

    va_start( list, fmt );
    vsnprintf( text, sizeof(text), fmt, list );
    va_end( list );
    DVDReadReport( priv, logcb, &report );
}
