/*
 * Copyright (C) 2000, 2001, 2002, 2003
 *               Björn Englund <d4bjorn@dtek.chalmers.se>,
 *               Håkan Hjort <d95hjort@dtek.chalmers.se>
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

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include <stdarg.h>
#include "bswap.h"
#include "dvdread/ifo_types.h"
#include "dvdread/ifo_read.h"
#include "dvdread/dvd_reader.h"
#include "dvdread_internal.h"
#include "dvdread/bitreader.h"

#ifndef HAVE_STATIC_ASSERT
#if defined(_MSC_VER)
#include <crtdbg.h>
#define _Static_assert(x, y)  _STATIC_ASSERT(x)
#else
#define _Static_assert(x, y) // packed size checks not supported by this compiler
#endif
#endif

#define POINTER_SIZE  sizeof(void*)


#ifndef DVD_BLOCK_LEN
#define DVD_BLOCK_LEN 2048
#endif

#define PRIV(a) container_of(a, struct ifo_handle_private_s, handle)

/* Reports. Every read of the IFO goes through ifo_read(), which keeps where
 * its bytes lie in the file; a check that fails is reported (dvd_report_t)
 * with the file, the table (from the reading function), the field and its
 * byte offset in the file when its bytes come from such a read. */

static const uint8_t my_friendly_zeros[2048];

/* Returns the bytes read, 0 when not all of them could be read: every
 * caller tests for 0, while DVDReadBytes() gives a negative value for a read
 * error (which passed as success). */
static int rec_bytes(struct ifo_handle_private_s *ifop, int64_t pos, uint8_t *buf, size_t len);

/* The read position: of the file, or of the chosen bytes (ifoOpen_rec()). */
static int ifo_seek(struct ifo_handle_private_s *ifop, uint32_t offset)
{
  if(ifop->rec) {
    ifop->rec_pos = offset;
    return 1;
  }
  return DVDFileSeek(ifop->file, (int)offset) == (int)offset;
}

static int64_t ifo_tell(struct ifo_handle_private_s *ifop)
{
  return ifop->rec ? ifop->rec_pos : DVDFileTell_(ifop->file);
}

static ssize_t ifo_read(struct ifo_handle_private_s *ifop, void *data, size_t size)
{
  unsigned int k = ifop->nb_span++ % (sizeof(ifop->span) / sizeof(ifop->span[0]));
  ssize_t ret;

  ifop->span[k].data = data;
  ifop->span[k].size = size;
  ifop->span[k].pos  = ifo_tell(ifop);
  if(ifop->rec) {
    if(!rec_bytes(ifop, ifop->rec_pos, data, size))
      return 0;
    ifop->rec_pos += size;
    return size;
  }
  ret = DVDReadBytes(ifop->file, data, size);
  return ret == (ssize_t)size ? ret : 0;
}

/* Byte offset in the file of the data at p (the latest read holding it), -1. */
static int64_t ifo_offset(const struct ifo_handle_private_s *ifop, const void *p)
{
  const unsigned int n = sizeof(ifop->span) / sizeof(ifop->span[0]);

  for(unsigned int i = 0; i < n && i < ifop->nb_span; i++) {
    unsigned int k = (ifop->nb_span - 1 - i) % n;
    const uint8_t *d = ifop->span[k].data;
    if((const uint8_t *)p >= d && (const uint8_t *)p < d + ifop->span[k].size)
      return ifop->span[k].pos + ((const uint8_t *)p - d);
  }
  return -1;
}

/* The table a reading function reads ("ifoRead_VTS_PTT_SRPT" -> "VTS_PTT_SRPT"). */
static const char *ifo_table(const char *func, char *buf, size_t size)
{
  static const struct { const char *func, *table; } mat[] = {
    { "ifoRead_VMG", "VMGI_MAT" }, { "ifoRead_VTS", "VTSI_MAT" },
    { "ifoRead_AMG", "AMGI_MAT" }, { "ifoRead_ATS", "ATSI_MAT" },
  };
  const char *t = func;
  size_t len;

  for(size_t i = 0; i < sizeof(mat) / sizeof(mat[0]); i++)
    if(!strcmp(func, mat[i].func))
      return mat[i].table;
  if(!strncmp(t, "ifoRead_", 8))
    t += 8;
  len = strlen(t);
  if(len > 9 && !strcmp(t + len - 9, "_internal"))
    len -= 9;
  snprintf(buf, size, "%.*s", (int)len, t);
  return buf;
}

/* A failed check: zero = a field that must be zero (reserved or unused
 * bytes: harmless), else a rule that does not hold. */
static void ifo_check_failed(struct ifo_handle_private_s *ifop, const char *func,
                             const char *what, const void *p, size_t size,
                             int64_t value, int zero)
{
  dvd_reader_t *ctx = ifop->ctx;
  char table[40], text[320];
  uint8_t raw[16];
  size_t nb_raw = 0;
  int64_t off = p ? ifo_offset(ifop, p) : -1;
  dvd_report_t r = { 0 };

  /* the field's bytes as on the disc (the read ones may be converted in
   * place already): read again, the read position put back */
  if(off >= 0 && off <= INT32_MAX) {
    nb_raw = size < sizeof(raw) ? size : sizeof(raw);
    if(ifop->rec) {
      if(!rec_bytes(ifop, off, raw, nb_raw))
        nb_raw = 0;
    } else {
      int64_t at = DVDFileTell_(ifop->file);
      if(DVDFileSeek(ifop->file, (int32_t)off) < 0 || DVDReadBytes(ifop->file, raw, nb_raw) != (ssize_t)nb_raw)
        nb_raw = 0;
      DVDFileSeek(ifop->file, (int32_t)at);
    }
  }

  if(zero && p)
    snprintf(text, sizeof(text), "%s is not zero (must be zero): kept", what);
  else if(zero)
    snprintf(text, sizeof(text), "%s is 0x%" PRIx64 " (must be zero): kept", what, (uint64_t)value);
  else
    snprintf(text, sizeof(text), "%s does not hold: kept", what);
  r.level    = zero ? DVD_REPORT_INFO : DVD_REPORT_WARN;
  r.file     = ifop->name[0] ? ifop->name : NULL;
  r.table    = ifo_table(func, table, sizeof(table));
  r.field    = what;
  r.offset   = off;
  r.sector   = -1;
  r.bytes    = nb_raw ? raw : NULL;
  r.nb_bytes = nb_raw;
  r.text     = text;
  DVDReadReport(ctx->priv, &ctx->logcb, &r);
}

#define CHECK_VALUE(arg)\
  if(!(arg)) {\
    ifo_check_failed(ifop, __func__, # arg, NULL, 0, 0, 0);\
  }

/* a field without an address of its own (a bit field) */
#define CHECK_ZERO0(arg)                                                \
  if(arg != 0) {                                                        \
    ifo_check_failed(ifop, __func__, # arg, NULL, 0, (int64_t)(arg), 1);\
  }

#define CHECK_ZERO(arg)                                                 \
  if(memcmp(my_friendly_zeros, &arg, sizeof(arg))) {                    \
    ifo_check_failed(ifop, __func__, # arg, &arg, sizeof(arg), 0, 1);   \
  }

/* Findings of the reading itself (a table that cannot be read, ...): of the
 * IFO being read (ifop), or of the disc (ctx) */
#define IFO_REPORT(ifop, level, ...) \
  DVDReadReportf((ifop)->ctx->priv, &(ifop)->ctx->logcb, level, \
                 (ifop)->name[0] ? (ifop)->name : NULL, NULL, -1, __VA_ARGS__)
#define CTX_REPORT(ctx, level, ...) \
  DVDReadReportf((ctx)->priv, &(ctx)->logcb, level, NULL, NULL, -1, __VA_ARGS__)

/* Prototypes for internal functions */
static int ifoRead_VMG(ifo_handle_t *ifofile);
static int ifoRead_AMG(ifo_handle_t *ifofile);
/* Can be used to make simple dvd-a playback, no menus*/
static int ifoRead_SAMG(ifo_handle_t *ifofile);
/* for the still video set */
static int ifoRead_ASVS(ifo_handle_t *ifofile);

/* main DVD-VR structure */
static int ifoRead_RTAV_VMGI(ifo_handle_t *ifofile);

static int ifoRead_VTS(ifo_handle_t *ifofile);
static int ifoRead_ATS(ifo_handle_t *ifofile);
static int ifoRead_PGC(ifo_handle_t *ifofile, pgc_t *pgc, unsigned int offset);
static int ifoRead_PGC_COMMAND_TBL(ifo_handle_t *ifofile,
                                   pgc_command_tbl_t *cmd_tbl,
                                   unsigned int offset);
static int ifoRead_PGC_PROGRAM_MAP(ifo_handle_t *ifofile,
                                   pgc_program_map_t *program_map,
                                   unsigned int nr, unsigned int offset);
static int ifoRead_CELL_PLAYBACK_TBL(ifo_handle_t *ifofile,
                                     cell_playback_t *cell_playback,
                                     unsigned int nr, unsigned int offset);
static int ifoRead_CELL_POSITION_TBL(ifo_handle_t *ifofile,
                                     cell_position_t *cell_position,
                                     unsigned int nr, unsigned int offset);
static int ifoRead_VTS_ATTRIBUTES(ifo_handle_t *ifofile,
                                  vts_attributes_t *vts_attributes,
                                  unsigned int offset);
static int ifoRead_C_ADT_internal(ifo_handle_t *ifofile, c_adt_t *c_adt,
                                  unsigned int sector);
static int ifoRead_VOBU_ADMAP_internal(ifo_handle_t *ifofile,
                                       vobu_admap_t *vobu_admap,
                                       unsigned int sector);
static int ifoRead_PGCIT_internal(ifo_handle_t *ifofile, pgcit_t *pgcit,
                                  unsigned int offset);

static void ifoFree_PGC(pgc_t *pgc);
static void ifoFree_PGC_COMMAND_TBL(pgc_command_tbl_t *cmd_tbl);
static void ifoFree_PGCIT_internal(pgcit_t *pgcit);

static inline int DVDFileSeekForce_( dvd_file_t *dvd_file, uint32_t offset, int force_size ) {
  return (DVDFileSeekForce(dvd_file, (int)offset, force_size) == (int)offset);
}

static inline int DVDFileSeek_( dvd_file_t *dvd_file, uint32_t offset ) {
  return (DVDFileSeek(dvd_file, (int)offset) == (int)offset);
}

#define STRINGIFY_NAME( z )   STRINGIFY_NAME_( z )
#define STRINGIFY_NAME_( z ) #z

#define CHECK_STRUCT_SIZE(strt, ptrs, size) \
  _Static_assert(sizeof(strt)  <= size + ptrs * POINTER_SIZE,  STRINGIFY_NAME(strt) " bigger than " STRINGIFY_NAME(size)); \
  _Static_assert(sizeof(strt)  >= size + ptrs * POINTER_SIZE,  STRINGIFY_NAME(strt) " smaller than " STRINGIFY_NAME(size))

#define CHECK_STRUCT_SIZE_REF(strt, ptrs, size) \
  _Static_assert(sizeof(strt)  <= size + ptrs * POINTER_SIZE + sizeof(int),  STRINGIFY_NAME(strt) " bigger than " STRINGIFY_NAME(size)); \
  _Static_assert(sizeof(strt)  >= size + ptrs * POINTER_SIZE + sizeof(int),  STRINGIFY_NAME(strt) " smaller than " STRINGIFY_NAME(size))

/* size checks on packed structures */
CHECK_STRUCT_SIZE(dvd_time_t,             0, DVD_TIME_SIZE);
CHECK_STRUCT_SIZE(vm_cmd_t,               0, COMMAND_DATA_SIZE);
CHECK_STRUCT_SIZE(video_attr_t,           0, VIDEO_ATTR_SIZE);
CHECK_STRUCT_SIZE(audio_attr_t,           0, AUDIO_ATTR_SIZE);
CHECK_STRUCT_SIZE(multichannel_ext_t,     0, MULTICHANNEL_EXT_SIZE);
CHECK_STRUCT_SIZE(subp_attr_t,            0, SUBP_ATTR_SIZE);
CHECK_STRUCT_SIZE(pgc_command_tbl_t,      3, PGC_COMMAND_TBL_SIZE);
CHECK_STRUCT_SIZE(cell_playback_t,        0, CELL_PLAYBACK_SIZE);
CHECK_STRUCT_SIZE(cell_position_t,        0, CELL_POSITION_SIZE);
CHECK_STRUCT_SIZE(user_ops_t,             0, USER_OPS_SIZE);
CHECK_STRUCT_SIZE_REF(pgc_t,              4, PGC_SIZE);
CHECK_STRUCT_SIZE(pgci_srp_t,             1, PGCI_SRP_SIZE);
CHECK_STRUCT_SIZE_REF(pgcit_t,            1, PGCIT_SIZE);
CHECK_STRUCT_SIZE(pgci_lu_t,              1, PGCI_LU_SIZE);
CHECK_STRUCT_SIZE(pgci_ut_t,              1, PGCI_UT_SIZE);
CHECK_STRUCT_SIZE(cell_adr_t,             0, CELL_ADDR_SIZE);
CHECK_STRUCT_SIZE(c_adt_t,                1, C_ADT_SIZE);
CHECK_STRUCT_SIZE(vobu_admap_t,           1, VOBU_ADMAP_SIZE);
CHECK_STRUCT_SIZE(vmgi_mat_t,             0, VMGI_MAT_SIZE);

CHECK_STRUCT_SIZE(playback_type_t,        0, PLAYBACK_TYPE_SIZE);
CHECK_STRUCT_SIZE(title_info_t,           0, TITLE_INFO_SIZE);
CHECK_STRUCT_SIZE(tt_srpt_t,              1, TT_SRPT_SIZE);
CHECK_STRUCT_SIZE(ptl_mait_country_t,     1, PTL_MAIT_COUNTRY_SIZE);
CHECK_STRUCT_SIZE(ptl_mait_t,             1, PTL_MAIT_SIZE);
CHECK_STRUCT_SIZE(vts_attributes_t,       0, VTS_ATTRIBUTES_SIZE);

CHECK_STRUCT_SIZE(vts_atrt_t,             2, VTS_ATRT_SIZE);
CHECK_STRUCT_SIZE(txtdt_t,                0, TXTDT_SIZE);
CHECK_STRUCT_SIZE(txtdt_lu_t,             1, TXTDT_LU_SIZE);
CHECK_STRUCT_SIZE(txtdt_mgi_t,            1, TXTDT_MGI_SIZE);
CHECK_STRUCT_SIZE(vtsi_mat_t,             0, VTSI_MAT_SIZE);
CHECK_STRUCT_SIZE(ptt_info_t,             0, PTT_INFO_SIZE);
CHECK_STRUCT_SIZE(ttu_t,                  1, TTU_SIZE);
CHECK_STRUCT_SIZE(vts_ptt_srpt_t,         2, VTS_PTT_SRPT_SIZE);
CHECK_STRUCT_SIZE(vts_tmap_t,             1, VTS_TMAP_SIZE);
CHECK_STRUCT_SIZE(vts_tmapt_t,            2, VTS_TMAPT_SIZE);
CHECK_STRUCT_SIZE(samg_chapter_t,         0, SAMG_CHAPTER_SIZE);
CHECK_STRUCT_SIZE(samg_mat_t,             1, SAMG_MAT_SIZE);
CHECK_STRUCT_SIZE(amgi_mat_t,             0, AMGI_MAT_SIZE);
CHECK_STRUCT_SIZE(track_info_t,           0, TRACK_INFO_SIZE);
CHECK_STRUCT_SIZE(tracks_info_table_t,    1, TRACKS_INFO_TABLE_SIZE);
CHECK_STRUCT_SIZE(atsi_record_t,          0, ATSI_RECORD_SIZE);
CHECK_STRUCT_SIZE(atsi_mat_t,             0, ATSI_MAT_SIZE);
CHECK_STRUCT_SIZE(atsi_title_index_t,     0, ATSI_TITLE_INDEX_SIZE);
CHECK_STRUCT_SIZE(atsi_track_timestamp_t, 0, ATSI_TRACK_TIMESTAMP_SIZE);
CHECK_STRUCT_SIZE(atsi_track_pointer_t,   0, ATSI_TRACK_POINTER_SIZE);
CHECK_STRUCT_SIZE(atsi_title_record_t,    4, ATSI_TITLE_ROW_TABLE_SIZE);
CHECK_STRUCT_SIZE(atsi_title_table_t,     2, ATSI_TITLE_TABLE_SIZE);
CHECK_STRUCT_SIZE(downmix_coeff_t,        0, DOWNMIX_COEFF_SIZE);
CHECK_STRUCT_SIZE(rtav_vmgi_t,            0, RTAV_VMGI_SIZE);
CHECK_STRUCT_SIZE(pgci_t,                 1, PGCI_SIZE);
CHECK_STRUCT_SIZE(pgc_gi_t,               2, PGC_GI_SIZE);
CHECK_STRUCT_SIZE(ud_pgcit_t,             3, UD_PGCIT_SIZE);
CHECK_STRUCT_SIZE(m_c_gi_t,               1, M_C_GI_SIZE);
CHECK_STRUCT_SIZE(m_c_epi_t,              0, M_C_EPI_SIZE);

static void read_video_attr(video_attr_t *va) {
  getbits_state_t state;
  uint8_t buf[sizeof(video_attr_t)];

  memcpy(buf, va, sizeof(video_attr_t));
  if (!dvdread_getbits_init(&state, buf)) abort();
  va->mpeg_version = dvdread_getbits(&state, 2);
  va->video_format = dvdread_getbits(&state, 2);
  va->display_aspect_ratio = dvdread_getbits(&state, 2);
  va->permitted_df = dvdread_getbits(&state, 2);
  va->line21_cc_1 = dvdread_getbits(&state, 1);
  va->line21_cc_2 = dvdread_getbits(&state, 1);
  va->unknown1 = dvdread_getbits(&state, 1);
  va->bit_rate = dvdread_getbits(&state, 1);
  va->picture_size = dvdread_getbits(&state, 2);
  va->letterboxed = dvdread_getbits(&state, 1);
  va->film_mode = dvdread_getbits(&state, 1);
}

static void read_audio_attr(audio_attr_t *aa) {
  getbits_state_t state;
  uint8_t buf[sizeof(audio_attr_t)];

  memcpy(buf, aa, sizeof(audio_attr_t));
  if (!dvdread_getbits_init(&state, buf)) abort();
  aa->audio_format = dvdread_getbits(&state, 3);
  aa->multichannel_extension = dvdread_getbits(&state, 1);
  aa->lang_type = dvdread_getbits(&state, 2);
  aa->application_mode = dvdread_getbits(&state, 2);
  aa->quantization = dvdread_getbits(&state, 2);
  aa->sample_frequency = dvdread_getbits(&state, 2);
  aa->unknown1 = dvdread_getbits(&state, 1);
  aa->channels = dvdread_getbits(&state, 3);
  aa->lang_code = dvdread_getbits(&state, 16);
  aa->lang_extension = dvdread_getbits(&state, 8);
  aa->code_extension = dvdread_getbits(&state, 8);
  aa->unknown3 = dvdread_getbits(&state, 8);
  aa->app_info.karaoke.unknown4 = dvdread_getbits(&state, 1);
  aa->app_info.karaoke.channel_assignment = dvdread_getbits(&state, 3);
  aa->app_info.karaoke.version = dvdread_getbits(&state, 2);
  aa->app_info.karaoke.mc_intro = dvdread_getbits(&state, 1);
  aa->app_info.karaoke.mode = dvdread_getbits(&state, 1);
}

static void read_multichannel_ext(multichannel_ext_t *me) {
  getbits_state_t state;
  uint8_t buf[sizeof(multichannel_ext_t)];

  memcpy(buf, me, sizeof(multichannel_ext_t));
  if (!dvdread_getbits_init(&state, buf)) abort();
  me->zero1 = dvdread_getbits(&state, 7);
  me->ach0_gme = dvdread_getbits(&state, 1);
  me->zero2 = dvdread_getbits(&state, 7);
  me->ach1_gme = dvdread_getbits(&state, 1);
  me->zero3 = dvdread_getbits(&state, 4);
  me->ach2_gv1e = dvdread_getbits(&state, 1);
  me->ach2_gv2e = dvdread_getbits(&state, 1);
  me->ach2_gm1e = dvdread_getbits(&state, 1);
  me->ach2_gm2e = dvdread_getbits(&state, 1);
  me->zero4 = dvdread_getbits(&state, 4);
  me->ach3_gv1e = dvdread_getbits(&state, 1);
  me->ach3_gv2e = dvdread_getbits(&state, 1);
  me->ach3_gmAe = dvdread_getbits(&state, 1);
  me->ach3_se2e = dvdread_getbits(&state, 1);
  me->zero5 = dvdread_getbits(&state, 4);
  me->ach4_gv1e = dvdread_getbits(&state, 1);
  me->ach4_gv2e = dvdread_getbits(&state, 1);
  me->ach4_gmBe = dvdread_getbits(&state, 1);
  me->ach4_seBe = dvdread_getbits(&state, 1);
}

static void read_subp_attr(subp_attr_t *sa) {
  getbits_state_t state;
  uint8_t buf[sizeof(subp_attr_t)];

  memcpy(buf, sa, sizeof(subp_attr_t));
  if (!dvdread_getbits_init(&state, buf)) abort();
  sa->code_mode = dvdread_getbits(&state, 3);
  sa->zero1 = dvdread_getbits(&state, 3);
  sa->type = dvdread_getbits(&state, 2);
  sa->zero2 = dvdread_getbits(&state, 8);
  sa->lang_code = dvdread_getbits(&state, 16);
  sa->lang_extension = dvdread_getbits(&state, 8);
  sa->code_extension = dvdread_getbits(&state, 8);
}

static void read_user_ops(user_ops_t *uo) {
  getbits_state_t state;
  uint8_t buf[sizeof(user_ops_t)];

  memcpy(buf, uo, sizeof(user_ops_t));
  if (!dvdread_getbits_init(&state, buf)) abort();
  uo->zero                           = dvdread_getbits(&state, 7);
  uo->video_pres_mode_change         = dvdread_getbits(&state, 1);
  uo->karaoke_audio_pres_mode_change = dvdread_getbits(&state, 1);
  uo->angle_change                   = dvdread_getbits(&state, 1);
  uo->subpic_stream_change           = dvdread_getbits(&state, 1);
  uo->audio_stream_change            = dvdread_getbits(&state, 1);
  uo->pause_on                       = dvdread_getbits(&state, 1);
  uo->still_off                      = dvdread_getbits(&state, 1);
  uo->button_select_or_activate      = dvdread_getbits(&state, 1);
  uo->resume                         = dvdread_getbits(&state, 1);
  uo->chapter_menu_call              = dvdread_getbits(&state, 1);
  uo->angle_menu_call                = dvdread_getbits(&state, 1);
  uo->audio_menu_call                = dvdread_getbits(&state, 1);
  uo->subpic_menu_call               = dvdread_getbits(&state, 1);
  uo->root_menu_call                 = dvdread_getbits(&state, 1);
  uo->title_menu_call                = dvdread_getbits(&state, 1);
  uo->backward_scan                  = dvdread_getbits(&state, 1);
  uo->forward_scan                   = dvdread_getbits(&state, 1);
  uo->next_pg_search                 = dvdread_getbits(&state, 1);
  uo->prev_or_top_pg_search          = dvdread_getbits(&state, 1);
  uo->time_or_chapter_search         = dvdread_getbits(&state, 1);
  uo->go_up                          = dvdread_getbits(&state, 1);
  uo->stop                           = dvdread_getbits(&state, 1);
  uo->title_play                     = dvdread_getbits(&state, 1);
  uo->chapter_search_or_play         = dvdread_getbits(&state, 1);
  uo->title_or_time_play             = dvdread_getbits(&state, 1);
}

static void read_pgci_srp(pgci_srp_t *ps) {
  getbits_state_t state;
  uint8_t buf[sizeof(pgci_srp_t)];

  memcpy(buf, ps, sizeof(pgci_srp_t));
  if (!dvdread_getbits_init(&state, buf)) abort();
  ps->entry_id                       = dvdread_getbits(&state, 8);
  ps->block_mode                     = dvdread_getbits(&state, 2);
  ps->block_type                     = dvdread_getbits(&state, 2);
  ps->zero_1                         = dvdread_getbits(&state, 4);
  ps->ptl_id_mask                    = dvdread_getbits(&state, 16);
  ps->pgc_start_byte                 = dvdread_getbits(&state, 32);
}

static void read_cell_playback(cell_playback_t *cp) {
  getbits_state_t state;
  uint8_t buf[sizeof(cell_playback_t)];

  memcpy(buf, cp, sizeof(cell_playback_t));
  if (!dvdread_getbits_init(&state, buf)) abort();
  cp->block_mode                      = dvdread_getbits(&state, 2);
  cp->block_type                      = dvdread_getbits(&state, 2);
  cp->seamless_play                   = dvdread_getbits(&state, 1);
  cp->interleaved                     = dvdread_getbits(&state, 1);
  cp->stc_discontinuity               = dvdread_getbits(&state, 1);
  cp->seamless_angle                  = dvdread_getbits(&state, 1);
  cp->zero_1                          = dvdread_getbits(&state, 1);
  cp->playback_mode                   = dvdread_getbits(&state, 1);
  cp->restricted                      = dvdread_getbits(&state, 1);
  cp->cell_type                       = dvdread_getbits(&state, 5);
  cp->still_time                      = dvdread_getbits(&state, 8);
  cp->cell_cmd_nr                     = dvdread_getbits(&state, 8);

  cp->playback_time.hour              = dvdread_getbits(&state, 8);
  cp->playback_time.minute            = dvdread_getbits(&state, 8);
  cp->playback_time.second            = dvdread_getbits(&state, 8);
  cp->playback_time.frame_u           = dvdread_getbits(&state, 8);

  cp->first_sector                    = dvdread_getbits(&state, 32);
  cp->first_ilvu_end_sector           = dvdread_getbits(&state, 32);
  cp->last_vobu_start_sector          = dvdread_getbits(&state, 32);
  cp->last_sector                     = dvdread_getbits(&state, 32);
}

static void read_playback_type(playback_type_t *pt) {
  getbits_state_t state;
  uint8_t buf[sizeof(playback_type_t)];

  memcpy(buf, pt, sizeof(playback_type_t));
  if (!dvdread_getbits_init(&state, buf)) abort();
  pt->zero_1                          = dvdread_getbits(&state, 1);
  pt->multi_or_random_pgc_title       = dvdread_getbits(&state, 1);
  pt->jlc_exists_in_cell_cmd          = dvdread_getbits(&state, 1);
  pt->jlc_exists_in_prepost_cmd       = dvdread_getbits(&state, 1);
  pt->jlc_exists_in_button_cmd        = dvdread_getbits(&state, 1);
  pt->jlc_exists_in_tt_dom            = dvdread_getbits(&state, 1);
  pt->chapter_search_or_play          = dvdread_getbits(&state, 1);
  pt->title_or_time_play              = dvdread_getbits(&state, 1);
}

static void free_ptl_mait(ptl_mait_t* ptl_mait, int num_entries) {
  int i;
  for (i = 0; i < num_entries; i++)
    free(ptl_mait->countries[i].pf_ptl_mai);

  free(ptl_mait->countries);
  free(ptl_mait);
}

static ifo_handle_t *ifoOpenFileOrBackup(dvd_reader_t *ctx, int title,
                                         int backup) {
  struct ifo_handle_private_s *ifop;
  dvd_read_domain_t domain = backup ? DVD_READ_INFO_BACKUP_FILE
                                    : DVD_READ_INFO_FILE;
  char ifo_filename[MAX_UDF_FILE_NAME_LEN];

  ifop = calloc(1, sizeof(*ifop));
  if(!ifop)
    return NULL;

  ifop->ctx = ctx;
  ifop->file = DVDOpenFile(ctx, title, domain);
  if(!ifop->file)
  {
      free(ifop);
      return NULL;
  }

  if ( ctx->dvd_type == DVD_VR )
    snprintf(ifo_filename, 13, "VR_MANGR.IFO" );
  else if (title)
    snprintf(ifo_filename, 13, "%cTS_%02d_0.%s", STREAM_TYPE_STRING( ctx->dvd_type )
             ,title, backup ? "BUP" : "IFO");
  else
    snprintf(ifo_filename, 13, "%s_TS.%s", DVD_TYPE_STRING( ctx->dvd_type ), backup ? "BUP" : "IFO" );
  snprintf(ifop->name, sizeof(ifop->name), "%s", ifo_filename);

  ifo_handle_t *ifofile = &ifop->handle;
  /* First check if this is a VMGI file. */
  if(ifoRead_VMG(ifofile)) {

    ifofile->ifo_format=IFO_VIDEO;
    /* These are both mandatory. */
    if(!ifoRead_FP_PGC(ifofile) || !ifoRead_TT_SRPT(ifofile))
      goto ifoOpen_fail;

    ifoRead_PGCI_UT(ifofile);
    ifoRead_PTL_MAIT(ifofile);

    /* This is also mandatory. */
    if(!ifoRead_VTS_ATRT(ifofile))
      goto ifoOpen_fail;

    ifoRead_TXTDT_MGI(ifofile);
    ifoRead_C_ADT(ifofile);
    ifoRead_VOBU_ADMAP(ifofile);
    
    return ifofile;
  }

  if(ifoRead_VTS(ifofile)) {

    ifofile->ifo_format=IFO_VIDEO;
    if(!ifoRead_VTS_PTT_SRPT(ifofile) || !ifoRead_PGCIT(ifofile))
      goto ifoOpen_fail;

    ifoRead_PGCI_UT(ifofile);
    ifoRead_VTS_TMAPT(ifofile);
    ifoRead_C_ADT(ifofile);
    ifoRead_VOBU_ADMAP(ifofile);

    if(!ifoRead_TITLE_C_ADT(ifofile) || !ifoRead_TITLE_VOBU_ADMAP(ifofile))
      goto ifoOpen_fail;

    return ifofile;
  }
  
  if(ifoRead_AMG(ifofile)){
    ifofile->ifo_format=IFO_AUDIO;
    /* same function for both tables, will not be the same table in the case of non hybrid discs */
    if(!ifoRead_TIF(ifofile,1))
      goto ifoOpen_fail;
    if(!ifoRead_TIF(ifofile,2))
      goto ifoOpen_fail;

    /* Should read SAMG as it contains location to AOB pointers */
    DVDCloseFile(ifop->file);
    ifop->file = DVDOpenFile(ctx, 2, DVD_READ_SAMG_INFO);
    if(!ifop->file)
      goto ifoOpen_fail;
    if(!ifoRead_SAMG(ifofile))
      goto ifoOpen_fail;

    /* The Audio Still Video Set (ASVS) is optional, though usually exists */
    DVDCloseFile(ifop->file);
    ifop->file = DVDOpenFile(ctx, 2, backup ? DVD_READ_ASVS_INFO_BACKUP
                             : DVD_READ_ASVS_INFO);
    if(!ifop->file)
      Log1(ctx, "Disc does not have a still video set");
    else {
      if(!ifoRead_ASVS(ifofile))
        goto ifoOpen_fail;
    }

    return ifofile;
  }


  if(ifoRead_ATS(ifofile)){
    ifofile->ifo_format=IFO_AUDIO;

    if(!ifoRead_TT(ifofile))
      goto ifoOpen_fail;

    return ifofile;
  }

  if(ifoRead_RTAV_VMGI(ifofile)){
    ifofile->ifo_format=IFO_VIDEO_RECORDING;

    if(!ifoRead_UD_PGCIT(ifofile))
      goto ifoOpen_fail;

    if(!ifoRead_PGCI(ifofile))
      goto ifoOpen_fail;

    if(!ifoRead_PGC_GI(ifofile))
      goto ifoOpen_fail;

    if(!ifofile->pgc_gi)
      goto ifoOpen_fail;

    return ifofile;
  }

ifoOpen_fail:
  CTX_REPORT(ctx, DVD_REPORT_ERROR, "Invalid IFO for title %d (%s).", title, ifo_filename);
  ifoClose(ifofile);
  return NULL;
}

static void ifoSetBupFlag(dvd_reader_t *ctx, int title)
{
    if(title > 63)
        ctx->ifoBUPflags[0] |= UINT64_C(1) << (title - 64);
    else
        ctx->ifoBUPflags[1] |= UINT64_C(1) << title;
}

static int ifoGetBupFlag(const dvd_reader_t *ctx, int title)
{
    int bupflag;
    if(title > 63)
        bupflag = !! (ctx->ifoBUPflags[0] & (UINT64_C(1) << (title - 64)));
    else
        bupflag = !! (ctx->ifoBUPflags[1] & (UINT64_C(1) << title));
    return bupflag;
}

ifo_handle_t *ifoOpen(dvd_reader_t *ctx, int title) {
  ifo_handle_t *ifofile;
  int bupflag = ifoGetBupFlag(ctx, title);

  ifofile = ifoOpenFileOrBackup(ctx, title, bupflag);
  if(!ifofile) /* Try backup */
  {
      ifofile = ifoOpenFileOrBackup(ctx, title, 1);
      if(ifofile && !bupflag)
          ifoSetBupFlag(ctx, title);
  }
  return ifofile;
}

ifo_handle_t *ifoOpenVMGI(dvd_reader_t *ctx) {
  struct ifo_handle_private_s *ifop;

  const char *filename_prefix;
  int (*reader_func)(ifo_handle_t *);
  int result_format;

  /* function should handle all ifo types gracefully */
  switch(ctx->dvd_type) {
    case DVD_VR:
      filename_prefix = "VR_MANGR";
      reader_func = ifoRead_RTAV_VMGI;
      result_format = IFO_VIDEO_RECORDING;
      break;
    case DVD_V:
      filename_prefix = "VIDEO_TS";
      reader_func = ifoRead_VMG;
      result_format = IFO_VIDEO;
      break;
    default: /* Fallback to DVD_A (Audio) */
      filename_prefix = "AUDIO_TS";
      reader_func = ifoRead_AMG;
      result_format = IFO_AUDIO;
      break;
  }

  for(int backup = ifoGetBupFlag(ctx, 0); backup <= 1; backup++) 
  {
    ifop = calloc(1, sizeof(*ifop));
    if(!ifop) return NULL;

    const dvd_read_domain_t domain = backup ? DVD_READ_INFO_BACKUP_FILE 
                                            : DVD_READ_INFO_FILE;
    const char *ext = backup ? "BUP" : "IFO";

    ifop->ctx = ctx;
    snprintf(ifop->name, sizeof(ifop->name), "%s.%s", filename_prefix, ext);

    ifop->file = DVDOpenFile(ctx, 0, domain);

    if(!ifop->file) {
      CTX_REPORT(ctx, DVD_REPORT_WARN, "Can't open file %s.%s", filename_prefix, ext);
      free(ifop);
      continue;
    }

    if(reader_func(&ifop->handle)) {
      ifop->handle.ifo_format = result_format;
      return &ifop->handle;
    }

    CTX_REPORT(ctx, DVD_REPORT_ERROR, "ifoOpenVMGI(): Invalid main menu IFO (%s.%s).", filename_prefix, ext);
    ifoClose(&ifop->handle);
  }

  return NULL;
}

ifo_handle_t *ifoOpenVTSI(dvd_reader_t *ctx, int title) {
  struct ifo_handle_private_s *ifop;

  if(title <= 0 || title > 99) {
    CTX_REPORT(ctx, DVD_REPORT_ERROR, "ifoOpenVTSI invalid title (%d).", title);
    return NULL;
  }

  for(int backup = ifoGetBupFlag(ctx, title); backup <= 1; backup++)
  {
    ifop = calloc(1, sizeof(*ifop));
    if(!ifop)
      return NULL;

    const dvd_read_domain_t domain = backup ? DVD_READ_INFO_BACKUP_FILE
                                            : DVD_READ_INFO_FILE;
    const char *ext = backup ? "BUP" : "IFO";
    ifop->ctx = ctx;
    snprintf(ifop->name, sizeof(ifop->name), "%cTS_%02d_0.%s", STREAM_TYPE_STRING(ctx->dvd_type), title, ext);
    ifop->file = DVDOpenFile(ctx, title, domain);
    /* Should really catch any error */
    if(!ifop->file) {
      CTX_REPORT(ctx, DVD_REPORT_WARN, "Can't open file %cTS_%02d_0.%s.",
           STREAM_TYPE_STRING( ctx->dvd_type ), title, ext);
      free(ifop);
      continue;
    }

      if ((ctx->dvd_type == DVD_V
              ? (ifoRead_VTS(&ifop->handle) && ifop->handle.vtsi_mat)
              : (ifoRead_ATS(&ifop->handle) && ifop->handle.atsi_mat))) {
        ifop->handle.ifo_format = ctx->dvd_type == DVD_V ? IFO_VIDEO : IFO_AUDIO;
        return &ifop->handle;
      }

    CTX_REPORT(ctx, DVD_REPORT_ERROR, "Invalid IFO for title %d (%cTS_%02d_0.%s).",
            title, STREAM_TYPE_STRING( ctx->dvd_type ), title, ext);
    ifoClose(&ifop->handle);
  }

  return NULL;
}

ifo_handle_t *ifoOpenASVS(dvd_reader_t *ctx) {

  if ( ctx->dvd_type != DVD_A )
  {
    Log1(ctx, "the ASVS IFO is exclusive to DVD-Audio Discs");
    return NULL;
  }

  struct ifo_handle_private_s *ifop;

  for(int backup = ifoGetBupFlag(ctx, 0); backup <= 1; backup++)
  {
    ifop = calloc(1, sizeof(*ifop));
    if(!ifop)
      return NULL;

    const dvd_read_domain_t domain = backup ? DVD_READ_ASVS_INFO_BACKUP
                                            : DVD_READ_ASVS_INFO;
    const char *ext = backup ? "BUP" : "IFO";

    ifop->ctx = ctx;
    ifop->file = DVDOpenFile(ctx, 0, domain);
    if(!ifop->file) { /* Should really catch any error */
      Log1(ctx, "Can't open file AUDIO_SV.%s.", ext);
      free(ifop);
      return NULL;
    }


    if (ifoRead_ASVS(&ifop->handle)) {
      ifop->handle.ifo_format = IFO_AUDIO;
      return &ifop->handle;
    }

    Log1(ctx, "ifoOpenASVS(): Invalid ASVS IFO (AUDIO_SV.%s).", ext);
    ifoClose(&ifop->handle);
  }
  return NULL;


}


ifo_handle_t * ifoOpenSAMG(dvd_reader_t *ctx) {

  if ( ctx->dvd_type != DVD_A )
  {
    Log1(ctx, "the SAMG IFO is exclusive to DVD-Audio Discs");
    return NULL;
  }

  struct ifo_handle_private_s *ifop;

  for(int backup = ifoGetBupFlag(ctx, 0); backup <= 1; backup++)
  {
    ifop = calloc(1, sizeof(*ifop));
    if(!ifop)
      return NULL;

    ifop->ctx = ctx;
    ifop->file = DVDOpenFile(ctx, 0, DVD_READ_SAMG_INFO);
    if(!ifop->file) { /* Should really catch any error */
      Log1(ctx, "Can't open file AUDIO_PP.IFO.");
      free(ifop);
      return NULL;
    }


    if (ifoRead_SAMG(&ifop->handle)) {
      ifop->handle.ifo_format = IFO_AUDIO;
      return &ifop->handle;
    }

    Log1(ctx, "ifoOpenSAMG(): Invalid SAMG IFO (AUDIO_PP.IFO).");
    ifoClose(&ifop->handle);
  }
  return NULL;
}

void ifoFree_TT(ifo_handle_t *ifofile){
  if(!ifofile)
    return;

  if (ifofile->atsi_title_table) {
    for (int j=0;j<ifofile->atsi_title_table->nr_titles;j++){
      free((ifofile->atsi_title_table->atsi_title_row_tables+j)->atsi_track_pointer_rows);
      free((ifofile->atsi_title_table->atsi_title_row_tables+j)->atsi_track_timestamp_rows);
      free((ifofile->atsi_title_table->atsi_title_row_tables+j)->ats_pg_asv_pbi_srp);
      free((ifofile->atsi_title_table->atsi_title_row_tables+j)->dlist);
    }
    free(ifofile->atsi_title_table->atsi_title_row_tables);
    free(ifofile->atsi_title_table->atsi_index_rows);
    free(ifofile->atsi_title_table);
    ifofile->atsi_title_table= NULL;
  }
}

DVDREAD_API void ifoFree_PGC_GI(ifo_handle_t *ifofile) {
  if(!ifofile || !ifofile->pgc_gi)
    return;

  for(int k = 0; k < ifofile->pgc_gi->nr_of_programs; k++){
    free(ifofile->pgc_gi->pgi[k].map.time_infos);
    free(ifofile->pgc_gi->pgi[k].map.vobu_infos);
  }
  free(ifofile->pgc_gi->program_offsets);
  free(ifofile->pgc_gi->pgi);
  free(ifofile->pgc_gi);
  ifofile->pgc_gi= NULL;
}

DVDREAD_API void ifoFree_UD_PGCIT(ifo_handle_t *ifofile) {
  for ( int j = 0; j < ifofile->ud_pgcit->total_nr_of_programs ; j++ ) {
    if(ifofile->ud_pgcit->m_c_gi[j].c_epi_n > 0)
      free(ifofile->ud_pgcit->m_c_gi[j].m_c_epi);
  }

  free(ifofile->ud_pgcit->m_c_gi);
  free(ifofile->ud_pgcit->ci_offsets);
  free(ifofile->ud_pgcit->ud_pgci_items);
  free(ifofile->ud_pgcit);
  ifofile->ud_pgcit = NULL;
}

ifo_handle_t *ifoAddRef(ifo_handle_t *ifofile) {
  if(ifofile)
    PRIV(ifofile)->extra_refs++;
  return ifofile;
}

void ifoClose(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  /* Only the last reference frees the handle. */
  if(PRIV(ifofile)->extra_refs > 0) {
    PRIV(ifofile)->extra_refs--;
    return;
  }

  switch(ifofile->ifo_format) {
    case IFO_VIDEO:
      ifoFree_VOBU_ADMAP(ifofile);
      ifoFree_TITLE_VOBU_ADMAP(ifofile);
      ifoFree_C_ADT(ifofile);
      ifoFree_TITLE_C_ADT(ifofile);
      ifoFree_TXTDT_MGI(ifofile);
      ifoFree_VTS_ATRT(ifofile);
      ifoFree_PTL_MAIT(ifofile);
      ifoFree_PGCI_UT(ifofile);
      ifoFree_TT_SRPT(ifofile);
      ifoFree_FP_PGC(ifofile);
      ifoFree_PGCIT(ifofile);
      ifoFree_VTS_PTT_SRPT(ifofile);
      ifoFree_VTS_TMAPT(ifofile);

      if(ifofile->vmgi_mat)
        free(ifofile->vmgi_mat);

      if(ifofile->vtsi_mat)
        free(ifofile->vtsi_mat);
      break;
    case IFO_AUDIO:
      if(ifofile->amgi_mat)
        free(ifofile->amgi_mat);

      if(ifofile->atsi_mat)
        free(ifofile->atsi_mat);

      if(ifofile->samg_mat){
        free(ifofile->samg_mat->samg_chapters);
        free(ifofile->samg_mat);
      }

      if(ifofile->asvs_mat){
        free(ifofile->asvs_mat->asv_srpt);
        free(ifofile->asvs_mat);
      }

      if(ifofile->info_table_first_sector){
        free(ifofile->info_table_first_sector->tracks_info);
        free(ifofile->info_table_first_sector);
      }
      if(ifofile->info_table_second_sector){
        free(ifofile->info_table_second_sector->tracks_info);
        free(ifofile->info_table_second_sector);
      }
      if(ifofile->atsi_title_table)
        ifoFree_TT(ifofile);
      break;
    case IFO_VIDEO_RECORDING:
      if(ifofile->rtav_vmgi)
        free(ifofile->rtav_vmgi);
      if(ifofile->ud_pgcit)
        ifoFree_UD_PGCIT(ifofile);
      if(ifofile->pgc_gi)
        ifoFree_PGC_GI(ifofile);
      if(ifofile->pgci){
        free(ifofile->pgci->vob_formats);
        free(ifofile->pgci);
      }
      break;
    case IFO_UNKNOWN:
      break;
  }
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  DVDCloseFile(ifop->file);
  if(ifop->bup)
    DVDCloseFile(ifop->bup);
  free(ifop->rec_cache);
  free(ifop);
}


static int ifoRead_VMG(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  vmgi_mat_t *vmgi_mat;

  vmgi_mat = calloc(1, sizeof(vmgi_mat_t));
  if(!vmgi_mat)
    return 0;

  ifofile->vmgi_mat = vmgi_mat;
  ifofile->ifo_format= IFO_VIDEO;

  if(!ifo_seek(ifop, 0)) {
    free(ifofile->vmgi_mat);
    ifofile->vmgi_mat = NULL;
    return 0;
  }

  if(!ifo_read(ifop, vmgi_mat, sizeof(vmgi_mat_t))) {
    free(ifofile->vmgi_mat);
    ifofile->vmgi_mat = NULL;
    return 0;
  }

  if(strncmp("DVDVIDEO-VMG", vmgi_mat->vmg_identifier, 12) != 0) {
    free(ifofile->vmgi_mat);
    ifofile->vmgi_mat = NULL;
    return 0;
  }

  B2N_32(vmgi_mat->vmg_last_sector);
  B2N_32(vmgi_mat->vmgi_last_sector);
  B2N_32(vmgi_mat->vmg_category);
  B2N_16(vmgi_mat->vmg_nr_of_volumes);
  B2N_16(vmgi_mat->vmg_this_volume_nr);
  B2N_16(vmgi_mat->vmg_nr_of_title_sets);
  B2N_64(vmgi_mat->vmg_pos_code);
  B2N_32(vmgi_mat->vmgi_last_byte);
  B2N_32(vmgi_mat->first_play_pgc);
  B2N_32(vmgi_mat->vmgm_vobs);
  B2N_32(vmgi_mat->tt_srpt);
  B2N_32(vmgi_mat->vmgm_pgci_ut);
  B2N_32(vmgi_mat->ptl_mait);
  B2N_32(vmgi_mat->vts_atrt);
  B2N_32(vmgi_mat->txtdt_mgi);
  B2N_32(vmgi_mat->vmgm_c_adt);
  B2N_32(vmgi_mat->vmgm_vobu_admap);
  read_video_attr(&vmgi_mat->vmgm_video_attr);
  read_audio_attr(&vmgi_mat->vmgm_audio_attr);
  read_subp_attr(&vmgi_mat->vmgm_subp_attr);


  CHECK_ZERO(vmgi_mat->zero_1);
  CHECK_ZERO(vmgi_mat->zero_2);
  /* DVDs created by VDR-to-DVD device LG RC590M violate the following check with
   * vmgi_mat->zero_3 = 0x00000000010000000000000000000000000000. */
  CHECK_ZERO(vmgi_mat->zero_3);
  CHECK_ZERO(vmgi_mat->zero_4);
  CHECK_ZERO(vmgi_mat->zero_5);
  CHECK_ZERO(vmgi_mat->zero_6);
  CHECK_ZERO(vmgi_mat->zero_7);
  CHECK_ZERO(vmgi_mat->zero_8);
  CHECK_ZERO(vmgi_mat->zero_9);
  CHECK_ZERO(vmgi_mat->zero_10);
  CHECK_VALUE(vmgi_mat->vmg_last_sector != 0);
  CHECK_VALUE(vmgi_mat->vmgi_last_sector != 0);
  CHECK_VALUE(vmgi_mat->vmgi_last_sector * 2 <= vmgi_mat->vmg_last_sector);
  CHECK_VALUE(vmgi_mat->vmgi_last_sector * 2 <= vmgi_mat->vmg_last_sector);
  CHECK_VALUE(vmgi_mat->vmg_nr_of_volumes != 0);
  CHECK_VALUE(vmgi_mat->vmg_this_volume_nr != 0);
  CHECK_VALUE(vmgi_mat->vmg_this_volume_nr <= vmgi_mat->vmg_nr_of_volumes);
  CHECK_VALUE(vmgi_mat->disc_side == 1 || vmgi_mat->disc_side == 2);
  CHECK_VALUE(vmgi_mat->vmg_nr_of_title_sets != 0);
  CHECK_VALUE(vmgi_mat->vmgi_last_byte >= 341);
  CHECK_VALUE(vmgi_mat->vmgi_last_byte / DVD_BLOCK_LEN <=
              vmgi_mat->vmgi_last_sector);
  /* It seems that first_play_pgc is optional. */
  CHECK_VALUE(vmgi_mat->first_play_pgc < vmgi_mat->vmgi_last_byte);
  CHECK_VALUE(vmgi_mat->vmgm_vobs == 0 ||
              (vmgi_mat->vmgm_vobs > vmgi_mat->vmgi_last_sector &&
               vmgi_mat->vmgm_vobs < vmgi_mat->vmg_last_sector));
  CHECK_VALUE(vmgi_mat->tt_srpt <= vmgi_mat->vmgi_last_sector);
  CHECK_VALUE(vmgi_mat->vmgm_pgci_ut <= vmgi_mat->vmgi_last_sector);
  CHECK_VALUE(vmgi_mat->ptl_mait <= vmgi_mat->vmgi_last_sector);
  CHECK_VALUE(vmgi_mat->vts_atrt <= vmgi_mat->vmgi_last_sector);
  CHECK_VALUE(vmgi_mat->txtdt_mgi <= vmgi_mat->vmgi_last_sector);
  CHECK_VALUE(vmgi_mat->vmgm_c_adt <= vmgi_mat->vmgi_last_sector);
  CHECK_VALUE(vmgi_mat->vmgm_vobu_admap <= vmgi_mat->vmgi_last_sector);

  CHECK_VALUE(vmgi_mat->nr_of_vmgm_audio_streams <= 1);
  CHECK_VALUE(vmgi_mat->nr_of_vmgm_subp_streams <= 1);

  return 1;
}

static int ifoRead_SAMG(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  samg_mat_t *samg_mat;

  samg_mat = calloc(1, sizeof(samg_mat_t));
  if(!samg_mat)
    return 0;


  ifofile->samg_mat = samg_mat;

  if(!ifo_seek(ifop, 0)) {
    free(ifofile->samg_mat);
    ifofile->samg_mat = NULL;
    return 0;
  }

  if(!ifo_read(ifop, samg_mat, SAMG_MAT_SIZE)) {
    free(ifofile->samg_mat);
    ifofile->samg_mat = NULL;
    return 0;
  }

  if(strncmp("DVDAUDIOSAPP", samg_mat->samg_identifier, 12) != 0) {
    free(ifofile->samg_mat);
    ifofile->samg_mat = NULL;
    return 0;
  }
 
  B2N_16(samg_mat->nr_chapters);
  B2N_16(samg_mat->specification_version);

  samg_mat->samg_chapters = calloc(samg_mat->nr_chapters, sizeof(samg_chapter_t));
  if(!samg_mat->samg_chapters) {
    free(ifofile->samg_mat);
    ifofile->samg_mat = NULL;
    return 0;
  }

  if(!ifo_read(ifop, samg_mat->samg_chapters, 
                   samg_mat->nr_chapters * sizeof(samg_chapter_t))) {
    free(ifofile->samg_mat->samg_chapters);
    free(ifofile->samg_mat);
    ifofile->samg_mat = NULL;
    return 0;
  }

  for (int i = 0; i < samg_mat->nr_chapters; i++) {
    samg_chapter_t *index = &samg_mat->samg_chapters[i];
    CHECK_ZERO(index->zero_1);
    B2N_32(index->timestamp_pts);
    B2N_32(index->chapter_len);
    CHECK_ZERO(index->zero_2);
    B2N_32(index->start_sector_1);
    B2N_32(index->start_sector_2);
    B2N_32(index->end_sector);
    CHECK_ZERO(index->zero_3);
  }

  return 1;
}

static int ifoRead_ASVS(ifo_handle_t *ifofile){

  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  asvs_mat_t *asvs_mat;

  asvs_mat = calloc(1, sizeof(asvs_mat_t));

  if(!asvs_mat)
    return 0;

  ifofile->asvs_mat = asvs_mat;

  if(!ifo_seek(ifop, 0)) {
    free(ifofile->asvs_mat);
    ifofile->asvs_mat = NULL;
    return 0;
  }

  if(!ifo_read(ifop, asvs_mat, ASVS_MAT_SIZE)) {
    free(ifofile->asvs_mat);
    ifofile->asvs_mat = NULL;
    return 0;
  }

  if(strncmp("DVDAUDIOASVS", asvs_mat->asvs_identifier, 12) != 0) {
    free(ifofile->asvs_mat);
    ifofile->asvs_mat = NULL;
    return 0;
  }

  B2N_16(asvs_mat->asvs_nr_of_asvus);
  B2N_16(asvs_mat->specification_version);
  B2N_16(asvs_mat->p_vobs_ea);

  /* a broken disc could ask for more groups than fit */
  if(asvs_mat->asvs_nr_of_asvus > ASVU_GI_MAX_SIZE) {
    free(ifofile->asvs_mat);
    ifofile->asvs_mat = NULL;
    return 0;
  }

  int total_nr_frames = 0;
  for (int i = 0; i < asvs_mat->asvs_nr_of_asvus; i++ ) {
    B2N_16(asvs_mat->asvu_gi[i].start_p_vob_number);
    B2N_16(asvs_mat->asvu_gi[i].ref_start_sector);
    CHECK_ZERO(asvs_mat->asvu_gi[i].zero_1);
    total_nr_frames+=asvs_mat->asvu_gi[i].p_vob_ns;
  }


  asvs_mat->asv_srpt = calloc(total_nr_frames, ASV_SRPT_SIZE);
  if(!asvs_mat->asv_srpt) {
    free(ifofile->asvs_mat);
    ifofile->asvs_mat = NULL;
    return 0;
  }

  if(!ifo_read(ifop, asvs_mat->asv_srpt,
                   total_nr_frames * ASV_SRPT_SIZE )) {
    free(asvs_mat->asv_srpt);
    free(ifofile->asvs_mat);
    ifofile->asvs_mat = NULL;
    return 0;
  }

  for(int i = 0; i < total_nr_frames; i++)
    B2N_16(asvs_mat->asv_srpt[i]);


  CHECK_VALUE(asvs_mat->specification_version == 0x0012);
  CHECK_ZERO(asvs_mat->zero_1);
  CHECK_ZERO(asvs_mat->zero_2);
  return 1;

}

/* Read the still picture table (ASVS) of an audio title, if present. */
static void ifoRead_TT_stillpics(struct ifo_handle_private_s *ifop,
                                 atsi_title_record_t *index,
                                 uint32_t record_offset) {
  int nr_tracks = index->nr_tracks;
  ats_pg_asv_pbi_srp_t *ranges;
  asv_dlist_t *frames;
  int base, max_end = 0, nr_frames;

  if(nr_tracks <= 0)
    return;

  if(!ifo_seek(ifop, record_offset + index->ats_asv_pbit_sa
                   + DVD_BLOCK_LEN))
    return;

  ranges = calloc(nr_tracks, sizeof(ats_pg_asv_pbi_srp_t));
  if(!ranges)
    return;

  if(!ifo_read(ifop, ranges, nr_tracks * sizeof(ats_pg_asv_pbi_srp_t))) {
    free(ranges);
    return;
  }

  for(int j = 0; j < nr_tracks; j++) {
    B2N_16(ranges[j].start_value);
    B2N_16(ranges[j].end_value);
    if(ranges[j].end_value > max_end)
      max_end = ranges[j].end_value;
  }
  index->ats_pg_asv_pbi_srp = ranges;

  /* The still frame records follow the range table; end_values are byte
   * offsets into them, so the last referenced record bounds their count. */
  base = nr_tracks * ATS_PG_ASV_PBI_SRP_SIZE;
  if(max_end < base)
    return;
  nr_frames = (max_end - base) / ASV_DLIST_SIZE + 1;

  frames = calloc(nr_frames, sizeof(asv_dlist_t));
  if(!frames)
    return;

  if(!ifo_read(ifop, frames, nr_frames * sizeof(asv_dlist_t))) {
    free(frames);
    return;
  }

  for(int j = 0; j < nr_frames; j++) {
    B2N_32(frames[j].display_timing);
  }

  index->dlist = frames;
}

int ifoRead_TT(ifo_handle_t *ifofile) {

  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  atsi_title_table_t *atsi_title_table;

  atsi_title_table= calloc(1, sizeof(atsi_title_table_t));

  if(!atsi_title_table)
    return 0;

  ifofile->atsi_title_table = atsi_title_table;

  if(!ifo_seek(ifop, DVD_BLOCK_LEN)) {
    free(ifofile->atsi_title_table);
    ifofile->atsi_title_table = NULL;
    return 0;
  }

  if(!ifo_read(ifop, atsi_title_table, ATSI_TITLE_TABLE_SIZE)) {
    free(ifofile->atsi_title_table);
    ifofile->atsi_title_table = NULL;
    return 0;
  }

  B2N_16(atsi_title_table->nr_titles);
  B2N_32(atsi_title_table->last_byte_address);

  atsi_title_table->atsi_index_rows = calloc(atsi_title_table->nr_titles,sizeof(atsi_title_index_t));
  if(!atsi_title_table->atsi_index_rows) {
    free(ifofile->atsi_title_table);
    ifofile->atsi_title_table = NULL;
    return 0;
  }


  if(!ifo_read(ifop, atsi_title_table->atsi_index_rows, 
                   atsi_title_table->nr_titles * sizeof(atsi_title_index_t))) {
    free(ifofile->atsi_title_table->atsi_index_rows);
    free(ifofile->atsi_title_table);
    ifofile->atsi_title_table = NULL;
    return 0;
  }
  
  for(int i=0; i<atsi_title_table->nr_titles; i++)
    B2N_32(atsi_title_table->atsi_index_rows[i].offset_record_table);
  
  
  atsi_title_table->atsi_title_row_tables = calloc(atsi_title_table->nr_titles, sizeof(atsi_title_record_t));

  if(!atsi_title_table->atsi_title_row_tables) {
    free(ifofile->atsi_title_table->atsi_index_rows);
    free(ifofile->atsi_title_table);
    ifofile->atsi_title_table = NULL;
    return 0;
  }

  int i;
  for (i=0; i < atsi_title_table->nr_titles; i++){
    atsi_title_record_t *index = (atsi_title_table->atsi_title_row_tables + i);

    uint32_t record_offset = (atsi_title_table->atsi_index_rows+i)->offset_record_table;
    if(!ifo_seek(ifop, record_offset + DVD_BLOCK_LEN))
      goto fail_audio;

    if(!ifo_read(ifop, index, ATSI_TITLE_ROW_TABLE_SIZE))
      goto fail_audio;

    B2N_16(index->start_sector_pointers_table);
    B2N_16(index->ats_asv_pbit_sa);
    B2N_32(index->length_pts);
    int nr_tracks=index->nr_tracks;
    int nr_pointer_records = index->nr_pointer_records;

    index->atsi_track_timestamp_rows = calloc(nr_tracks, sizeof(atsi_track_timestamp_t));
    if(!index->atsi_track_timestamp_rows)
      goto fail_audio;

    index->atsi_track_pointer_rows = calloc(nr_pointer_records, sizeof(atsi_track_pointer_t));
    if(!index->atsi_track_pointer_rows)
      goto fail_audio;

    if(!ifo_read(ifop, index->atsi_track_timestamp_rows,
                     nr_tracks * sizeof(atsi_track_timestamp_t))) {
      free(index->atsi_track_timestamp_rows);
      goto fail_audio;
    }

    if(!ifo_seek(ifop, (atsi_title_table->atsi_index_rows + i)->offset_record_table
                     + index->start_sector_pointers_table + DVD_BLOCK_LEN)) {
      free(index->atsi_track_timestamp_rows);
      free(index->atsi_track_pointer_rows);
      goto fail_audio;
    }

    if(!ifo_read(ifop, index->atsi_track_pointer_rows,
                     nr_pointer_records * sizeof(atsi_track_pointer_t))) {
      free(index->atsi_track_timestamp_rows);
      free(index->atsi_track_pointer_rows);
      goto fail_audio;
    }
    
    for (int j = 0; j<nr_tracks; j++) {
      CHECK_ZERO(index->atsi_track_timestamp_rows[j].zero);
      B2N_32(index->atsi_track_timestamp_rows[j].first_pts_of_track);
      B2N_32(index->atsi_track_timestamp_rows[j].length_pts_of_track);
    }

    for (int j = 0; j<nr_pointer_records; j++) {
      B2N_32(index->atsi_track_pointer_rows[j].start_sector);
      B2N_32(index->atsi_track_pointer_rows[j].end_sector);
    }

    /* Sanity Check */
    CHECK_VALUE( index->start_sector_pointers_table
                 == ( ATSI_TITLE_ROW_TABLE_SIZE 
                 + index->nr_tracks * ATSI_TRACK_TIMESTAMP_SIZE ) );

    if(index->ats_asv_pbit_sa)
      ifoRead_TT_stillpics(ifop, index, record_offset);

  }
  return 1;

  fail_audio:
      for (int j = 0; j < i; j++){
        free((atsi_title_table->atsi_title_row_tables + j)->atsi_track_pointer_rows);
        free((atsi_title_table->atsi_title_row_tables + j)->atsi_track_timestamp_rows);
        free((atsi_title_table->atsi_title_row_tables + j)->ats_pg_asv_pbi_srp);
        free((atsi_title_table->atsi_title_row_tables + j)->dlist);
      }
      free(atsi_title_table->atsi_title_row_tables);
      free(ifofile->atsi_title_table->atsi_index_rows);
      free(ifofile->atsi_title_table);
      ifofile->atsi_title_table = NULL;
      return 0;
}

int ifoRead_TIF(ifo_handle_t *ifofile, int sector_offset) {
  /* check early if sector_offset corresponds to one of the tables */
  if (sector_offset != 2 && sector_offset != 1)
    return 0;

  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  tracks_info_table_t *tracks_info_table;

  tracks_info_table= calloc(1, sizeof(tracks_info_table_t));
  if(!tracks_info_table)
    return 0;

  if(!ifo_seek(ifop,DVD_BLOCK_LEN * sector_offset)) {
    free(tracks_info_table);
    return 0;
  }

  if(!ifo_read(ifop, tracks_info_table,TRACKS_INFO_TABLE_SIZE)) {
    free(tracks_info_table);
    return 0;
  }

  B2N_16(tracks_info_table->nr_of_titles);
  B2N_16(tracks_info_table->last_byte_in_table);

  tracks_info_table->tracks_info = calloc(tracks_info_table->nr_of_titles,sizeof(track_info_t));
  if(!tracks_info_table->tracks_info) {
    free(tracks_info_table);
    return 0;
  }

  if(!ifo_read(ifop, tracks_info_table->tracks_info,
                   tracks_info_table->nr_of_titles * sizeof(track_info_t))) {
    free(tracks_info_table->tracks_info);
    free(tracks_info_table);
    return 0;
  }

  /* the second table is an audio_ts only table, the first is audio_ts, video_ts, strangly the nr_titles in second table doesnt match up with the true nr_titles for this table. Need to subtract video titles*/
  for(int i=0; i<tracks_info_table->nr_of_titles; i++) {
    if(tracks_info_table->tracks_info[i].type_and_rank==0 && sector_offset == 2) {
      track_info_t *resized = realloc(tracks_info_table->tracks_info, i * sizeof(track_info_t));
      if(resized || i == 0)
        tracks_info_table->tracks_info = resized;
      tracks_info_table->nr_of_titles = i;
      break;
    }
    B2N_32(tracks_info_table->tracks_info[i].len_audio_zone_pts);
    CHECK_ZERO(tracks_info_table->tracks_info[i].zero_1);
    B2N_32(tracks_info_table->tracks_info[i].ts_pointer_relative_sector);
  }

  /* sanity check */
  /* sector table two's size and end byte are always the same as sector one's table
   * even though it will be equal to or smaller, since it only lists audio titles 
   * sector two's table has been resized in the ifo to match it's true size */
  if(sector_offset == 1)
    CHECK_VALUE( (TRACKS_INFO_TABLE_SIZE + 
                  tracks_info_table->nr_of_titles * TRACK_INFO_SIZE -
                  1) == tracks_info_table->last_byte_in_table );

  switch(sector_offset) {
    case 1:
      ifofile->info_table_first_sector = tracks_info_table;
      break;
    case 2:
      ifofile->info_table_second_sector = tracks_info_table;
      break;
  }

  return 1;
}


static int ifoRead_AMG(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  amgi_mat_t *amgi_mat;

  amgi_mat = calloc(1, sizeof(amgi_mat_t));
  if(!amgi_mat)
    return 0;

  ifofile->amgi_mat = amgi_mat;

  if(!ifo_seek(ifop, 0)) {
    free(ifofile->amgi_mat);
    ifofile->amgi_mat = NULL;
    return 0;
  }

  if(!ifo_read(ifop, amgi_mat, sizeof(amgi_mat_t))) {
    free(ifofile->amgi_mat);
    ifofile->amgi_mat = NULL;
    return 0;
  }

  if(strncmp("DVDAUDIO-AMG", amgi_mat->amg_identifier, 12) != 0) {
    free(ifofile->amgi_mat);
    ifofile->amgi_mat = NULL;
    return 0;
  }
  
  /* Should be some checks and conversions here, will see about this later*/
  B2N_32(amgi_mat->audio_sv_ifo_relative_p);
  B2N_32(amgi_mat->amg_end_byte_address);
  B2N_32(amgi_mat->amg_start_sector);
  B2N_32(amgi_mat->amgi_last_sector);
  B2N_16(amgi_mat->amg_nr_of_volumes);
  B2N_16(amgi_mat->specification_version);
  B2N_16(amgi_mat->amg_this_volume_nr);
  B2N_16(amgi_mat->amg_nr_of_zones);
  CHECK_ZERO(amgi_mat->zero_1);
  CHECK_ZERO(amgi_mat->zero_2);
  CHECK_ZERO(amgi_mat->zero_3);
  CHECK_ZERO(amgi_mat->zero_4);
  CHECK_ZERO(amgi_mat->zero_5);
  CHECK_ZERO(amgi_mat->zero_6);
  CHECK_ZERO(amgi_mat->zero_7);
  CHECK_ZERO(amgi_mat->zero_8);
  CHECK_ZERO(amgi_mat->zero_9);
  CHECK_ZERO(amgi_mat->zero_10);
  CHECK_VALUE(amgi_mat->specification_version == 0x0012);

  return 1;
}

static int ifoRead_RTAV_VMGI(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  rtav_vmgi_t *rtav_vmgi;

  rtav_vmgi = calloc(1, sizeof(rtav_vmgi_t));
  if(!rtav_vmgi)
    return 0;

  ifofile->rtav_vmgi = rtav_vmgi;

  if(!ifo_seek(ifop, 0)) {
    free(ifofile->rtav_vmgi);
    ifofile->rtav_vmgi= NULL;
    return 0;
  }

  if(!(ifo_read(ifop, rtav_vmgi, sizeof(rtav_vmgi_t)))) {
    free(ifofile->rtav_vmgi);
    ifofile->rtav_vmgi= NULL;
    return 0;
  }

  if(strncmp("DVD_RTR_VMG0", rtav_vmgi->id, 12) != 0) {
    free(ifofile->rtav_vmgi);
    ifofile->rtav_vmgi = NULL;
    return 0;
  }

  B2N_16(ifofile->rtav_vmgi->version);
  B2N_16(ifofile->rtav_vmgi->still_tm);
  B2N_32(ifofile->rtav_vmgi->vmg_ea);
  B2N_32(ifofile->rtav_vmgi->vmgi_ea);
  B2N_32(ifofile->rtav_vmgi->org_pgci_sa);
  B2N_32(ifofile->rtav_vmgi->ud_pgcit_sa);
  B2N_32(ifofile->rtav_vmgi->org_pgci_ea);
  B2N_32(ifofile->rtav_vmgi->info_308_sa);
  B2N_32(ifofile->rtav_vmgi->info_312_sa);
  B2N_32(ifofile->rtav_vmgi->info_316_sa);
  B2N_32(ifofile->rtav_vmgi->txtdt_mg_sa);
  B2N_32(ifofile->rtav_vmgi->mnfit_sa);


  CHECK_ZERO(ifofile->rtav_vmgi->zero_16);
  CHECK_ZERO(ifofile->rtav_vmgi->zero_34);
  CHECK_ZERO(ifofile->rtav_vmgi->zero_226);
  CHECK_ZERO(ifofile->rtav_vmgi->zero_280);
  CHECK_ZERO(ifofile->rtav_vmgi->zero_320);
  CHECK_ZERO(ifofile->rtav_vmgi->zero_360);
  return 1;

}

int ifoRead_PGCI(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  pgci_t *pgci;

  pgci = malloc(sizeof(pgci_t));

  if(!pgci)
    return 0;

  ifofile->pgci= pgci;

  if(!ifo_seek(ifop, ifofile->rtav_vmgi->org_pgci_sa)) {
    free(ifofile->pgci);
    ifofile->pgci = NULL;
    return 0;
  }

  if(!ifo_read(ifop, pgci, PGCI_SIZE)) {
    free(ifofile->pgci);
    ifofile->pgci = NULL;
    return 0;
  }

  pgci->vob_formats = calloc(ifofile->pgci->nr_of_vob_formats, VOB_FORMAT_SIZE);

  if(!pgci->vob_formats) {
    free(ifofile->pgci->vob_formats);
    free(ifofile->pgci);
    ifofile->pgci = NULL;
    return 0;
  }

  if(!ifo_read(ifop, pgci->vob_formats, pgci->nr_of_vob_formats * VOB_FORMAT_SIZE)) {
    free(ifofile->pgci->vob_formats);
    free(ifofile->pgci);
    ifofile->pgci = NULL;
    return 0;
  }

  for (int i = 0; i < pgci->nr_of_vob_formats ; i++)
    B2N_16(pgci->vob_formats[i].video_attr);

  B2N_32(pgci->len_org_pgci);
  CHECK_ZERO(pgci->zero1);
  return 1;

}

int ifoRead_PGC_GI(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  pgc_gi_t *pgc_gi;

  pgc_gi = malloc(sizeof(pgc_gi_t));

  if(!pgc_gi)
    return 0;

  ifofile->pgc_gi = pgc_gi;

  /* the pgc_gi directly follows pgci */
  uint32_t pgc_gi_sa = ifofile->rtav_vmgi->org_pgci_sa
                     + PGCI_SIZE
                     + ifofile->pgci->nr_of_vob_formats * VOB_FORMAT_SIZE;

  if(!ifo_seek(ifop, pgc_gi_sa )) {
    free(ifofile->pgc_gi);
    ifofile->pgc_gi = NULL;
    return 0;
  }

  if(!ifo_read(ifop, pgc_gi, PGC_GI_SIZE)) {
    free(ifofile->pgc_gi);
    ifofile->pgc_gi= NULL;
    return 0;
  }

  B2N_16(pgc_gi->nr_of_programs);

  /* there is a case where the original program info table is undefined or overwritten, regardless this should not be the default table used when we are navigating the disc */
  if ( pgc_gi->nr_of_programs == 0 ) {
    free(ifofile->pgc_gi);
    ifofile->pgc_gi= NULL;
    return 1;
  }

  pgc_gi->program_offsets = calloc(pgc_gi->nr_of_programs, sizeof(uint32_t));

  if(!pgc_gi->program_offsets) {
    free(ifofile->pgc_gi);
    ifofile->pgc_gi= NULL;
    return 0;
  }

  if(!ifo_read(ifop, pgc_gi->program_offsets, pgc_gi->nr_of_programs * sizeof(uint32_t))) {
    free(ifofile->pgc_gi->program_offsets);
    free(ifofile->pgc_gi);
    ifofile->pgc_gi= NULL;
    return 0;
  }

  pgc_gi->pgi= calloc(pgc_gi->nr_of_programs, sizeof(pgi_t));

  if(!pgc_gi->pgi) {
    free(ifofile->pgc_gi->program_offsets);
    free(ifofile->pgc_gi);
    ifofile->pgc_gi= NULL;
    return 0;
  }

  int i, j;
  for(i = 0; i < pgc_gi->nr_of_programs; i++) {
    B2N_32(pgc_gi->program_offsets[i]);

    if(!ifo_seek(ifop, ifofile->rtav_vmgi->org_pgci_sa
                     + pgc_gi->program_offsets[i]))
      goto fail3;

    if(!ifo_read(ifop, &pgc_gi->pgi[i].header, VVOB_SIZE)) {
      goto fail3;
   }

    B2N_16(pgc_gi->pgi[i].header.vob_attr);
    B2N_16(pgc_gi->pgi[i].header.vob_v_e_ptm.ptm_extra);
    B2N_16(pgc_gi->pgi[i].header.vob_v_s_ptm.ptm_extra);
    B2N_32(pgc_gi->pgi[i].header.vob_v_e_ptm.ptm);
    B2N_32(pgc_gi->pgi[i].header.vob_v_s_ptm.ptm);



    /* this entry is supposedly optional, and will not exist without this flag */
    if(pgc_gi->pgi[i].header.vob_attr & 0x80) {
      if(!ifo_read(ifop, &pgc_gi->pgi[i].adj_info, ADJ_VOB_SIZE))
        goto fail3;
    }

    /* since we're reading a struct that isn't packed , we have to
     * skip the padding this way */
    uint16_t pad2;
    if(!ifo_read(ifop, &pad2, sizeof(pad2)))
      goto fail3;

    /* header of this table */
    if(!ifo_read(ifop, &pgc_gi->pgi[i].map, VOBU_MAP_SIZE))
      goto fail3;

    B2N_16(pgc_gi->pgi[i].map.nr_of_time_info);
    B2N_16(pgc_gi->pgi[i].map.nr_of_vobu_info);
    B2N_16(pgc_gi->pgi[i].map.time_offset);
    B2N_32(pgc_gi->pgi[i].map.vob_offset);

    if(pgc_gi->pgi[i].map.nr_of_time_info > 0) {
      pgc_gi->pgi[i].map.time_infos = calloc(pgc_gi->pgi[i].map.nr_of_time_info,
                                                 TIME_INFO_SIZE);
      if(!pgc_gi->pgi[i].map.time_infos)
        goto fail3;

      if(!ifo_read(ifop, pgc_gi->pgi[i].map.time_infos,
                       pgc_gi->pgi[i].map.nr_of_time_info * TIME_INFO_SIZE)) {
        free(pgc_gi->pgi[i].map.time_infos);
        goto fail3;
      }

      for(int j = 0; j < pgc_gi->pgi[i].map.nr_of_time_info; j++) {
        B2N_16(pgc_gi->pgi[i].map.time_infos[j].vobu_entn);
        B2N_32(pgc_gi->pgi[i].map.time_infos[j].vobu_adr);
      }

    } else {
      pgc_gi->pgi[i].map.time_infos = NULL;
    }

    if(pgc_gi->pgi[i].map.nr_of_vobu_info > 0) {
      pgc_gi->pgi[i].map.vobu_infos = calloc(pgc_gi->pgi[i].map.nr_of_vobu_info,
                                                 VOBU_INFO_SIZE);
      if(!pgc_gi->pgi[i].map.vobu_infos) {
        free(pgc_gi->pgi[i].map.time_infos);
        goto fail3;
      }

      if(!ifo_read(ifop, pgc_gi->pgi[i].map.vobu_infos,
                     pgc_gi->pgi[i].map.nr_of_vobu_info * VOBU_INFO_SIZE)) {
        free(pgc_gi->pgi[i].map.time_infos);
        free(pgc_gi->pgi[i].map.vobu_infos);
        goto fail3;
      }
      for(j = 0; j<pgc_gi->pgi[i].map.nr_of_vobu_info; j++)
        B2N_16(pgc_gi->pgi[i].map.vobu_infos[j].vobu_size);

    } else {
      pgc_gi->pgi[i].map.vobu_infos = NULL;
    }
  }

  return 1;

fail3:
  for(int k = 0; k < i; k++){
    free(ifofile->pgc_gi->pgi[k].map.time_infos);
    free(ifofile->pgc_gi->pgi[k].map.vobu_infos);
  }
  free(ifofile->pgc_gi->program_offsets);
  free(ifofile->pgc_gi->pgi);
  free(ifofile->pgc_gi);
  ifofile->pgc_gi= NULL;
  return 0;
}

int ifoRead_UD_PGCIT(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  ud_pgcit_t *ud_pgcit;

  ud_pgcit = malloc(sizeof(ud_pgcit_t));

  if(!ud_pgcit)
    return 0;

  ifofile->ud_pgcit = ud_pgcit;

  if(!ifo_seek(ifop, ifofile->rtav_vmgi->ud_pgcit_sa)) {
    free(ifofile->ud_pgcit);
    ifofile->ud_pgcit = NULL;
    return 0;
  }

  if(!ifo_read(ifop, ud_pgcit, UD_PGCIT_SIZE)) {
    free(ifofile->ud_pgcit);
    ifofile->ud_pgcit = NULL;
    return 0;
  }

  B2N_16(ud_pgcit->total_nr_of_programs);

  ud_pgcit->ud_pgci_items = calloc(ud_pgcit->nr_of_pgci, UD_PGCI_SIZE);

  if(!ud_pgcit->ud_pgci_items) {
    free(ifofile->ud_pgcit);
    ifofile->ud_pgcit = NULL;
    return 0;
  }

  if(!ifo_read(ifop, ud_pgcit->ud_pgci_items, ud_pgcit->nr_of_pgci * UD_PGCI_SIZE)) {
    free(ud_pgcit->ud_pgci_items);
    free(ifofile->ud_pgcit);
    ifofile->ud_pgcit = NULL;
    return 0;
  }

  for(int i = 0; i < ud_pgcit->nr_of_pgci; i++) {
    B2N_16(ud_pgcit->ud_pgci_items[i].nr_of_programs);
    B2N_16(ud_pgcit->ud_pgci_items[i].prog_set_id);
    B2N_16(ud_pgcit->ud_pgci_items[i].first_prog_id);
  }

  ud_pgcit->ci_offsets = calloc(ud_pgcit->total_nr_of_programs, CI_OFFSET_SIZE);

  if(!ud_pgcit->ci_offsets) {
    free(ud_pgcit->ud_pgci_items);
    free(ifofile->ud_pgcit);
    ifofile->ud_pgcit = NULL;
    return 0;
  }

  if(!ifo_read(ifop, ud_pgcit->ci_offsets,
                   ud_pgcit->total_nr_of_programs * CI_OFFSET_SIZE)) {
    free(ud_pgcit->ci_offsets);
    free(ud_pgcit->ud_pgci_items);
    free(ifofile->ud_pgcit);
    ifofile->ud_pgcit = NULL;
    return 0;
  }

  ud_pgcit->m_c_gi = calloc(ud_pgcit->total_nr_of_programs, sizeof(m_c_gi_t) );
  if(!ud_pgcit->m_c_gi) {
    free(ud_pgcit->ci_offsets);
    free(ud_pgcit->ud_pgci_items);
    free(ifofile->ud_pgcit);
    ifofile->ud_pgcit = NULL;
    return 0;
  }

  int i;
  for ( i = 0; i < ud_pgcit->total_nr_of_programs; i++ ) {
    B2N_32(ud_pgcit->ci_offsets[i]);

    if(!ifo_seek(ifop,
                     ifofile->rtav_vmgi->ud_pgcit_sa + ud_pgcit->ci_offsets[i])) {
      for (int j = 0; j < i; j++) {
        if(ud_pgcit->m_c_gi[j].c_epi_n > 0)
          free(ud_pgcit->m_c_gi[j].m_c_epi);
      }
      free(ud_pgcit->m_c_gi);
      free(ifofile->ud_pgcit->ci_offsets);
      free(ud_pgcit->ud_pgci_items);
      free(ifofile->ud_pgcit);
      ifofile->ud_pgcit = NULL;
      return 0;
    }

    if(!ifo_read(ifop, &ud_pgcit->m_c_gi[i], M_C_GI_SIZE)) {
      for (int j = 0; j < i; j++) {
        if(ud_pgcit->m_c_gi[j].c_epi_n > 0)
          free(ud_pgcit->m_c_gi[j].m_c_epi);
      }
      free(ud_pgcit->m_c_gi);
      free(ud_pgcit->ci_offsets);
      free(ud_pgcit->ud_pgci_items);
      free(ifofile->ud_pgcit);
      ifofile->ud_pgcit = NULL;
      return 0;
    }

    B2N_16(ud_pgcit->m_c_gi[i].m_vobi_srpn);

    B2N_32(ud_pgcit->m_c_gi[i].c_v_s_ptm.ptm);
    B2N_16(ud_pgcit->m_c_gi[i].c_v_s_ptm.ptm_extra);

    B2N_32(ud_pgcit->m_c_gi[i].c_v_e_ptm.ptm);
    B2N_16(ud_pgcit->m_c_gi[i].c_v_e_ptm.ptm_extra);

    if(ud_pgcit->m_c_gi[i].c_epi_n > 0){
      ud_pgcit->m_c_gi[i].m_c_epi = calloc(ud_pgcit->m_c_gi[i].c_epi_n, M_C_EPI_SIZE );

      if(!ud_pgcit->m_c_gi[i].m_c_epi)
        goto fail4;

      if(!ifo_read(ifop, ud_pgcit->m_c_gi[i].m_c_epi,
                       ud_pgcit->m_c_gi[i].c_epi_n * M_C_EPI_SIZE)) {
        free(ud_pgcit->m_c_gi[i].m_c_epi);
        goto fail4;
      }

      for( int j = 0; j < ud_pgcit->m_c_gi[i].c_epi_n; j++){
        B2N_32(ud_pgcit->m_c_gi[i].m_c_epi[j].ep_ptm.ptm);
        B2N_16(ud_pgcit->m_c_gi[i].m_c_epi[j].ep_ptm.ptm_extra);
      }

    }

  }

  return 1;

fail4:

  for ( int j = 0; j < i; j++ ) {
    if(ud_pgcit->m_c_gi[j].c_epi_n > 0)
      free(ud_pgcit->m_c_gi[j].m_c_epi);
  }

  free(ud_pgcit->m_c_gi);
  free(ud_pgcit->ci_offsets);
  free(ud_pgcit->ud_pgci_items);
  free(ifofile->ud_pgcit);
  ifofile->ud_pgcit = NULL;
  return 0;

}

static int ifoRead_VTS(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  vtsi_mat_t *vtsi_mat;
  int i;

  vtsi_mat = calloc(1, sizeof(vtsi_mat_t));
  if(!vtsi_mat)
    return 0;

  ifofile->vtsi_mat = vtsi_mat;

  if(!ifo_seek(ifop, 0)) {
    free(ifofile->vtsi_mat);
    ifofile->vtsi_mat = NULL;
    return 0;
  }

  if(!(ifo_read(ifop, vtsi_mat, sizeof(vtsi_mat_t)))) {
    free(ifofile->vtsi_mat);
    ifofile->vtsi_mat = NULL;
    return 0;
  }

  if(strncmp("DVDVIDEO-VTS", vtsi_mat->vts_identifier, 12) != 0) {
    free(ifofile->vtsi_mat);
    ifofile->vtsi_mat = NULL;
    return 0;
  }

  read_video_attr(&vtsi_mat->vtsm_video_attr);
  read_video_attr(&vtsi_mat->vts_video_attr);
  read_audio_attr(&vtsi_mat->vtsm_audio_attr);
  for(i=0; i<8; i++)
    read_audio_attr(&vtsi_mat->vts_audio_attr[i]);
  read_subp_attr(&vtsi_mat->vtsm_subp_attr);
  for(i=0; i<32; i++)
    read_subp_attr(&vtsi_mat->vts_subp_attr[i]);
  B2N_32(vtsi_mat->vts_last_sector);
  B2N_32(vtsi_mat->vtsi_last_sector);
  B2N_32(vtsi_mat->vts_category);
  B2N_32(vtsi_mat->vtsi_last_byte);
  B2N_32(vtsi_mat->vtsm_vobs);
  B2N_32(vtsi_mat->vtstt_vobs);
  B2N_32(vtsi_mat->vts_ptt_srpt);
  B2N_32(vtsi_mat->vts_pgcit);
  B2N_32(vtsi_mat->vtsm_pgci_ut);
  B2N_32(vtsi_mat->vts_tmapt);
  B2N_32(vtsi_mat->vtsm_c_adt);
  B2N_32(vtsi_mat->vtsm_vobu_admap);
  B2N_32(vtsi_mat->vts_c_adt);
  B2N_32(vtsi_mat->vts_vobu_admap);


  CHECK_ZERO(vtsi_mat->zero_1);
  CHECK_ZERO(vtsi_mat->zero_2);
  CHECK_ZERO(vtsi_mat->zero_3);
  CHECK_ZERO(vtsi_mat->zero_4);
  CHECK_ZERO(vtsi_mat->zero_5);
  CHECK_ZERO(vtsi_mat->zero_6);
  CHECK_ZERO(vtsi_mat->zero_7);
  CHECK_ZERO(vtsi_mat->zero_8);
  CHECK_ZERO(vtsi_mat->zero_9);
  CHECK_ZERO(vtsi_mat->zero_10);
  CHECK_ZERO(vtsi_mat->zero_11);
  CHECK_ZERO(vtsi_mat->zero_12);
  CHECK_ZERO(vtsi_mat->zero_13);
  CHECK_ZERO(vtsi_mat->zero_14);
  CHECK_ZERO(vtsi_mat->zero_15);
  CHECK_ZERO(vtsi_mat->zero_16);
  CHECK_ZERO(vtsi_mat->zero_17);
  CHECK_ZERO(vtsi_mat->zero_18);
  CHECK_ZERO(vtsi_mat->zero_19);
  CHECK_ZERO(vtsi_mat->zero_20);
  CHECK_ZERO(vtsi_mat->zero_21);
  CHECK_VALUE(vtsi_mat->vtsi_last_sector*2 <= vtsi_mat->vts_last_sector);
  CHECK_VALUE(vtsi_mat->vtsi_last_byte/DVD_BLOCK_LEN <= vtsi_mat->vtsi_last_sector);
  CHECK_VALUE(vtsi_mat->vtsm_vobs == 0 ||
              (vtsi_mat->vtsm_vobs > vtsi_mat->vtsi_last_sector &&
               vtsi_mat->vtsm_vobs < vtsi_mat->vts_last_sector));
  CHECK_VALUE(vtsi_mat->vtstt_vobs == 0 ||
              (vtsi_mat->vtstt_vobs > vtsi_mat->vtsi_last_sector &&
               vtsi_mat->vtstt_vobs < vtsi_mat->vts_last_sector));
  CHECK_VALUE(vtsi_mat->vts_ptt_srpt <= vtsi_mat->vtsi_last_sector);
  CHECK_VALUE(vtsi_mat->vts_pgcit <= vtsi_mat->vtsi_last_sector);
  CHECK_VALUE(vtsi_mat->vtsm_pgci_ut <= vtsi_mat->vtsi_last_sector);
  CHECK_VALUE(vtsi_mat->vts_tmapt <= vtsi_mat->vtsi_last_sector);
  CHECK_VALUE(vtsi_mat->vtsm_c_adt <= vtsi_mat->vtsi_last_sector);
  CHECK_VALUE(vtsi_mat->vtsm_vobu_admap <= vtsi_mat->vtsi_last_sector);
  CHECK_VALUE(vtsi_mat->vts_c_adt <= vtsi_mat->vtsi_last_sector);
  CHECK_VALUE(vtsi_mat->vts_vobu_admap <= vtsi_mat->vtsi_last_sector);

  CHECK_VALUE(vtsi_mat->nr_of_vtsm_audio_streams <= 1);
  CHECK_VALUE(vtsi_mat->nr_of_vtsm_subp_streams <= 1);

  CHECK_VALUE(vtsi_mat->nr_of_vts_audio_streams <= 8);
  for(i = vtsi_mat->nr_of_vts_audio_streams; i < 8; i++)
    CHECK_ZERO(vtsi_mat->vts_audio_attr[i]);

  CHECK_VALUE(vtsi_mat->nr_of_vts_subp_streams <= 32);
  for(i = vtsi_mat->nr_of_vts_subp_streams; i < 32; i++)
    CHECK_ZERO(vtsi_mat->vts_subp_attr[i]);

  for(i = 0; i < 8; i++) {
    read_multichannel_ext(&vtsi_mat->vts_mu_audio_attr[i]);
    CHECK_ZERO0(vtsi_mat->vts_mu_audio_attr[i].zero1);
    CHECK_ZERO0(vtsi_mat->vts_mu_audio_attr[i].zero2);
    CHECK_ZERO0(vtsi_mat->vts_mu_audio_attr[i].zero3);
    CHECK_ZERO0(vtsi_mat->vts_mu_audio_attr[i].zero4);
    CHECK_ZERO0(vtsi_mat->vts_mu_audio_attr[i].zero5);
    CHECK_ZERO(vtsi_mat->vts_mu_audio_attr[i].zero6);
  }

  return 1;
}

static int ifoRead_ATS(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  atsi_mat_t *atsi_mat;

  atsi_mat = calloc(1, sizeof(atsi_mat_t));
  if(!atsi_mat)
    return 0;

  ifofile->atsi_mat = atsi_mat;

  if(!ifo_seek(ifop, 0)) {
    free(ifofile->atsi_mat);
    ifofile->atsi_mat = NULL;
    return 0;
  }

  if(!(ifo_read(ifop, atsi_mat, sizeof(atsi_mat_t)))) {
    free(ifofile->atsi_mat);
    ifofile->atsi_mat = NULL;
    return 0;
  }

  if(strncmp("DVDAUDIO-ATS", atsi_mat->ats_identifier, 12) != 0) {
    free(ifofile->atsi_mat);
    ifofile->atsi_mat = NULL;
    return 0;
  }

  for (int i=0; i < DOWNMIX_COEFF_MAX_SIZE; i++) {
    CHECK_ZERO(atsi_mat->downmix_coefficients[i].zero_1);
    CHECK_ZERO(atsi_mat->downmix_coefficients[i].zero_2);
  }
  

  B2N_32(atsi_mat->ats_last_sector);
  B2N_32(atsi_mat->atsi_last_sector);
  B2N_32(atsi_mat->atst_aobs);
  B2N_32(atsi_mat->atsi_last_byte);
  B2N_32(atsi_mat->ats_pgci_ut);
  B2N_32(atsi_mat->vts_tmapt);
  B2N_32(atsi_mat->vtsm_c_adt);
  B2N_32(atsi_mat->vtsm_vobu_admap);
  B2N_32(atsi_mat->vts_c_adt);
  B2N_32(atsi_mat->vts_vobu_admap);


  CHECK_ZERO(atsi_mat->zero_1);
  CHECK_ZERO(atsi_mat->zero_2);
  CHECK_ZERO(atsi_mat->zero_3);
  CHECK_ZERO(atsi_mat->zero_4);

  return 1;
}

static int ifoRead_PGC_COMMAND_TBL(ifo_handle_t *ifofile,
                                   pgc_command_tbl_t *cmd_tbl,
                                   unsigned int offset) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  if(!ifo_seek(ifop, offset))
    return 0;

  if(!(ifo_read(ifop, cmd_tbl, PGC_COMMAND_TBL_SIZE)))
    return 0;

  B2N_16(cmd_tbl->nr_of_pre);
  B2N_16(cmd_tbl->nr_of_post);
  B2N_16(cmd_tbl->nr_of_cell);
  B2N_16(cmd_tbl->last_byte);

  uint32_t nr_cmds = (uint32_t)cmd_tbl->nr_of_pre
                   + (uint32_t)cmd_tbl->nr_of_post
                   + (uint32_t)cmd_tbl->nr_of_cell;

  CHECK_VALUE(nr_cmds <= 255);
  CHECK_VALUE(nr_cmds * COMMAND_DATA_SIZE + PGC_COMMAND_TBL_SIZE
              <= (uint32_t)cmd_tbl->last_byte + 1U);

  if(cmd_tbl->nr_of_pre != 0) {
    unsigned int pre_cmds_size  = cmd_tbl->nr_of_pre * COMMAND_DATA_SIZE;
    cmd_tbl->pre_cmds = malloc(pre_cmds_size);
    if(!cmd_tbl->pre_cmds)
      return 0;

    if(!(ifo_read(ifop, cmd_tbl->pre_cmds, pre_cmds_size))) {
      free(cmd_tbl->pre_cmds);
      return 0;
    }
  }

  if(cmd_tbl->nr_of_post != 0) {
    unsigned int post_cmds_size = cmd_tbl->nr_of_post * COMMAND_DATA_SIZE;
    cmd_tbl->post_cmds = malloc(post_cmds_size);
    if(!cmd_tbl->post_cmds) {
      if(cmd_tbl->pre_cmds)
        free(cmd_tbl->pre_cmds);
      return 0;
    }
    if(!(ifo_read(ifop, cmd_tbl->post_cmds, post_cmds_size))) {
      if(cmd_tbl->pre_cmds)
        free(cmd_tbl->pre_cmds);
      free(cmd_tbl->post_cmds);
      return 0;
    }
  }

  if(cmd_tbl->nr_of_cell != 0) {
    unsigned int cell_cmds_size = cmd_tbl->nr_of_cell * COMMAND_DATA_SIZE;
    cmd_tbl->cell_cmds = malloc(cell_cmds_size);
    if(!cmd_tbl->cell_cmds) {
      if(cmd_tbl->pre_cmds)
        free(cmd_tbl->pre_cmds);
      if(cmd_tbl->post_cmds)
        free(cmd_tbl->post_cmds);
      return 0;
    }
    if(!(ifo_read(ifop, cmd_tbl->cell_cmds, cell_cmds_size))) {
      if(cmd_tbl->pre_cmds)
        free(cmd_tbl->pre_cmds);
      if(cmd_tbl->post_cmds)
        free(cmd_tbl->post_cmds);
      free(cmd_tbl->cell_cmds);
      return 0;
    }
  }

  /*
   * Make a run over all the commands and see that we can interpret them all?
   */
  return 1;
}


static void ifoFree_PGC_COMMAND_TBL(pgc_command_tbl_t *cmd_tbl) {
  if(cmd_tbl) {
    if(cmd_tbl->nr_of_pre && cmd_tbl->pre_cmds)
      free(cmd_tbl->pre_cmds);
    if(cmd_tbl->nr_of_post && cmd_tbl->post_cmds)
      free(cmd_tbl->post_cmds);
    if(cmd_tbl->nr_of_cell && cmd_tbl->cell_cmds)
      free(cmd_tbl->cell_cmds);
    free(cmd_tbl);
  }
}

static int ifoRead_PGC_PROGRAM_MAP(ifo_handle_t *ifofile,
                                   pgc_program_map_t *program_map,
                                   unsigned int nr, unsigned int offset) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  unsigned int size = nr * sizeof(pgc_program_map_t);

  if(!ifo_seek(ifop, offset))
    return 0;

  if(!(ifo_read(ifop, program_map, size)))
    return 0;

  return 1;
}

static int ifoRead_CELL_PLAYBACK_TBL(ifo_handle_t *ifofile,
                                     cell_playback_t *cell_playback,
                                     unsigned int nr, unsigned int offset) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  unsigned int i;
  unsigned int size = nr * sizeof(cell_playback_t);

  if(!ifo_seek(ifop, offset))
    return 0;

  if(!(ifo_read(ifop, cell_playback, size)))
    return 0;

  for(i = 0; i < nr; i++) {
    read_cell_playback(&cell_playback[i]);
    /* Changed < to <= because this was false in the movie 'Pi'. */
    CHECK_VALUE(cell_playback[i].last_vobu_start_sector <=
                cell_playback[i].last_sector);
    CHECK_VALUE(cell_playback[i].first_sector <=
                cell_playback[i].last_vobu_start_sector);
  }

  return 1;
}


static int ifoRead_CELL_POSITION_TBL(ifo_handle_t *ifofile,
                                     cell_position_t *cell_position,
                                     unsigned int nr, unsigned int offset) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  unsigned int i;
  unsigned int size = nr * sizeof(cell_position_t);

  if(!ifo_seek(ifop, offset))
    return 0;

  if(!(ifo_read(ifop, cell_position, size)))
    return 0;

  for(i = 0; i < nr; i++) {
    B2N_16(cell_position[i].vob_id_nr);
    CHECK_ZERO(cell_position[i].zero_1);
  }

  return 1;
}

static int ifoRead_PGC(ifo_handle_t *ifofile, pgc_t *pgc, unsigned int offset) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  unsigned int i;

  if(!ifo_seek(ifop, offset))
    return 0;

  if(!(ifo_read(ifop, pgc, PGC_SIZE)))
    return 0;

  read_user_ops(&pgc->prohibited_ops);
  B2N_16(pgc->next_pgc_nr);
  B2N_16(pgc->prev_pgc_nr);
  B2N_16(pgc->goup_pgc_nr);
  B2N_16(pgc->command_tbl_offset);
  B2N_16(pgc->program_map_offset);
  B2N_16(pgc->cell_playback_offset);
  B2N_16(pgc->cell_position_offset);

  for(i = 0; i < 8; i++)
    B2N_16(pgc->audio_control[i]);
  for(i = 0; i < 32; i++)
    B2N_32(pgc->subp_control[i]);
  for(i = 0; i < 16; i++)
    B2N_32(pgc->palette[i]);

  CHECK_ZERO(pgc->zero_1);
  CHECK_VALUE(pgc->nr_of_programs <= pgc->nr_of_cells);

  /* verify time (look at print_time) */
  for(i = 0; i < 8; i++)
    if(!(pgc->audio_control[i] & 0x8000)) /* The 'is present' bit */
      CHECK_ZERO(pgc->audio_control[i]);
  for(i = 0; i < 32; i++)
    if(!(pgc->subp_control[i] & 0x80000000)) /* The 'is present' bit */
      CHECK_ZERO(pgc->subp_control[i]);

  /* Check that time is 0:0:0:0 also if nr_of_programs == 0 */
  if(pgc->nr_of_programs == 0) {
    CHECK_ZERO(pgc->still_time);
    CHECK_ZERO(pgc->pg_playback_mode); /* ?? */
    CHECK_VALUE(pgc->program_map_offset == 0);
    CHECK_VALUE(pgc->cell_playback_offset == 0);
    CHECK_VALUE(pgc->cell_position_offset == 0);
  } else {
    CHECK_VALUE(pgc->program_map_offset != 0);
    CHECK_VALUE(pgc->cell_playback_offset != 0);
    CHECK_VALUE(pgc->cell_position_offset != 0);
  }

  if(pgc->command_tbl_offset != 0) {
    pgc->command_tbl = calloc(1, sizeof(pgc_command_tbl_t));
    if(!pgc->command_tbl)
      return 0;

    if(!ifoRead_PGC_COMMAND_TBL(ifofile, pgc->command_tbl,
                                offset + pgc->command_tbl_offset)) {
      return 0;
    }
  } else {
    pgc->command_tbl = NULL;
  }

  if(pgc->program_map_offset != 0 && pgc->nr_of_programs>0) {
    pgc->program_map = calloc(pgc->nr_of_programs, sizeof(pgc_program_map_t));
    if(!pgc->program_map) {
      return 0;
    }
    if(!ifoRead_PGC_PROGRAM_MAP(ifofile, pgc->program_map,pgc->nr_of_programs,
                                offset + pgc->program_map_offset)) {
      return 0;
    }
  } else {
    pgc->program_map = NULL;
  }

  if(pgc->cell_playback_offset != 0 && pgc->nr_of_cells>0) {
    pgc->cell_playback = calloc(pgc->nr_of_cells, sizeof(cell_playback_t));
    if(!pgc->cell_playback) {
      return 0;
    }
    if(!ifoRead_CELL_PLAYBACK_TBL(ifofile, pgc->cell_playback,
                                  pgc->nr_of_cells,
                                  offset + pgc->cell_playback_offset)) {
      return 0;
    }
  } else {
    pgc->cell_playback = NULL;
  }

  if(pgc->cell_position_offset != 0 && pgc->nr_of_cells>0) {
    pgc->cell_position = calloc(pgc->nr_of_cells, sizeof(cell_position_t));
    if(!pgc->cell_position) {
      return 0;
    }
    if(!ifoRead_CELL_POSITION_TBL(ifofile, pgc->cell_position,
                                  pgc->nr_of_cells,
                                  offset + pgc->cell_position_offset)) {
      return 0;
    }
  } else {
    pgc->cell_position = NULL;
  }

  return 1;
}

int ifoRead_FP_PGC(ifo_handle_t *ifofile) {

  if(!ifofile)
    return 0;

  if(!ifofile->vmgi_mat)
    return 0;

  /* It seems that first_play_pgc is optional after all. */
  ifofile->first_play_pgc = NULL;
  if(!ifofile->vmgi_mat->first_play_pgc)
    return 1;

  ifofile->first_play_pgc = calloc(1, sizeof(pgc_t));
  if(!ifofile->first_play_pgc)
    return 0;

  ifofile->first_play_pgc->ref_count = 1;
  if(!ifoRead_PGC(ifofile, ifofile->first_play_pgc,
                  ifofile->vmgi_mat->first_play_pgc)) {
    ifoFree_PGC(ifofile->first_play_pgc);
    ifofile->first_play_pgc = NULL;
    return 0;
  }

  return 1;
}

static void ifoFree_PGC(pgc_t *pgc) {
  if(pgc && (--pgc->ref_count) <= 0) {
    ifoFree_PGC_COMMAND_TBL(pgc->command_tbl);
    if(pgc->program_map)
      free(pgc->program_map);
    if(pgc->cell_playback)
      free(pgc->cell_playback);
    if(pgc->cell_position)
      free(pgc->cell_position);
    free(pgc);
  }
}

void ifoFree_FP_PGC(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  if(ifofile->first_play_pgc) {
    ifoFree_PGC(ifofile->first_play_pgc);
    ifofile->first_play_pgc = NULL;
  }
}


int ifoRead_TT_SRPT(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  tt_srpt_t *tt_srpt;
  unsigned int i;
  size_t info_length;

  if(!ifofile)
    return 0;

  if(!ifofile->vmgi_mat)
    return 0;

  if(ifofile->vmgi_mat->tt_srpt == 0) /* mandatory */
    return 0;

  if(!ifo_seek(ifop, ifofile->vmgi_mat->tt_srpt * DVD_BLOCK_LEN))
    return 0;

  tt_srpt = calloc(1, sizeof(tt_srpt_t));
  if(!tt_srpt)
    return 0;

  ifofile->tt_srpt = tt_srpt;

  if(!(ifo_read(ifop, tt_srpt, TT_SRPT_SIZE))) {
    IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read TT_SRPT.");
    free(tt_srpt);
    return 0;
  }

  B2N_16(tt_srpt->nr_of_srpts);
  B2N_32(tt_srpt->last_byte);

  /* E-One releases don't fill this field */
  if(tt_srpt->last_byte == 0) {
    tt_srpt->last_byte = tt_srpt->nr_of_srpts * sizeof(title_info_t) - 1 + TT_SRPT_SIZE;
  }
  info_length = tt_srpt->last_byte + 1 - TT_SRPT_SIZE;

  tt_srpt->title = calloc(1, info_length);
  if(!tt_srpt->title) {
    free(tt_srpt);
    ifofile->tt_srpt = NULL;
    return 0;
  }
  if(!(ifo_read(ifop, tt_srpt->title, info_length))) {
    IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read TT_SRPT.");
    ifoFree_TT_SRPT(ifofile);
    return 0;
  }

  if(tt_srpt->nr_of_srpts>info_length/sizeof(title_info_t)){
    IFO_REPORT(ifop, DVD_REPORT_WARN, "data mismatch: info_length (%zd)!= nr_of_srpts (%d). Truncating.",
            info_length/sizeof(title_info_t),tt_srpt->nr_of_srpts);
    tt_srpt->nr_of_srpts=info_length/sizeof(title_info_t);
  }

  for(i =  0; i < tt_srpt->nr_of_srpts; i++) {
    B2N_16(tt_srpt->title[i].nr_of_ptts);
    B2N_16(tt_srpt->title[i].parental_id);
    B2N_32(tt_srpt->title[i].title_set_sector);
  }


  CHECK_ZERO(tt_srpt->zero_1);
  CHECK_VALUE(tt_srpt->nr_of_srpts != 0);
  CHECK_VALUE(tt_srpt->nr_of_srpts < 100); /* ?? */
  CHECK_VALUE(tt_srpt->nr_of_srpts * sizeof(title_info_t) <= info_length);

  for(i = 0; i < tt_srpt->nr_of_srpts; i++) {
    read_playback_type(&tt_srpt->title[i].pb_ty);
    CHECK_VALUE(tt_srpt->title[i].pb_ty.zero_1 == 0);
    CHECK_VALUE(tt_srpt->title[i].nr_of_angles != 0);
    CHECK_VALUE(tt_srpt->title[i].nr_of_angles < 10);
    /* CHECK_VALUE(tt_srpt->title[i].nr_of_ptts != 0); */
    /* XXX: this assertion breaks Ghostbusters: */
    CHECK_VALUE(tt_srpt->title[i].nr_of_ptts < 1000); /* ?? */
    CHECK_VALUE(tt_srpt->title[i].title_set_nr != 0);
    CHECK_VALUE(tt_srpt->title[i].title_set_nr < 100); /* ?? */
    CHECK_VALUE(tt_srpt->title[i].vts_ttn != 0);
    CHECK_VALUE(tt_srpt->title[i].vts_ttn < 100); /* ?? */
    /* CHECK_VALUE(tt_srpt->title[i].title_set_sector != 0); */
  }

  /* Make this a function */
#if 0
  if(memcmp((uint8_t *)tt_srpt->title +
            tt_srpt->nr_of_srpts * sizeof(title_info_t),
            my_friendly_zeros,
            info_length - tt_srpt->nr_of_srpts * sizeof(title_info_t))) {
    IFO_REPORT(ifop, DVD_REPORT_INFO, "VMG_PTT_SRPT: the slack after the table is not zero: kept");
    hexdump((uint8_t *)tt_srpt->title +
            tt_srpt->nr_of_srpts * sizeof(title_info_t),
            info_length - tt_srpt->nr_of_srpts * sizeof(title_info_t));
  }
#endif

  return 1;
}


void ifoFree_TT_SRPT(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  if(ifofile->tt_srpt) {
    free(ifofile->tt_srpt->title);
    ifofile->tt_srpt->title = NULL;
    free(ifofile->tt_srpt);
    ifofile->tt_srpt = NULL;
  }
}


int ifoRead_VTS_PTT_SRPT(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  vts_ptt_srpt_t *vts_ptt_srpt = NULL;
  int info_length, i, j;
  uint32_t *data = NULL;

  if(!ifofile)
    return 0;

  if(!ifofile->vtsi_mat)
    return 0;

  if(ifofile->vtsi_mat->vts_ptt_srpt == 0) /* mandatory */
    return 0;

  if(!ifo_seek(ifop,
                   ifofile->vtsi_mat->vts_ptt_srpt * DVD_BLOCK_LEN))
    return 0;

  vts_ptt_srpt = calloc(1, sizeof(vts_ptt_srpt_t));
  if(!vts_ptt_srpt)
    return 0;

  vts_ptt_srpt->title = NULL;
  ifofile->vts_ptt_srpt = vts_ptt_srpt;

  if(!(ifo_read(ifop, vts_ptt_srpt, VTS_PTT_SRPT_SIZE))) {
    IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read PTT search table.");
    goto fail;
  }

  B2N_16(vts_ptt_srpt->nr_of_srpts);
  B2N_32(vts_ptt_srpt->last_byte);

  CHECK_ZERO(vts_ptt_srpt->zero_1);
  CHECK_VALUE(vts_ptt_srpt->nr_of_srpts != 0);
  CHECK_VALUE(vts_ptt_srpt->nr_of_srpts < 100); /* ?? */

  /* E-One releases don't fill this field */
  if(vts_ptt_srpt->last_byte == 0) {
    vts_ptt_srpt->last_byte  = vts_ptt_srpt->nr_of_srpts * sizeof(*data) - 1 + VTS_PTT_SRPT_SIZE;
  }
  info_length = vts_ptt_srpt->last_byte + 1 - VTS_PTT_SRPT_SIZE;
  data = calloc(1, info_length);
  if(!data)
    goto fail;

  if(!(ifo_read(ifop, data, info_length))) {
    IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read PTT search table.");
    goto fail;
  }

  if(vts_ptt_srpt->nr_of_srpts > info_length / sizeof(*data)) {
    IFO_REPORT(ifop, DVD_REPORT_ERROR, "PTT search table too small.");
    goto fail;
  }

  if(vts_ptt_srpt->nr_of_srpts == 0) {
    IFO_REPORT(ifop, DVD_REPORT_ERROR, "Zero entries in PTT search table.");
    goto fail;
  }

  for(i = 0; i < vts_ptt_srpt->nr_of_srpts; i++) {
    /* Transformers 3 has PTT start bytes that point outside the SRPT PTT */
    uint32_t start = data[i];
    B2N_32(start);
    if(start + sizeof(ptt_info_t) > vts_ptt_srpt->last_byte + 1) {
      /* don't mess with any bytes beyond the end of the allocation */
      vts_ptt_srpt->nr_of_srpts = i;
      break;
    }
    data[i] = start;
    /* assert(data[i] + sizeof(ptt_info_t) <= vts_ptt_srpt->last_byte + 1);
       Magic Knight Rayearth Daybreak is mastered very strange and has
       Titles with 0 PTTs. They all have a data[i] offsets beyond the end of
       of the vts_ptt_srpt structure. */
    CHECK_VALUE(data[i] + sizeof(ptt_info_t) <= vts_ptt_srpt->last_byte + 1 + 4);
  }

  vts_ptt_srpt->ttu_offset = data;

  vts_ptt_srpt->title = calloc(vts_ptt_srpt->nr_of_srpts, sizeof(ttu_t));
  if(!vts_ptt_srpt->title)
    goto fail;

  for(i = 0; i < vts_ptt_srpt->nr_of_srpts; i++) {
    int n;
    if(i < vts_ptt_srpt->nr_of_srpts - 1)
      n = (data[i+1] - data[i]);
    else
      n = (vts_ptt_srpt->last_byte + 1 - data[i]);

    /* assert(n > 0 && (n % 4) == 0);
       Magic Knight Rayearth Daybreak is mastered very strange and has
       Titles with 0 PTTs. */
    if(n < 0) n = 0;

    /* DVDs created by the VDR-to-DVD device LG RC590M violate the following requirement */
    CHECK_VALUE(n % 4 == 0);

    vts_ptt_srpt->title[i].nr_of_ptts = n / 4;
    vts_ptt_srpt->title[i].ptt = calloc(n / 4, sizeof(ptt_info_t));
    if(!vts_ptt_srpt->title[i].ptt) {
      for(n = 0; n < i; n++)
        free(vts_ptt_srpt->title[n].ptt);

      goto fail;
    }
    for(j = 0; j < vts_ptt_srpt->title[i].nr_of_ptts; j++) {
      /* The assert placed here because of Magic Knight Rayearth Daybreak */
      CHECK_VALUE(data[i] + sizeof(ptt_info_t) <= vts_ptt_srpt->last_byte + 1);
      vts_ptt_srpt->title[i].ptt[j].pgcn
        = *(uint16_t*)(((char *)data) + data[i] + 4*j - VTS_PTT_SRPT_SIZE);
      vts_ptt_srpt->title[i].ptt[j].pgn
        = *(uint16_t*)(((char *)data) + data[i] + 4*j + 2 - VTS_PTT_SRPT_SIZE);
    }
  }

  for(i = 0; i < vts_ptt_srpt->nr_of_srpts; i++) {
    for(j = 0; j < vts_ptt_srpt->title[i].nr_of_ptts; j++) {
      B2N_16(vts_ptt_srpt->title[i].ptt[j].pgcn);
      B2N_16(vts_ptt_srpt->title[i].ptt[j].pgn);
    }
  }

  for(i = 0; i < vts_ptt_srpt->nr_of_srpts; i++) {
    CHECK_VALUE(vts_ptt_srpt->title[i].nr_of_ptts < 1000); /* ?? */
    for(j = 0; j < vts_ptt_srpt->title[i].nr_of_ptts; j++) {
      CHECK_VALUE(vts_ptt_srpt->title[i].ptt[j].pgcn != 0 );
      CHECK_VALUE(vts_ptt_srpt->title[i].ptt[j].pgcn < 1000); /* ?? */
      CHECK_VALUE(vts_ptt_srpt->title[i].ptt[j].pgn != 0);
      CHECK_VALUE(vts_ptt_srpt->title[i].ptt[j].pgn < 100); /* ?? */
      //don't abort here. E-One DVDs contain PTT with pgcn or pgn == 0
    }
  }

  return 1;

fail:
  free(data);
  ifofile->vts_ptt_srpt = NULL;
  free(vts_ptt_srpt->title);
  free(vts_ptt_srpt);
  return 0;
}


void ifoFree_VTS_PTT_SRPT(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  if(ifofile->vts_ptt_srpt) {
    int i;
    for(i = 0; i < ifofile->vts_ptt_srpt->nr_of_srpts; i++)
      free(ifofile->vts_ptt_srpt->title[i].ptt);
    free(ifofile->vts_ptt_srpt->ttu_offset);
    free(ifofile->vts_ptt_srpt->title);
    free(ifofile->vts_ptt_srpt);
    ifofile->vts_ptt_srpt = 0;
  }
}


int ifoRead_PTL_MAIT(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  ptl_mait_t *ptl_mait;
  int info_length;
  unsigned int i, j;

  if(!ifofile)
    return 0;

  if(!ifofile->vmgi_mat)
    return 0;

  if(!ifofile->vmgi_mat->ptl_mait)
    return 1;

  if(!ifo_seek(ifop, ifofile->vmgi_mat->ptl_mait * DVD_BLOCK_LEN))
    return 0;

  ptl_mait = calloc(1, sizeof(ptl_mait_t));
  if(!ptl_mait)
    return 0;

  ifofile->ptl_mait = ptl_mait;

  if(!(ifo_read(ifop, ptl_mait, PTL_MAIT_SIZE))) {
    free(ptl_mait);
    ifofile->ptl_mait = NULL;
    return 0;
  }

  B2N_16(ptl_mait->nr_of_countries);
  B2N_16(ptl_mait->nr_of_vtss);
  B2N_32(ptl_mait->last_byte);

  CHECK_VALUE(ptl_mait->nr_of_countries != 0);
  CHECK_VALUE(ptl_mait->nr_of_countries < 100); /* ?? */
  CHECK_VALUE(ptl_mait->nr_of_vtss != 0);
  CHECK_VALUE(ptl_mait->nr_of_vtss < 100); /* ?? */
  CHECK_VALUE(ptl_mait->nr_of_countries * PTL_MAIT_COUNTRY_SIZE
              <= ptl_mait->last_byte + 1 - PTL_MAIT_SIZE);

  info_length = ptl_mait->nr_of_countries * sizeof(ptl_mait_country_t);
  ptl_mait->countries = calloc(1, info_length);
  if(!ptl_mait->countries) {
    free(ptl_mait);
    ifofile->ptl_mait = NULL;
    return 0;
  }
  for(i = 0; i < ptl_mait->nr_of_countries; i++) {
    ptl_mait->countries[i].pf_ptl_mai = NULL;
  }

  for(i = 0; i < ptl_mait->nr_of_countries; i++) {
    if(!(ifo_read(ifop, &ptl_mait->countries[i], PTL_MAIT_COUNTRY_SIZE))) {
      IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read PTL_MAIT.");
      free(ptl_mait->countries);
      free(ptl_mait);
      ifofile->ptl_mait = NULL;
      return 0;
    }
  }

  for(i = 0; i < ptl_mait->nr_of_countries; i++) {
    B2N_16(ptl_mait->countries[i].country_code);
    B2N_16(ptl_mait->countries[i].pf_ptl_mai_start_byte);
  }

  for(i = 0; i < ptl_mait->nr_of_countries; i++) {
    CHECK_ZERO(ptl_mait->countries[i].zero_1);
    CHECK_ZERO(ptl_mait->countries[i].zero_2);
    CHECK_VALUE(ptl_mait->countries[i].pf_ptl_mai_start_byte
                + sizeof(pf_level_t) * (ptl_mait->nr_of_vtss + 1) <= ptl_mait->last_byte + 1);
  }

  for(i = 0; i < ptl_mait->nr_of_countries; i++) {
    uint16_t *pf_temp;

    if(!ifo_seek(ifop,
                     ifofile->vmgi_mat->ptl_mait * DVD_BLOCK_LEN
                     + ptl_mait->countries[i].pf_ptl_mai_start_byte)) {
      IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to seek PTL_MAIT table at index %d.",i);
      free(ptl_mait->countries);
      free(ptl_mait);
      ifofile->ptl_mait = NULL;
      return 0;
    }
    info_length = (ptl_mait->nr_of_vtss + 1) * sizeof(pf_level_t);
    pf_temp = calloc(1, info_length);
    if(!pf_temp) {
      free_ptl_mait(ptl_mait, i);
      ifofile->ptl_mait = NULL;
      return 0;
    }
    if(!(ifo_read(ifop, pf_temp, info_length))) {
      IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read PTL_MAIT table at index %d.",i);
      free(pf_temp);
      free_ptl_mait(ptl_mait, i);
      ifofile->ptl_mait = NULL;
      return 0;
    }
    for (j = 0; j < ((ptl_mait->nr_of_vtss + 1U) * 8U); j++) {
      B2N_16(pf_temp[j]);
    }
    ptl_mait->countries[i].pf_ptl_mai = calloc(1, info_length);
    if(!ptl_mait->countries[i].pf_ptl_mai) {
      free(pf_temp);
      free_ptl_mait(ptl_mait, i);
      ifofile->ptl_mait = NULL;
      return 0;
    }
    { /* Transpose the array so we can use C indexing. */
      int level, vts;
      for(level = 0; level < PTL_MAIT_NUM_LEVEL; level++) {
        for(vts = 0; vts <= ptl_mait->nr_of_vtss; vts++) {
          ptl_mait->countries[i].pf_ptl_mai[vts][level] =
            pf_temp[(7-level)*(ptl_mait->nr_of_vtss+1) + vts];
        }
      }
      free(pf_temp);
    }
  }
  return 1;
}

void ifoFree_PTL_MAIT(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  if(ifofile->ptl_mait) {
    unsigned int i;

    for(i = 0; i < ifofile->ptl_mait->nr_of_countries; i++) {
      free(ifofile->ptl_mait->countries[i].pf_ptl_mai);
    }
    free(ifofile->ptl_mait->countries);
    free(ifofile->ptl_mait);
    ifofile->ptl_mait = NULL;
  }
}

int ifoRead_VTS_TMAPT(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  vts_tmapt_t *vts_tmapt;
  uint32_t *vts_tmap_srp;
  unsigned int offset;
  int info_length;
  unsigned int i, j;

  if(!ifofile)
    return 0;

  if(!ifofile->vtsi_mat)
    return 0;

  if(ifofile->vtsi_mat->vts_tmapt == 0) {
    ifofile->vts_tmapt = NULL;
    return 1;
  }

  offset = ifofile->vtsi_mat->vts_tmapt * DVD_BLOCK_LEN;

  if(!ifo_seek(ifop, offset))
    return 0;

  vts_tmapt = calloc(1, sizeof(vts_tmapt_t));
  if(!vts_tmapt)
    return 0;

  ifofile->vts_tmapt = vts_tmapt;

  if(!(ifo_read(ifop, vts_tmapt, VTS_TMAPT_SIZE))) {
    IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read VTS_TMAPT.");
    free(vts_tmapt);
    ifofile->vts_tmapt = NULL;
    return 0;
  }

  B2N_16(vts_tmapt->nr_of_tmaps);
  B2N_32(vts_tmapt->last_byte);

  CHECK_ZERO(vts_tmapt->zero_1);

  info_length = vts_tmapt->nr_of_tmaps * 4;

  vts_tmap_srp = calloc(1, info_length);
  if(!vts_tmap_srp) {
    free(vts_tmapt);
    ifofile->vts_tmapt = NULL;
    return 0;
  }

  vts_tmapt->tmap_offset = vts_tmap_srp;

  if(!(ifo_read(ifop, vts_tmap_srp, info_length))) {
    IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read VTS_TMAPT.");
    free(vts_tmap_srp);
    free(vts_tmapt);
    ifofile->vts_tmapt = NULL;
    return 0;
  }

  for (i = 0; i < vts_tmapt->nr_of_tmaps; i++) {
    B2N_32(vts_tmap_srp[i]);
  }


  info_length = vts_tmapt->nr_of_tmaps * sizeof(vts_tmap_t);

  vts_tmapt->tmap = calloc(1, info_length);
  if(!vts_tmapt->tmap) {
    free(vts_tmap_srp);
    free(vts_tmapt);
    ifofile->vts_tmapt = NULL;
    return 0;
  }

  for(i = 0; i < vts_tmapt->nr_of_tmaps; i++) {
    if(!ifo_seek(ifop, offset + vts_tmap_srp[i])) {
      ifoFree_VTS_TMAPT(ifofile);
      return 0;
    }

    if(!(ifo_read(ifop, &vts_tmapt->tmap[i], VTS_TMAP_SIZE))) {
      IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read VTS_TMAP.");
      ifoFree_VTS_TMAPT(ifofile);
      return 0;
    }

    B2N_16(vts_tmapt->tmap[i].nr_of_entries);
    CHECK_ZERO(vts_tmapt->tmap[i].zero_1);

    if(vts_tmapt->tmap[i].nr_of_entries == 0) { /* Early out if zero entries */
      vts_tmapt->tmap[i].map_ent = NULL;
      continue;
    }

    info_length = vts_tmapt->tmap[i].nr_of_entries * sizeof(map_ent_t);

    vts_tmapt->tmap[i].map_ent = calloc(1, info_length);
    if(!vts_tmapt->tmap[i].map_ent) {
      ifoFree_VTS_TMAPT(ifofile);
      return 0;
    }

    if(!(ifo_read(ifop, vts_tmapt->tmap[i].map_ent, info_length))) {
      IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read VTS_TMAP_ENT.");
      ifoFree_VTS_TMAPT(ifofile);
      return 0;
    }

    for(j = 0; j < vts_tmapt->tmap[i].nr_of_entries; j++)
      B2N_32(vts_tmapt->tmap[i].map_ent[j]);
  }

  return 1;
}

void ifoFree_VTS_TMAPT(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  if(ifofile->vts_tmapt) {
    unsigned int i;

    for(i = 0; i < ifofile->vts_tmapt->nr_of_tmaps; i++)
      if(ifofile->vts_tmapt->tmap[i].map_ent)
        free(ifofile->vts_tmapt->tmap[i].map_ent);
    free(ifofile->vts_tmapt->tmap);
    free(ifofile->vts_tmapt->tmap_offset);
    free(ifofile->vts_tmapt);
    ifofile->vts_tmapt = NULL;
  }
}


int ifoRead_TITLE_C_ADT(ifo_handle_t *ifofile) {

  if(!ifofile)
    return 0;

  if(!ifofile->vtsi_mat)
    return 0;

  if(ifofile->vtsi_mat->vts_c_adt == 0) /* mandatory */
    return 0;

  ifofile->vts_c_adt = calloc(1, sizeof(c_adt_t));
  if(!ifofile->vts_c_adt)
    return 0;

  if(!ifoRead_C_ADT_internal(ifofile, ifofile->vts_c_adt,
                             ifofile->vtsi_mat->vts_c_adt)) {
    free(ifofile->vts_c_adt);
    ifofile->vts_c_adt = NULL;
    return 0;
  }

  return 1;
}

int ifoRead_C_ADT(ifo_handle_t *ifofile) {
  unsigned int sector;

  if(!ifofile)
    return 0;

  if(ifofile->vmgi_mat) {
    if(ifofile->vmgi_mat->vmgm_c_adt == 0)
      return 1;
    sector = ifofile->vmgi_mat->vmgm_c_adt;
  } else if(ifofile->vtsi_mat) {
    if(ifofile->vtsi_mat->vtsm_c_adt == 0)
      return 1;
    sector = ifofile->vtsi_mat->vtsm_c_adt;
  } else {
    return 0;
  }

  ifofile->menu_c_adt = calloc(1, sizeof(c_adt_t));
  if(!ifofile->menu_c_adt)
    return 0;

  if(!ifoRead_C_ADT_internal(ifofile, ifofile->menu_c_adt, sector)) {
    free(ifofile->menu_c_adt);
    ifofile->menu_c_adt = NULL;
    return 0;
  }

  return 1;
}

static int ifoRead_C_ADT_internal(ifo_handle_t *ifofile,
                                  c_adt_t *c_adt, unsigned int sector) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  size_t i, info_length;

  if(!ifo_seek(ifop, sector * DVD_BLOCK_LEN))
    return 0;

  if(!(ifo_read(ifop, c_adt, C_ADT_SIZE)))
    return 0;

  B2N_16(c_adt->nr_of_vobs);
  B2N_32(c_adt->last_byte);

  if(c_adt->last_byte + 1 < C_ADT_SIZE)
    return 0;

  info_length = c_adt->last_byte + 1 - C_ADT_SIZE;

  CHECK_ZERO(c_adt->zero_1);
  /* assert(c_adt->nr_of_vobs > 0);
     Magic Knight Rayearth Daybreak is mastered very strange and has
     Titles with a VOBS that has no cells. */
  CHECK_VALUE(info_length % sizeof(cell_adr_t) == 0);

  /* assert(info_length / sizeof(cell_adr_t) >= c_adt->nr_of_vobs);
     Enemy of the State region 2 (de) has Titles where nr_of_vobs field
     is to high, they high ones are never referenced though. */
  if(info_length / sizeof(cell_adr_t) < c_adt->nr_of_vobs) {
    IFO_REPORT(ifop, DVD_REPORT_WARN, "C_ADT nr_of_vobs > available info entries");
    c_adt->nr_of_vobs = info_length / sizeof(cell_adr_t);
  }

  c_adt->cell_adr_table = calloc(1, info_length);
  if(!c_adt->cell_adr_table)
    return 0;

  if(info_length &&
     !(ifo_read(ifop, c_adt->cell_adr_table, info_length))) {
    free(c_adt->cell_adr_table);
    return 0;
  }

  for(i = 0; i < info_length/sizeof(cell_adr_t); i++) {
    B2N_16(c_adt->cell_adr_table[i].vob_id);
    B2N_32(c_adt->cell_adr_table[i].start_sector);
    B2N_32(c_adt->cell_adr_table[i].last_sector);

    CHECK_ZERO(c_adt->cell_adr_table[i].zero_1);
    CHECK_VALUE(c_adt->cell_adr_table[i].vob_id > 0);
    CHECK_VALUE(c_adt->cell_adr_table[i].vob_id <= c_adt->nr_of_vobs);
    CHECK_VALUE(c_adt->cell_adr_table[i].cell_id > 0);
    CHECK_VALUE(c_adt->cell_adr_table[i].start_sector <
                c_adt->cell_adr_table[i].last_sector);
  }

  return 1;
}


static void ifoFree_C_ADT_internal(c_adt_t *c_adt) {
  if(c_adt) {
    free(c_adt->cell_adr_table);
    free(c_adt);
  }
}

void ifoFree_C_ADT(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  ifoFree_C_ADT_internal(ifofile->menu_c_adt);
  ifofile->menu_c_adt = NULL;
}

void ifoFree_TITLE_C_ADT(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  ifoFree_C_ADT_internal(ifofile->vts_c_adt);
  ifofile->vts_c_adt = NULL;
}

int ifoRead_TITLE_VOBU_ADMAP(ifo_handle_t *ifofile) {
  if(!ifofile)
    return 0;

  if(!ifofile->vtsi_mat)
    return 0;

  if(ifofile->vtsi_mat->vts_vobu_admap == 0) /* mandatory */
    return 0;

  ifofile->vts_vobu_admap = calloc(1, sizeof(vobu_admap_t));
  if(!ifofile->vts_vobu_admap)
    return 0;

  if(!ifoRead_VOBU_ADMAP_internal(ifofile, ifofile->vts_vobu_admap,
                                  ifofile->vtsi_mat->vts_vobu_admap)) {
    free(ifofile->vts_vobu_admap);
    ifofile->vts_vobu_admap = NULL;
    return 0;
  }

  return 1;
}

int ifoRead_VOBU_ADMAP(ifo_handle_t *ifofile) {
  unsigned int sector;

  if(!ifofile)
    return 0;

  if(ifofile->vmgi_mat) {
    if(ifofile->vmgi_mat->vmgm_vobu_admap == 0)
      return 1;
    sector = ifofile->vmgi_mat->vmgm_vobu_admap;
  } else if(ifofile->vtsi_mat) {
    if(ifofile->vtsi_mat->vtsm_vobu_admap == 0)
      return 1;
    sector = ifofile->vtsi_mat->vtsm_vobu_admap;
  } else {
    return 0;
  }

  ifofile->menu_vobu_admap = calloc(1, sizeof(vobu_admap_t));
  if(!ifofile->menu_vobu_admap)
    return 0;

  if(!ifoRead_VOBU_ADMAP_internal(ifofile, ifofile->menu_vobu_admap, sector)) {
    free(ifofile->menu_vobu_admap);
    ifofile->menu_vobu_admap = NULL;
    return 0;
  }

  return 1;
}

static int ifoRead_VOBU_ADMAP_internal(ifo_handle_t *ifofile,
                                       vobu_admap_t *vobu_admap,
                                       unsigned int sector) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  unsigned int i;
  int info_length;

  if(!DVDFileSeekForce_(ifop->file, sector * DVD_BLOCK_LEN, sector))
    return 0;

  if(!(ifo_read(ifop, vobu_admap, VOBU_ADMAP_SIZE)))
    return 0;

  B2N_32(vobu_admap->last_byte);

  info_length = vobu_admap->last_byte + 1 - VOBU_ADMAP_SIZE;
  /* assert(info_length > 0);
     Magic Knight Rayearth Daybreak is mastered very strange and has
     Titles with a VOBS that has no VOBUs. */
  CHECK_VALUE(info_length % sizeof(uint32_t) == 0);

  vobu_admap->vobu_start_sectors = calloc(1, info_length);
  if(!vobu_admap->vobu_start_sectors) {
    return 0;
  }
  if(info_length &&
     !(ifo_read(ifop, vobu_admap->vobu_start_sectors, info_length))) {
    free(vobu_admap->vobu_start_sectors);
    return 0;
  }

  for(i = 0; i < info_length/sizeof(uint32_t); i++)
    B2N_32(vobu_admap->vobu_start_sectors[i]);

  return 1;
}


static void ifoFree_VOBU_ADMAP_internal(vobu_admap_t *vobu_admap) {
  if(vobu_admap) {
    free(vobu_admap->vobu_start_sectors);
    free(vobu_admap);
  }
}

void ifoFree_VOBU_ADMAP(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  ifoFree_VOBU_ADMAP_internal(ifofile->menu_vobu_admap);
  ifofile->menu_vobu_admap = NULL;
}

void ifoFree_TITLE_VOBU_ADMAP(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  ifoFree_VOBU_ADMAP_internal(ifofile->vts_vobu_admap);
  ifofile->vts_vobu_admap = NULL;
}

/* ---- the program chain table for title lists (ifoRead_VTS_PGCIT_rec) ----
 * Read inside the table only: its whole sectors up to its last byte, a field
 * past them reads as 0. */

#define PGC_REC_MAX_COMMANDS 128      /* pre + post + cell commands of one program chain */

typedef struct {
  struct ifo_handle_private_s *ifop;
  const uint8_t *p;
  uint32_t len;                       /* the table's bytes read */
  int64_t  pos;                       /* the table's byte offset in the file */
  int64_t  outside;                   /* the first field read past them (file offset), -1: none */
} rec_table_t;

static void rec_report(rec_table_t *t, dvd_report_level_t level, int64_t offset, const char *fmt, ...)
{
  dvd_reader_t *ctx = t->ifop->ctx;
  char text[320];
  va_list ap;
  dvd_report_t r = { level, t->ifop->name[0] ? t->ifop->name : NULL, "VTS_PGCIT", NULL, offset, -1,
                     NULL, 0, text };

  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  DVDReadReport(ctx->priv, &ctx->logcb, &r);
}

static void rec_report_table(rec_table_t *t, const char *table, dvd_report_level_t level, int64_t offset,
                             const char *fmt, ...)
{
  dvd_reader_t *ctx = t->ifop->ctx;
  char text[320];
  va_list ap;
  dvd_report_t r = { level, t->ifop->name[0] ? t->ifop->name : NULL, table, NULL, offset, -1, NULL, 0, text };

  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  DVDReadReport(ctx->priv, &ctx->logcb, &r);
}

static void rec_outside(rec_table_t *t, uint32_t o)
{
  if(t->outside < 0)
    t->outside = t->pos + o;
}

static uint8_t rec_B(rec_table_t *t, uint32_t o)
{
  if((uint64_t)o + 1 > t->len) { rec_outside(t, o); return 0; }
  return t->p[o];
}

static uint16_t rec_W(rec_table_t *t, uint32_t o)
{
  if((uint64_t)o + 2 > t->len) { rec_outside(t, o); return 0; }
  return (uint16_t)(t->p[o] << 8 | t->p[o + 1]);
}

static uint32_t rec_L(rec_table_t *t, uint32_t o)
{
  if((uint64_t)o + 4 > t->len) { rec_outside(t, o); return 0; }
  return (uint32_t)t->p[o] << 24 | (uint32_t)t->p[o + 1] << 16 | (uint32_t)t->p[o + 2] << 8 | t->p[o + 3];
}

static void free_pgc_rec(pgc_rec_t **pgc)
{
  if(!*pgc)
    return;
  free((*pgc)->commands);
  free((*pgc)->program_map);
  free((*pgc)->cells);
  free(*pgc);
  *pgc = NULL;
}

/* Program chain i at off of the table: *out, or NULL when it is left out
 * (reported); -1 out of memory. */
static int rec_pgc(rec_table_t *t, int i, uint32_t entry, uint32_t off, pgc_rec_t **out)
{
  pgc_rec_t *p;
  uint16_t pm, cply, cpos, ct;

  *out = NULL;
  t->outside = -1;
  if(!(p = calloc(1, sizeof(*p))))
    return -1;
  p->number         = i;
  p->entry          = entry;
  p->nr_of_programs = rec_B(t, off + 2);
  p->nr_of_cells    = rec_B(t, off + 3);
  p->playback_time  = rec_L(t, off + 4);
  p->prohibited_ops = rec_L(t, off + 8);
  if((p->nr_of_programs && !(p->program_map = calloc(p->nr_of_programs, 1))) ||
     (p->nr_of_cells && !(p->cells = calloc(p->nr_of_cells, sizeof(*p->cells))))) {
    free_pgc_rec(&p);
    return -1;
  }
  pm = rec_W(t, off + 0xe6);
  for(int k = 0; k < p->nr_of_programs; k++)
    p->program_map[k] = rec_B(t, off + pm + k);
  cpos = rec_W(t, off + 0xea);
  cply = rec_W(t, off + 0xe8);
  if(!p->nr_of_cells) {
    *out = p;                         /* no cells: nothing more is read */
    return 1;
  }
  /* a cell table pointer 0 reads the cells from the program chain's own
   * start (as the title list's reference reads them; looked into again
   * later) */
  if(!cply || !cpos)
    rec_report(t, DVD_REPORT_WARN, t->pos + off + (cply ? 0xea : 0xe8),
               "program chain %d has %d cell(s) but its cell %s table pointer is 0: they are read from the "
               "program chain's start", i + 1, p->nr_of_cells, cply ? "position" : "playback");
  for(int c = 0; c < p->nr_of_cells; c++) {
    cell_rec_t *cl = &p->cells[c];
    uint32_t cp = off + cply + 0x18 * c;
    cl->vob_id_nr = rec_W(t, off + cpos + 4 * c);
    cl->cell_nr   = rec_B(t, off + cpos + 4 * c + 3);
    if((uint64_t)cp + 4 <= t->len)
      memcpy(cl->category, t->p + cp, 4);
    cl->playback_time          = rec_L(t, cp + 4);
    cl->first_sector           = rec_L(t, cp + 8);
    cl->first_ilvu_end_sector  = rec_L(t, cp + 0x0c);
    cl->last_vobu_start_sector = rec_L(t, cp + 0x10);
    cl->last_sector            = rec_L(t, cp + 0x14);
  }
  if((uint64_t)off + 0x9c > t->len)
    goto broken;
  memcpy(p->audio_control, t->p + off + 0x0c, 16);
  memcpy(p->subp_control, t->p + off + 0x1c, 128);
  if((uint64_t)off + 0xe6 <= t->len)
    ct = (uint16_t)(t->p[off + 0xe4] << 8 | t->p[off + 0xe5]);
  else {
    rec_outside(t, off + 0xe4);
    ct = 0;
  }
  if(ct) {
    uint32_t b = off + ct, n;
    p->nr_of_pre  = rec_W(t, b);
    p->nr_of_post = rec_W(t, b + 2);
    p->nr_of_cell = rec_W(t, b + 4);
    n = (uint32_t)p->nr_of_pre + p->nr_of_post + p->nr_of_cell;
    if(n > PGC_REC_MAX_COMMANDS) {
      rec_report(t, DVD_REPORT_WARN, t->pos + b, "program chain %d has %u commands (more than %d): it is left out",
                 i + 1, n, PGC_REC_MAX_COMMANDS);
      free_pgc_rec(&p);
      return 1;
    }
    /* the command table's header and its commands inside the table (also
     * without commands: one byte past the header) */
    if((uint64_t)b + 8 >= t->len || (uint64_t)b + n * 8 + 8 > t->len)
      goto broken;
    if(n) {
      if(!(p->commands = malloc(n * 8))) {
        free_pgc_rec(&p);
        return -1;
      }
      memcpy(p->commands, t->p + b + 8, n * 8);
    }
  }
  for(int j = 0; j < 16; j++)
    p->palette[j] = rec_L(t, off + 0xa4 + 4 * j);
  if(t->outside >= 0)
    rec_report(t, DVD_REPORT_WARN, t->outside, "program chain %d: fields past the table's end are read as 0",
               i + 1);
  *out = p;
  return 1;
broken:
  rec_report(t, DVD_REPORT_WARN, t->pos + off, "program chain %d runs past the end of VTS_PGCIT: it is left out",
             i + 1);
  free_pgc_rec(&p);
  return 1;
}

/* The information file's size by its header (VMG or VTS). */
static int64_t rec_ifo_size(const ifo_handle_t *ifofile)
{
  uint32_t last = ifofile->vtsi_mat ? ifofile->vtsi_mat->vtsi_last_sector :
                  ifofile->vmgi_mat ? ifofile->vmgi_mat->vmgi_last_sector : 0;
  return ((int64_t)(last & 0x1ffff) + 1) * DVD_BLOCK_LEN;
}

/* A table's length from its first sector: last_byte_at >= 0, its last-byte
 * field there (+ 1; 0 = empty when that does not reach past the field);
 * else a title table: its entries (at most max_entries) x 12 + 8. */
static uint32_t rec_len(const uint8_t *first, int last_byte_at, unsigned max_entries)
{
  if(last_byte_at < 0) {
    unsigned n = (unsigned)first[0] << 8 | first[1];
    return (n < max_entries ? n : max_entries) * 12 + 8;
  } else {
    uint32_t len = ((uint32_t)first[last_byte_at] << 24 | (uint32_t)first[last_byte_at + 1] << 16 |
                    (uint32_t)first[last_byte_at + 2] << 8 | first[last_byte_at + 3]) + 1;
    return len < (uint32_t)last_byte_at + 4 ? 0 : len;
  }
}

static int rec_read(ifo_handle_t *ifofile, uint32_t sector, int last_byte_at, unsigned max_entries,
                    const char *table, uint8_t **data, uint32_t *size)
{
  struct ifo_handle_private_s *ifop;
  uint8_t first[DVD_BLOCK_LEN];
  rec_table_t t = { 0 };
  int64_t ifo_size;
  uint32_t len;

  *data = NULL;
  *size = 0;
  if(!ifofile || (!ifofile->vtsi_mat && !ifofile->vmgi_mat) || last_byte_at + 4 > DVD_BLOCK_LEN)
    return 0;
  ifop     = PRIV(ifofile);
  t.ifop   = ifop;
  t.pos    = (int64_t)sector * DVD_BLOCK_LEN;
  ifo_size = rec_ifo_size(ifofile);
  if(t.pos + DVD_BLOCK_LEN > ifo_size || !ifo_seek(ifop, (uint32_t)t.pos) ||
     !ifo_read(ifop, first, DVD_BLOCK_LEN)) {
    rec_report_table(&t, table, DVD_REPORT_ERROR, t.pos, "%s at sector %u cannot be read (the information file "
                     "has %" PRId64 " bytes)", table, sector, ifo_size);
    return 0;
  }
  if(!(len = rec_len(first, last_byte_at, max_entries)))
    return 1;                         /* empty */
  if(len > 0x3fffffff || t.pos + (len > DVD_BLOCK_LEN ? len : DVD_BLOCK_LEN) > ifo_size) {
    rec_report_table(&t, table, DVD_REPORT_ERROR, t.pos + (last_byte_at > 0 ? last_byte_at : 0),
                     "%s's %u bytes at sector %u lie past the information file's %" PRId64 " bytes: it cannot be "
                     "read", table, len, sector, ifo_size);
    return 0;
  }
  if(!(*data = malloc(len)))
    return 0;
  memcpy(*data, first, len < DVD_BLOCK_LEN ? len : DVD_BLOCK_LEN);
  if(len > DVD_BLOCK_LEN && (!ifo_seek(ifop, (uint32_t)t.pos + DVD_BLOCK_LEN) ||
                             !ifo_read(ifop, *data + DVD_BLOCK_LEN, len - DVD_BLOCK_LEN))) {
    rec_report_table(&t, table, DVD_REPORT_ERROR, t.pos + DVD_BLOCK_LEN, "%s at sector %u cannot be read",
                     table, sector);
    free(*data);
    *data = NULL;
    return 0;
  }
  *size = len;
  return 1;
}

int ifoRead_table_rec(ifo_handle_t *ifofile, uint32_t sector, unsigned last_byte_at, const char *table,
                      uint8_t **data, uint32_t *size)
{
  return rec_read(ifofile, sector, (int)last_byte_at, 0, table, data, size);
}

int ifoRead_TT_SRPT_rec(ifo_handle_t *ifofile, unsigned max_entries, uint8_t **data, uint32_t *size)
{
  *data = NULL;
  *size = 0;
  if(!ifofile || !ifofile->vmgi_mat)
    return 0;
  return rec_read(ifofile, ifofile->vmgi_mat->tt_srpt, -1, max_entries, "TT_SRPT", data, size);
}

/* ---- the information file opened by the title list's rules (ifoOpen_rec) ---- */

#define REC_IFO_CACHE_MAX   (2 << 20)   /* an IFO read whole when it is no larger */
#define REC_IFO_CACHE_TRIES 16          /* tries of that read (alternating IFO and BUP) */
#define REC_HEADER_SIZE     0x400

static void rec_note(struct ifo_handle_private_s *ifop, dvd_report_level_t level, int64_t offset,
                     const char *fmt, ...)
{
  dvd_reader_t *ctx = ifop->ctx;
  char text[320];
  va_list ap;
  dvd_report_t r = { level, ifop->name[0] ? ifop->name : NULL, NULL, NULL, offset, -1, NULL, 0, text };

  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  DVDReadReport(ctx->priv, &ctx->logcb, &r);
}

/* len bytes of file f at pos, all or nothing. */
static int rec_file_read(dvd_file_t *f, int64_t pos, uint8_t *buf, size_t len)
{
  return pos >= 0 && pos <= INT32_MAX && DVDFileSeek(f, (int32_t)pos) == (int32_t)pos &&
         DVDReadBytes(f, buf, len) == (ssize_t)len;
}

/* The chosen bytes: from the IFO read whole, else per 2048-byte piece from
 * the IFO, the BUP when that fails (only the BUP when the IFO's signature is
 * wrong). */
static int rec_bytes(struct ifo_handle_private_s *ifop, int64_t pos, uint8_t *buf, size_t len)
{
  dvd_file_t *bup = ifop->bup ? ifop->bup : ifop->file;

  if(ifop->rec_cache) {
    if(pos < 0 || pos + (int64_t)len > ifop->rec_cache_size) {
      rec_note(ifop, DVD_REPORT_ERROR, pos, "%zu bytes at byte %" PRId64 " lie past the IFO's %d bytes",
               len, pos, ifop->rec_cache_size);
      return 0;
    }
    memcpy(buf, ifop->rec_cache + pos, len);
    return 1;
  }
  for(size_t done = 0; done < len; ) {
    size_t n = len - done < DVD_BLOCK_LEN ? len - done : DVD_BLOCK_LEN;
    int64_t at = pos + done;
    if(ifop->bup_only || !rec_file_read(ifop->file, at, buf + done, n)) {
      if(!ifop->bup_only)
        rec_note(ifop, DVD_REPORT_WARN, at, "IFO bytes %" PRId64 "-%" PRId64 " read from the BUP", at, at + n - 1);
      if(!rec_file_read(bup, at, buf + done, n)) {
        rec_note(ifop, DVD_REPORT_ERROR, at, "IFO bytes %" PRId64 "-%" PRId64 " cannot be read from the IFO or "
                 "the BUP", at, at + n - 1);
        return 0;
      }
    }
    done += n;
  }
  return 1;
}

/* The whole IFO: tries per piece, each failed try moving to the other copy;
 * after the first failed try one sector at a time. */
static int rec_read_whole(struct ifo_handle_private_s *ifop, uint8_t *buf, int len)
{
  dvd_file_t *copies[2] = { ifop->file, ifop->bup ? ifop->bup : ifop->file };
  int nb = 2, cur = 0, single = 0;

  if(ifop->bup_only) {
    copies[0] = copies[1];
    nb = 1;
  }
  for(int off = 0; off < len; ) {
    int tries = 0, want;
    for(;;) {
      int ok, past;
      if(tries && !single)
        single = 1;
      want = single ? (len - off < DVD_BLOCK_LEN ? len - off : DVD_BLOCK_LEN) : len - off;
      past = (int64_t)off + want > (int64_t)DVDFileSize(copies[cur]) * DVD_BLOCK_LEN;
      ok   = !past && rec_file_read(copies[cur], off, buf + off, want);
      if(ok)
        break;
      tries++;
      rec_note(ifop, DVD_REPORT_WARN, off, "reading the whole IFO, %d bytes at %d from the %s failed (%s); try "
               "%d of %d", want, off, copies[cur] == ifop->file && !ifop->bup_only ? "IFO" : "BUP",
               past ? "past the end of the file" : "a read error", tries, REC_IFO_CACHE_TRIES);
      if(tries >= REC_IFO_CACHE_TRIES)
        return 0;
      cur = (cur + 1) % nb;
    }
    off += want;
  }
  return 1;
}

static uint32_t rec_be32(const uint8_t *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Where the header puts the BUP against where it is: on a disc image by
 * the files' places, in a folder by the sizes of the IFO and the VOB files
 * before the BUP. */
static void rec_check_bup(struct ifo_handle_private_s *ifop, int title, const uint8_t *header)
{
  char base[16], path[48];
  dvdstat_t ifo, bup, st;
  int64_t want = (int64_t)(uint32_t)(rec_be32(header + 0x0c) - rec_be32(header + 0x1c)) * DVD_BLOCK_LEN, have;
  int image;

  if(title)
    snprintf(base, sizeof(base), "VTS_%02d", title);
  else
    snprintf(base, sizeof(base), "VIDEO_TS");
  snprintf(path, sizeof(path), title ? "/VIDEO_TS/%s_0.IFO" : "/VIDEO_TS/%s.IFO", base);
  if(DVDStatFile_(ifop->ctx, path, &ifo) < 0)
    return;
  snprintf(path, sizeof(path), title ? "/VIDEO_TS/%s_0.BUP" : "/VIDEO_TS/%s.BUP", base);
  if(DVDStatFile_(ifop->ctx, path, &bup) < 0)
    return;
  image = ifo.sector >= 0 && bup.sector >= 0;
  if(image) {
    have = (bup.sector - ifo.sector) * DVD_BLOCK_LEN;
  } else {
    have = ifo.size;
    snprintf(path, sizeof(path), title ? "/VIDEO_TS/%s_0.VOB" : "/VIDEO_TS/%s.VOB", base);
    if(DVDStatFile_(ifop->ctx, path, &st) >= 0)
      have += st.size;
    for(int part = 1; title && part <= 9; part++) {
      snprintf(path, sizeof(path), "/VIDEO_TS/%s_%d.VOB", base, part);
      if(DVDStatFile_(ifop->ctx, path, &st) < 0)
        break;
      have += st.size;
    }
  }
  if(have != want)
    rec_note(ifop, DVD_REPORT_WARN, 0x0c, "the header puts the BUP %" PRId64 " sectors after the IFO "
             "(vts_last_sector %u - vtsi_last_sector %u), %s %" PRId64 " sectors%s", want / DVD_BLOCK_LEN,
             rec_be32(header + 0x0c), rec_be32(header + 0x1c),
             image ? "the file system puts it" : "the IFO and VOB files make", have / DVD_BLOCK_LEN,
             !image && have % DVD_BLOCK_LEN ? " (not whole sectors)" : "");
}

/* The video manager's provider identifier names a program known to rewrite
 * IFOs. */
static void rec_check_provider(struct ifo_handle_private_s *ifop, const uint8_t *header)
{
  const uint8_t *pid = header + 0x40;

  for(int i = 0; i + 4 <= 32; i++)
    if(!memcmp(pid + i, "(Fab", 4)) {
      rec_note(ifop, DVD_REPORT_WARN, 0x40 + i, "the provider identifier (%.32s) says the disc was processed by "
               "DVDFab / MacTheRipper, which are known to rewrite IFOs", (const char *)pid);
      return;
    }
}

/* ---- consistency of an IFO's tables with each other ---- */

/* A finding of table (its bytes at offset in the information file, n of them). */
static void rec_table_note(struct ifo_handle_private_s *ifop, dvd_report_level_t level, const char *table,
                           int64_t offset, const uint8_t *bytes, size_t n, const char *fmt, ...)
{
  dvd_reader_t *ctx = ifop->ctx;
  char text[320];
  va_list ap;
  dvd_report_t r = { level, ifop->name[0] ? ifop->name : NULL, table, NULL, offset, -1, bytes, n, text, 0, 0 };

  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  DVDReadReport(ctx->priv, &ctx->logcb, &r);
}

int ifoCheckCellAddresses_rec(ifo_handle_t *ifofile, const pgcit_rec_t *pgcit)
{
  struct ifo_handle_private_s *ifop;
  uint8_t *d = NULL, raw[4];
  uint32_t size = 0, sector;
  int64_t base;
  int found = 0;

  if(!ifofile || !pgcit)
    return 0;
  ifop = PRIV(ifofile);
  if(!ifoRead_rec_bytes(ifofile, 0xe0, raw, 4) || !(sector = rec_be32(raw)))
    return 0;                             /* no cell address table */
  if(!ifoRead_table_rec(ifofile, sector, 4, "VTS_C_ADT", &d, &size) || !d)
    return 0;                             /* (reported by the reading) */
  base = (int64_t)sector * DVD_BLOCK_LEN;
  for(int i = 0; i < pgcit->nr_of_pgcs; i++) {
    const pgc_rec_t *p = pgcit->pgcs[i];
    if(!p || !p->cells)
      continue;
    for(int c = 0; c < p->nr_of_cells; c++) {
      const cell_rec_t *cl = &p->cells[c];
      uint32_t first = 0, last = 0;
      int64_t at_first = -1, at_last = -1;
      /* the cell's pieces: entries of its VOB id and cell id (12 bytes each from 8) */
      for(uint32_t o = 8; o + 12 <= size; o += 12)
        if(((uint32_t)d[o] << 8 | d[o + 1]) == cl->vob_id_nr && d[o + 2] == cl->cell_nr) {
          if(at_first < 0) {
            first    = rec_be32(d + o + 4);
            at_first = o;
          }
          last    = rec_be32(d + o + 8);
          at_last = o;
        }
      if(at_first < 0) {
        rec_table_note(ifop, DVD_REPORT_WARN, "VTS_C_ADT", base, NULL, 0, "program chain %d cell %d (VOB id %u, "
                       "cell id %u) is not in the cell address table", i + 1, c + 1, cl->vob_id_nr, cl->cell_nr);
        found++;
        continue;
      }
      if(first != cl->first_sector)
        rec_table_note(ifop, DVD_REPORT_WARN, "VTS_C_ADT", base + at_first + 4, d + at_first + 4, 4,
                       "program chain %d cell %d starts at sector %u, its first piece in the cell address table "
                       "at %u", i + 1, c + 1, cl->first_sector, first);
      if(last != cl->last_sector)
        rec_table_note(ifop, DVD_REPORT_WARN, "VTS_C_ADT", base + at_last + 8, d + at_last + 8, 4,
                       "program chain %d cell %d ends at sector %u, its last piece in the cell address table "
                       "at %u", i + 1, c + 1, cl->last_sector, last);
      found += (first != cl->first_sector) + (last != cl->last_sector);
    }
  }
  free(d);
  return found;
}

#define REC_ATRT_ATTRS 0x216              /* VTSI_MAT 0x100..0x315: the menus' and titles' attributes */

int ifoCheckVtsAttributes_rec(ifo_handle_t *vmg, ifo_handle_t *vts, int title)
{
  struct ifo_handle_private_s *vmgp;
  uint8_t *d = NULL, raw[4], own[REC_ATRT_ATTRS];
  uint32_t size = 0, sector, entry, cat;
  int64_t base;
  int found = 0;

  if(!vmg || !vts || title < 1 || title > 99)
    return 0;
  vmgp = PRIV(vmg);
  if(!ifoRead_rec_bytes(vmg, 0xd0, raw, 4) || !(sector = rec_be32(raw)))
    return 0;                             /* no attribute table */
  if(!ifoRead_table_rec(vmg, sector, 4, "VMG_VTS_ATRT", &d, &size) || !d)
    return 0;
  base = (int64_t)sector * DVD_BLOCK_LEN;
  if(size < 8 + 4u * title || (entry = rec_be32(d + 8 + 4 * (title - 1))) + 8 + REC_ATRT_ATTRS > size) {
    rec_table_note(vmgp, DVD_REPORT_WARN, "VMG_VTS_ATRT", base, NULL, 0, "title set %d has no complete entry in "
                   "the title set attribute table", title);
    free(d);
    return 1;
  }
  if(ifoRead_rec_bytes(vts, 0x22, raw, 4) && (cat = rec_be32(raw)) != rec_be32(d + entry + 4)) {
    rec_table_note(vmgp, DVD_REPORT_WARN, "VMG_VTS_ATRT", base + entry + 4, d + entry + 4, 4, "title set %d's "
                   "category is %08x in the title set attribute table, %08x in its own IFO (vts_category)", title,
                   rec_be32(d + entry + 4), cat);
    found++;
  }
  if(ifoRead_rec_bytes(vts, 0x100, own, sizeof(own)) && memcmp(own, d + entry + 8, sizeof(own))) {
    int at = 0;
    while(own[at] == d[entry + 8 + at])
      at++;
    rec_table_note(vmgp, DVD_REPORT_WARN, "VMG_VTS_ATRT", base + entry + 8 + at, d + entry + 8 + at, 1,
                   "title set %d's attributes differ from those in its own IFO, first at VTSI_MAT byte 0x%x (%02x "
                   "there)", title, 0x100 + at, own[at]);
    found++;
  }
  free(d);
  return found;
}

ifo_handle_t *ifoOpen_rec(dvd_reader_t *ctx, int title)
{
  struct ifo_handle_private_s *ifop;
  uint8_t header[REC_HEADER_SIZE];
  const char *sig = title ? "DVDVIDEO-VTS" : "DVDVIDEO-VMG";
  uint32_t size;

  if(!ctx || title < 0 || title > 99 || ctx->dvd_type != DVD_V || !(ifop = calloc(1, sizeof(*ifop))))
    return NULL;
  ifop->ctx = ctx;
  ifop->rec = 1;
  if(title)
    snprintf(ifop->name, sizeof(ifop->name), "VTS_%02d_0.IFO", title);
  else
    snprintf(ifop->name, sizeof(ifop->name), "VIDEO_TS.IFO");
  ifop->file = DVDOpenFile(ctx, title, DVD_READ_INFO_FILE);
  ifop->bup  = DVDOpenFile(ctx, title, DVD_READ_INFO_BACKUP_FILE);
  if(!ifop->file) {
    ifop->file = ifop->bup;           /* the BUP stands in */
    ifop->bup  = NULL;
  }
  if(!ifop->file) {
    rec_note(ifop, DVD_REPORT_ERROR, -1, "neither the IFO nor its BUP can be opened");
    free(ifop);
    return NULL;
  }

  /* the header: the IFO's, the BUP's when the IFO's signature is wrong */
  if(!rec_bytes(ifop, 0, header, sizeof(header)))
    goto fail;
  if(memcmp(header, sig, 12)) {
    rec_note(ifop, DVD_REPORT_WARN, 0, "the IFO does not start with %s: its BUP is read instead", sig);
    ifop->bup_only = 1;
    if(!rec_bytes(ifop, 0, header, sizeof(header)))
      goto fail;
    if(memcmp(header, sig, 12)) {
      rec_note(ifop, DVD_REPORT_ERROR, 0, "neither the IFO nor the BUP starts with %s: it cannot be read", sig);
      goto fail;
    }
  }

  /* the whole IFO, once (its size by its header) */
  size = ((uint32_t)header[0x1c] << 24 | (uint32_t)header[0x1d] << 16 | (uint32_t)header[0x1e] << 8 |
          header[0x1f]) & 0x1ffff;
  size *= DVD_BLOCK_LEN;
  if(size > REC_IFO_CACHE_MAX - DVD_BLOCK_LEN) {
    Log2(ctx, "%s: the IFO is larger than %d bytes: its tables are read from the files", ifop->name,
         REC_IFO_CACHE_MAX);
  } else {
    size += DVD_BLOCK_LEN;
    if(!(ifop->rec_cache = malloc(size)))
      goto fail;
    if(!rec_read_whole(ifop, ifop->rec_cache, size)) {
      rec_note(ifop, DVD_REPORT_WARN, -1, "the whole IFO (%u bytes) cannot be read: its tables are read piece by "
               "piece", size);
      free(ifop->rec_cache);
      ifop->rec_cache = NULL;
    } else {
      ifop->rec_cache_size = size;
    }
  }

  rec_check_bup(ifop, title, header);
  if(!title)
    rec_check_provider(ifop, header);

  /* its main table from the chosen bytes */
  if(!(title ? ifoRead_VTS(&ifop->handle) : ifoRead_VMG(&ifop->handle))) {
    rec_note(ifop, DVD_REPORT_ERROR, 0, "its main table (%s) cannot be read", title ? "VTSI_MAT" : "VMGI_MAT");
    goto fail;
  }
  ifop->handle.ifo_format = IFO_VIDEO;
  return &ifop->handle;
fail:
  ifoClose(&ifop->handle);
  return NULL;
}

int ifoRead_rec_bytes(ifo_handle_t *ifofile, int64_t pos, uint8_t *buf, size_t len)
{
  struct ifo_handle_private_s *ifop;

  if(!ifofile)
    return 0;
  ifop = PRIV(ifofile);
  if(!ifop->rec)
    return rec_file_read(ifop->file, pos, buf, len);
  return rec_bytes(ifop, pos, buf, len);
}

void ifoGet_rec_state(ifo_handle_t *ifofile, int *bup_only, const uint8_t **cache, int *cache_size)
{
  struct ifo_handle_private_s *ifop = PRIV(ifofile);

  *bup_only   = ifop->bup_only;
  *cache      = ifop->rec_cache;
  *cache_size = ifop->rec_cache_size;
}

static int rec_cmp_u32(const void *a, const void *b)
{
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

int ifoRead_VOBU_ADMAP_rec(ifo_handle_t *ifofile, uint32_t sector, const char *table, uint32_t **starts, int *nb)
{
  rec_table_t t = { 0 };
  uint8_t *data;
  uint32_t size, *s;
  int n, k = 0;

  *starts = NULL;
  *nb = 0;
  if(!ifoRead_table_rec(ifofile, sector, 0, table, &data, &size))
    return 0;
  if(size < 4) {
    t.ifop = PRIV(ifofile);
    rec_report_table(&t, table, DVD_REPORT_ERROR, (int64_t)sector * DVD_BLOCK_LEN,
                     "%s at sector %u is shorter than its header", table, sector);
    free(data);
    return 0;
  }
  n = size / 4;
  if(!(s = malloc((n > 1 ? n - 1 : 1) * sizeof(*s)))) {
    free(data);
    return 0;
  }
  for(int i = 1; i < n; i++)
    s[k++] = (uint32_t)data[4 * i] << 24 | (uint32_t)data[4 * i + 1] << 16 |
             (uint32_t)data[4 * i + 2] << 8 | data[4 * i + 3];
  free(data);
  qsort(s, k, sizeof(*s), rec_cmp_u32);
  n = 0;
  for(int i = 0; i < k; i++)
    if(!n || s[n - 1] != s[i])
      s[n++] = s[i];
  *starts = s;
  *nb = n;
  return 1;
}

void ifoFree_VTS_PGCIT_rec(pgcit_rec_t **pgcit)
{
  if(!pgcit || !*pgcit)
    return;
  for(int i = 0; i < (*pgcit)->nr_of_pgcs; i++)
    free_pgc_rec(&(*pgcit)->pgcs[i]);
  free((*pgcit)->pgcs);
  free((*pgcit)->same_as);
  free(*pgcit);
  *pgcit = NULL;
}

int ifoRead_VTS_PGCIT_rec(ifo_handle_t *ifofile, pgcit_rec_t **out)
{
  struct ifo_handle_private_s *ifop;
  rec_table_t t = { 0 };
  uint8_t first[DVD_BLOCK_LEN], *data = NULL;
  uint32_t sector, len, last, *starts = NULL;
  int64_t ifo_size;
  int *start_of = NULL, nr, nb_starts = 0, ok = 0;
  pgcit_rec_t *pgcit = NULL;

  *out = NULL;
  if(!ifofile || !ifofile->vtsi_mat)
    return 0;
  ifop = PRIV(ifofile);
  t.ifop = ifop;
  sector = ifofile->vtsi_mat->vts_pgcit;
  if(sector == 0 || sector == 0xffffffff)
    return 1;                         /* no table */
  /* the information file's size by its header */
  ifo_size = ((int64_t)(ifofile->vtsi_mat->vtsi_last_sector & 0x1ffff) + 1) * DVD_BLOCK_LEN;
  t.pos    = (int64_t)sector * DVD_BLOCK_LEN;
  if(t.pos + DVD_BLOCK_LEN > ifo_size || !ifo_seek(ifop, (uint32_t)t.pos) ||
     !ifo_read(ifop, first, DVD_BLOCK_LEN)) {
    rec_report(&t, DVD_REPORT_ERROR, t.pos, "VTS_PGCIT at sector %u cannot be read (the information file has "
               "%" PRId64 " bytes)", sector, ifo_size);
    return 0;
  }
  len = ((uint32_t)first[4] << 24 | (uint32_t)first[5] << 16 | (uint32_t)first[6] << 8 | first[7]);
  len = (len & 0xfffff800) + DVD_BLOCK_LEN;
  if(len > 0x3fffffff || t.pos + len > ifo_size) {
    rec_report(&t, DVD_REPORT_ERROR, t.pos + 4, "VTS_PGCIT's %u bytes at sector %u lie past the information "
               "file's %" PRId64 " bytes: it cannot be read", len, sector, ifo_size);
    return 0;
  }
  if(!(data = malloc(len)))
    return 0;
  memcpy(data, first, len < DVD_BLOCK_LEN ? len : DVD_BLOCK_LEN);
  if(len > DVD_BLOCK_LEN && (!ifo_seek(ifop, (uint32_t)t.pos + DVD_BLOCK_LEN) ||
                             !ifo_read(ifop, data + DVD_BLOCK_LEN, len - DVD_BLOCK_LEN))) {
    rec_report(&t, DVD_REPORT_ERROR, t.pos + DVD_BLOCK_LEN, "VTS_PGCIT at sector %u cannot be read", sector);
    free(data);
    return 0;
  }
  t.p   = data;
  t.len = len;
  t.outside = -1;
  nr   = rec_W(&t, 0);
  last = t.len >= 8 ? ((uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7]) + 1 : 1;
  if(!(pgcit = calloc(1, sizeof(*pgcit))) ||
     (nr && (!(pgcit->pgcs = calloc(nr, sizeof(*pgcit->pgcs))) || !(pgcit->same_as = malloc(nr * sizeof(int))) ||
             !(starts = malloc(nr * sizeof(*starts))) || !(start_of = malloc(nr * sizeof(int))))))
    goto end;
  pgcit->nr_of_pgcs = nr;
  for(int i = 0; i < nr; i++) {
    uint32_t entry = rec_L(&t, 8 + 8 * i), off = rec_L(&t, 0x0c + 8 * i);
    pgcit->same_as[i] = -1;
    if((uint64_t)off + 4 >= last) {
      rec_report(&t, DVD_REPORT_WARN, t.pos + 0x0c + 8 * i, "program chain %d starts outside VTS_PGCIT (byte %u): "
                 "it is left out", i + 1, off);
      continue;
    }
    if(nr != 1) {
      int dup = -1;
      for(int k = 0; k < nb_starts && dup < 0; k++)
        if(starts[k] == off)
          dup = k;
      if(dup >= 0) {
        /* two search pointers to one program chain: the later one names the first */
        pgcit->same_as[i] = start_of[dup];
        Log2(ifop->ctx, "%s: VTS_PGCIT: program chain %d starts where program chain %d does: it is left out",
             ifop->name, i + 1, pgcit->same_as[i] + 1);
        continue;
      }
      start_of[nb_starts] = i;
      starts[nb_starts++] = off;
    }
    if(rec_pgc(&t, i, entry, off, &pgcit->pgcs[i]) < 0)
      goto end;
  }
  ok = 1;
end:
  free(starts);
  free(start_of);
  free(data);
  if(!ok) {
    ifoFree_VTS_PGCIT_rec(&pgcit);
    return 0;
  }
  *out = pgcit;
  return 1;
}

int ifoRead_PGCIT(ifo_handle_t *ifofile) {

  if(!ifofile)
    return 0;

  if(!ifofile->vtsi_mat)
    return 0;

  if(ifofile->vtsi_mat->vts_pgcit == 0) /* mandatory */
    return 0;

  ifofile->vts_pgcit = calloc(1, sizeof(pgcit_t));
  if(!ifofile->vts_pgcit)
    return 0;

  ifofile->vts_pgcit->ref_count = 1;
  if(!ifoRead_PGCIT_internal(ifofile, ifofile->vts_pgcit,
                             ifofile->vtsi_mat->vts_pgcit * DVD_BLOCK_LEN)) {
    free(ifofile->vts_pgcit);
    ifofile->vts_pgcit = NULL;
    return 0;
  }

  return 1;
}

static int find_dup_pgc(pgci_srp_t *pgci_srp, uint32_t start_byte, int count) {
  int i;

  for(i = 0; i < count; i++) {
    if(pgci_srp[i].pgc_start_byte == start_byte) {
      return i;
    }
  }
  return -1;
}

static int ifoRead_PGCIT_internal(ifo_handle_t *ifofile, pgcit_t *pgcit,
                                  unsigned int offset) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  int i, info_length;
  uint8_t *data, *ptr;

  if(!ifo_seek(ifop, offset))
    return 0;

  if(!(ifo_read(ifop, pgcit, PGCIT_SIZE)))
    return 0;

  B2N_16(pgcit->nr_of_pgci_srp);
  B2N_32(pgcit->last_byte);

  CHECK_ZERO(pgcit->zero_1);
  /* assert(pgcit->nr_of_pgci_srp != 0);
     Magic Knight Rayearth Daybreak is mastered very strange and has
     Titles with 0 PTTs. */
  CHECK_VALUE(pgcit->nr_of_pgci_srp < 10000); /* ?? seen max of 1338 */

  if (pgcit->nr_of_pgci_srp == 0) {
    pgcit->pgci_srp = NULL;
    return 1;
  }

  info_length = pgcit->nr_of_pgci_srp * PGCI_SRP_SIZE;
  data = calloc(1, info_length);
  if(!data)
    return 0;

  if(info_length && !(ifo_read(ifop, data, info_length))) {
    free(data);
    return 0;
  }

  pgcit->pgci_srp = calloc(pgcit->nr_of_pgci_srp, sizeof(pgci_srp_t));
  if(!pgcit->pgci_srp) {
    free(data);
    return 0;
  }
  ptr = data;
  for(i = 0; i < pgcit->nr_of_pgci_srp; i++) {
    memcpy(&pgcit->pgci_srp[i], ptr, PGCI_SRP_SIZE);
    ptr += PGCI_SRP_SIZE;
    read_pgci_srp(&pgcit->pgci_srp[i]);
    CHECK_VALUE(pgcit->pgci_srp[i].zero_1 == 0);
  }
  free(data);

  for(i = 0; i < pgcit->nr_of_pgci_srp; i++)
    CHECK_VALUE(pgcit->pgci_srp[i].pgc_start_byte + PGC_SIZE <= pgcit->last_byte+1);

  for(i = 0; i < pgcit->nr_of_pgci_srp; i++) {
    int dup;
    if((dup = find_dup_pgc(pgcit->pgci_srp, pgcit->pgci_srp[i].pgc_start_byte, i)) >= 0) {
      /* a duplicate of a PGC that could not be read is left out too */
      pgcit->pgci_srp[i].pgc = pgcit->pgci_srp[dup].pgc;
      if(pgcit->pgci_srp[i].pgc)
        pgcit->pgci_srp[i].pgc->ref_count++;
      else
        IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read invalid PCG (duplicate of PGC %d)", dup + 1);
      continue;
    }
    pgcit->pgci_srp[i].pgc = calloc(1, sizeof(pgc_t));
    if(!pgcit->pgci_srp[i].pgc) {
      int j;
      for(j = 0; j < i; j++) {
        ifoFree_PGC(pgcit->pgci_srp[j].pgc);
        pgcit->pgci_srp[j].pgc = NULL;
      }
      goto fail;
    }
    pgcit->pgci_srp[i].pgc->ref_count = 1;
    if(!ifoRead_PGC(ifofile, pgcit->pgci_srp[i].pgc,
                    offset + pgcit->pgci_srp[i].pgc_start_byte)) {
      IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read invalid PCG");
      //E-One releases provide bogus PGC, ie: out of bound start_byte
      free(pgcit->pgci_srp[i].pgc);
      pgcit->pgci_srp[i].pgc = NULL;
    }
  }

  return 1;
fail:
  free(pgcit->pgci_srp);
  pgcit->pgci_srp = NULL;
  return 0;
}

static void ifoFree_PGCIT_internal(pgcit_t *pgcit) {
  if(pgcit && (--pgcit->ref_count <= 0)) {
    int i;
    for(i = 0; i < pgcit->nr_of_pgci_srp; i++)
    {
      ifoFree_PGC(pgcit->pgci_srp[i].pgc);
      pgcit->pgci_srp[i].pgc = NULL;
    }
    free(pgcit->pgci_srp);
    free(pgcit);
  }
}

void ifoFree_PGCIT(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  if(ifofile->vts_pgcit) {
    ifoFree_PGCIT_internal(ifofile->vts_pgcit);
    ifofile->vts_pgcit = NULL;
  }
}

static int find_dup_lut(pgci_lu_t *lu, uint32_t start_byte, int count) {
  int i;

  for(i = 0; i < count; i++) {
    if(lu[i].lang_start_byte == start_byte) {
      return i;
    }
  }
  return -1;
}

int ifoRead_PGCI_UT(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  pgci_ut_t *pgci_ut;
  unsigned int sector;
  unsigned int i;
  int info_length;
  uint8_t *data, *ptr;

  if(!ifofile)
    return 0;

  if(ifofile->vmgi_mat) {
    if(ifofile->vmgi_mat->vmgm_pgci_ut == 0)
      return 1;
    sector = ifofile->vmgi_mat->vmgm_pgci_ut;
  } else if(ifofile->vtsi_mat) {
    if(ifofile->vtsi_mat->vtsm_pgci_ut == 0)
      return 1;
    sector = ifofile->vtsi_mat->vtsm_pgci_ut;
  } else {
    return 0;
  }

  ifofile->pgci_ut = calloc(1, sizeof(pgci_ut_t));
  if(!ifofile->pgci_ut)
    return 0;

  if(!ifo_seek(ifop, sector * DVD_BLOCK_LEN)) {
    free(ifofile->pgci_ut);
    ifofile->pgci_ut = NULL;
    return 0;
  }

  if(!(ifo_read(ifop, ifofile->pgci_ut, PGCI_UT_SIZE))) {
    free(ifofile->pgci_ut);
    ifofile->pgci_ut = NULL;
    return 0;
  }

  pgci_ut = ifofile->pgci_ut;

  B2N_16(pgci_ut->nr_of_lus);
  B2N_32(pgci_ut->last_byte);

  CHECK_ZERO(pgci_ut->zero_1);
  CHECK_VALUE(pgci_ut->nr_of_lus != 0);
  CHECK_VALUE(pgci_ut->nr_of_lus < 100); /* ?? 3-4 ? */
  CHECK_VALUE((uint32_t)pgci_ut->nr_of_lus * PGCI_LU_SIZE < pgci_ut->last_byte);

  info_length = pgci_ut->nr_of_lus * PGCI_LU_SIZE;
  data = calloc(1, info_length);
  if(!data) {
    free(pgci_ut);
    ifofile->pgci_ut = NULL;
    return 0;
  }
  if(!(ifo_read(ifop, data, info_length))) {
    free(data);
    free(pgci_ut);
    ifofile->pgci_ut = NULL;
    return 0;
  }

  pgci_ut->lu = calloc(pgci_ut->nr_of_lus, sizeof(pgci_lu_t));
  if(!pgci_ut->lu) {
    free(data);
    free(pgci_ut);
    ifofile->pgci_ut = NULL;
    return 0;
  }
  ptr = data;
  for(i = 0; i < pgci_ut->nr_of_lus; i++) {
    memcpy(&pgci_ut->lu[i], ptr, PGCI_LU_SIZE);
    ptr += PGCI_LU_SIZE;
    B2N_16(pgci_ut->lu[i].lang_code);
    B2N_32(pgci_ut->lu[i].lang_start_byte);
  }
  free(data);

  for(i = 0; i < pgci_ut->nr_of_lus; i++) {
    /* Maybe this is only defined for v1.1 and later titles? */
    /* If the bits in 'lu[i].exists' are enumerated abcd efgh then:
       VTS_x_yy.IFO        VIDEO_TS.IFO
       a == 0x83 "Root"         0x82 "Title"
       b == 0x84 "Subpicture"
       c == 0x85 "Audio"
       d == 0x86 "Angle"
       e == 0x87 "PTT"
    */
    CHECK_VALUE((pgci_ut->lu[i].exists & 0x07) == 0);
  }

  for(i = 0; i < pgci_ut->nr_of_lus; i++) {
    int dup;
    if((dup = find_dup_lut(pgci_ut->lu, pgci_ut->lu[i].lang_start_byte, i)) >= 0) {
      pgci_ut->lu[i].pgcit = pgci_ut->lu[dup].pgcit;
      pgci_ut->lu[i].pgcit->ref_count++;
      continue;
    }
    pgci_ut->lu[i].pgcit = calloc(1, sizeof(pgcit_t));
    if(!pgci_ut->lu[i].pgcit) {
      unsigned int j;
      for(j = 0; j < i; j++) {
        ifoFree_PGCIT_internal(pgci_ut->lu[j].pgcit);
        pgci_ut->lu[j].pgcit = NULL;
      }
      free(pgci_ut->lu);
      free(pgci_ut);
      ifofile->pgci_ut = NULL;
      return 0;
    }
    pgci_ut->lu[i].pgcit->ref_count = 1;
    if(!ifoRead_PGCIT_internal(ifofile, pgci_ut->lu[i].pgcit,
                               sector * DVD_BLOCK_LEN
                               + pgci_ut->lu[i].lang_start_byte)) {
      unsigned int j;
      for(j = 0; j <= i; j++) {
        ifoFree_PGCIT_internal(pgci_ut->lu[j].pgcit);
        pgci_ut->lu[j].pgcit = NULL;
      }
      free(pgci_ut->lu);
      free(pgci_ut);
      ifofile->pgci_ut = NULL;
      return 0;
    }
    /* FIXME: Iterate and verify that all menus that should exists accordingly
     * to pgci_ut->lu[i].exists really do? */
  }

  return 1;
}


void ifoFree_PGCI_UT(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  if(ifofile->pgci_ut) {
    unsigned int i;

    for(i = 0; i < ifofile->pgci_ut->nr_of_lus; i++) {
      ifoFree_PGCIT_internal(ifofile->pgci_ut->lu[i].pgcit);
      ifofile->pgci_ut->lu[i].pgcit = NULL;
    }
    free(ifofile->pgci_ut->lu);
    free(ifofile->pgci_ut);
    ifofile->pgci_ut = NULL;
  }
}

static int ifoRead_VTS_ATTRIBUTES(ifo_handle_t *ifofile,
                                  vts_attributes_t *vts_attributes,
                                  unsigned int offset) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  unsigned int i;

  if(!ifo_seek(ifop, offset))
    return 0;

  if(!(ifo_read(ifop, vts_attributes, sizeof(vts_attributes_t))))
    return 0;

  read_video_attr(&vts_attributes->vtsm_vobs_attr);
  read_video_attr(&vts_attributes->vtstt_vobs_video_attr);
  read_audio_attr(&vts_attributes->vtsm_audio_attr);
  for(i=0; i<8; i++)
    read_audio_attr(&vts_attributes->vtstt_audio_attr[i]);
  read_subp_attr(&vts_attributes->vtsm_subp_attr);
  for(i=0; i<32; i++)
    read_subp_attr(&vts_attributes->vtstt_subp_attr[i]);
  B2N_32(vts_attributes->last_byte);
  B2N_32(vts_attributes->vts_cat);

  CHECK_ZERO(vts_attributes->zero_1);
  CHECK_ZERO(vts_attributes->zero_2);
  CHECK_ZERO(vts_attributes->zero_3);
  CHECK_ZERO(vts_attributes->zero_4);
  CHECK_ZERO(vts_attributes->zero_5);
  CHECK_ZERO(vts_attributes->zero_6);
  CHECK_ZERO(vts_attributes->zero_7);
  CHECK_VALUE(vts_attributes->nr_of_vtsm_audio_streams <= 1);
  CHECK_VALUE(vts_attributes->nr_of_vtsm_subp_streams <= 1);
  CHECK_VALUE(vts_attributes->nr_of_vtstt_audio_streams <= 8);
  for(i = vts_attributes->nr_of_vtstt_audio_streams; i < 8; i++)
    CHECK_ZERO(vts_attributes->vtstt_audio_attr[i]);
  CHECK_VALUE(vts_attributes->nr_of_vtstt_subp_streams <= 32);
  {
    unsigned int nr_coded;
    CHECK_VALUE(vts_attributes->last_byte + 1 >= VTS_ATTRIBUTES_MIN_SIZE);
    nr_coded = (vts_attributes->last_byte + 1 - VTS_ATTRIBUTES_MIN_SIZE)/6;
    /* This is often nr_coded = 70, how do you know how many there really are? */
    if(nr_coded > 32) { /* We haven't read more from disk/file anyway */
      nr_coded = 32;
    }
    CHECK_VALUE(vts_attributes->nr_of_vtstt_subp_streams <= nr_coded);
    for(i = vts_attributes->nr_of_vtstt_subp_streams; i < nr_coded; i++)
      CHECK_ZERO(vts_attributes->vtstt_subp_attr[i]);
  }

  return 1;
}



int ifoRead_VTS_ATRT(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  vts_atrt_t *vts_atrt;
  unsigned int i, info_length, sector;
  uint32_t *data;

  if(!ifofile)
    return 0;

  if(!ifofile->vmgi_mat)
    return 0;

  if(ifofile->vmgi_mat->vts_atrt == 0) /* mandatory */
    return 0;

  sector = ifofile->vmgi_mat->vts_atrt;
  if(!ifo_seek(ifop, sector * DVD_BLOCK_LEN))
    return 0;

  vts_atrt = calloc(1, sizeof(vts_atrt_t));
  if(!vts_atrt)
    return 0;

  ifofile->vts_atrt = vts_atrt;

  if(!(ifo_read(ifop, vts_atrt, VTS_ATRT_SIZE))) {
    free(vts_atrt);
    ifofile->vts_atrt = NULL;
    return 0;
  }

  B2N_16(vts_atrt->nr_of_vtss);
  B2N_32(vts_atrt->last_byte);

  CHECK_ZERO(vts_atrt->zero_1);
  CHECK_VALUE(vts_atrt->nr_of_vtss != 0);
  CHECK_VALUE(vts_atrt->nr_of_vtss < 100); /* ?? */
  CHECK_VALUE((uint32_t)vts_atrt->nr_of_vtss * (4 + VTS_ATTRIBUTES_MIN_SIZE) +
              VTS_ATRT_SIZE < vts_atrt->last_byte + 1);

  info_length = vts_atrt->nr_of_vtss * sizeof(uint32_t);
  data = calloc(1, info_length);
  if(!data) {
    free(vts_atrt);
    ifofile->vts_atrt = NULL;
    return 0;
  }

  vts_atrt->vts_atrt_offsets = data;

  if(!(ifo_read(ifop, data, info_length))) {
    free(data);
    free(vts_atrt);
    ifofile->vts_atrt = NULL;
    return 0;
  }

  for(i = 0; i < vts_atrt->nr_of_vtss; i++) {
    B2N_32(data[i]);
    CHECK_VALUE(data[i] + VTS_ATTRIBUTES_MIN_SIZE < vts_atrt->last_byte + 1);
  }

  info_length = vts_atrt->nr_of_vtss * sizeof(vts_attributes_t);
  vts_atrt->vts = calloc(1, info_length);
  if(!vts_atrt->vts) {
    free(data);
    free(vts_atrt);
    ifofile->vts_atrt = NULL;
    return 0;
  }
  for(i = 0; i < vts_atrt->nr_of_vtss; i++) {
    unsigned int offset = data[i];
    if(!ifoRead_VTS_ATTRIBUTES(ifofile, &(vts_atrt->vts[i]),
                               (sector * DVD_BLOCK_LEN) + offset)) {
      free(data);
      free(vts_atrt);
      ifofile->vts_atrt = NULL;
      return 0;
    }

    /* This assert can't be in ifoRead_VTS_ATTRIBUTES */
    CHECK_VALUE(offset + vts_atrt->vts[i].last_byte <= vts_atrt->last_byte + 1);
    /* Is this check correct? */
  }

  return 1;
}


void ifoFree_VTS_ATRT(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  if(ifofile->vts_atrt) {
    free(ifofile->vts_atrt->vts);
    free(ifofile->vts_atrt->vts_atrt_offsets);
    free(ifofile->vts_atrt);
    ifofile->vts_atrt = NULL;
  }
}


int ifoRead_TXTDT_MGI(ifo_handle_t *ifofile) {
  struct ifo_handle_private_s *ifop = PRIV(ifofile);
  txtdt_mgi_t *txtdt_mgi;

  if(!ifofile)
    return 0;

  if(!ifofile->vmgi_mat)
    return 0;

  /* Return successfully if there is nothing to read. */
  if(ifofile->vmgi_mat->txtdt_mgi == 0)
    return 1;

  if(!ifo_seek(ifop,
                   ifofile->vmgi_mat->txtdt_mgi * DVD_BLOCK_LEN))
    return 0;

  txtdt_mgi = calloc(1, sizeof(txtdt_mgi_t));
  if(!txtdt_mgi) {
    return 0;
  }
  ifofile->txtdt_mgi = txtdt_mgi;

  if(!(ifo_read(ifop, txtdt_mgi, TXTDT_MGI_SIZE))) {
    IFO_REPORT(ifop, DVD_REPORT_ERROR, "Unable to read TXTDT_MGI.");
    free(txtdt_mgi);
    ifofile->txtdt_mgi = NULL;
    return 0;
  }

  /* Log1(ifop->ctx, "-- Not done yet --\n"); */
  return 1;
}

void ifoFree_TXTDT_MGI(ifo_handle_t *ifofile) {
  if(!ifofile)
    return;

  if(ifofile->txtdt_mgi) {
    free(ifofile->txtdt_mgi);
    ifofile->txtdt_mgi = NULL;
  }
}
