/*
 * The title timeline: closed captions to text. CCExtractor runs as a
 * helper program next to ours: CEA-608 caption blocks go in on its standard
 * input in its raw caption format (RCWT), SRT text comes out on its
 * standard output and is read back into cues.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "disctl.h"

extern char **environ;

/* The helper's options: raw caption input ("bin") on stdin, SRT in UTF-8 on
 * stdout, no input buffering, output flushed as it is written. */
static const char *const ccx_args[] = { "-in=bin", "-out=srt", "-stdin", "-stdout", "-utf8", "-nobi", "-ff" };

/* The helper's exit status when the data held no caption text (its
 * EXIT_NO_CAPTIONS): no cues, not a failure. */
#define CCX_EXIT_NO_CAPTIONS 10

/* The raw caption format's header: magic, creator, version, format
 * revision 1 (CEA-608 blocks). */
static const uint8_t rcwt_header[11] = { 0xCC, 0xCC, 0xED, 0xCC, 0x00, 0x50, 0x00, 0x01, 0x00, 0x00, 0x00 };

struct DRCcx {
    void      *log;
    pid_t      pid;
    int        in;                 /* our end of its standard input (a socket: no SIGPIPE) */
    int        out, err;           /* our ends of its standard output and error */
    pthread_t  reader;
    int        reader_running;
    AVBPrint   out_text, err_text; /* filled by the reader thread until it ends */
    int        started;            /* the header was written */
    int        failed;             /* a write failed: the helper is gone */
};

/* ---- the SRT the helper writes ---- */

static int digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

/* HH:MM:SS,mmm in milliseconds. */
static int64_t srt_time(const char *s)
{
    return ((int64_t)((s[0] - '0') * 10 + s[1] - '0') * 3600 + ((s[3] - '0') * 10 + s[4] - '0') * 60 +
            (s[6] - '0') * 10 + s[7] - '0') * 1000 + (s[9] - '0') * 100 + (s[10] - '0') * 10 + s[11] - '0';
}

/* A line of srt from *pos: its start and length without CR / LF; *pos moves
 * past it. 0 at the end. */
static int next_line(const char *srt, int size, int *pos, const char **line, int *len)
{
    int p = *pos, e;

    if (p >= size)
        return 0;
    for (e = p; e < size && srt[e] != '\n'; e++)
        ;
    *pos  = e < size ? e + 1 : e;
    *line = srt + p;
    *len  = e - p;
    if (*len && srt[e - 1] == '\r')
        (*len)--;
    return 1;
}

void ff_disctl_cues_free(DRCue **cues, int nb)
{
    for (int i = 0; *cues && i < nb; i++)
        av_free((*cues)[i].text);
    av_freep(cues);
}

int ff_disctl_srt_parse(void *logctx, const char *srt, int size, DRCue **cues, int *nb_cues)
{
    DRCue *c = NULL;
    int n = 0, cap = 0, pos = 0, len, ret = AVERROR_INVALIDDATA;
    const char *line;

    *cues    = NULL;
    *nb_cues = 0;
    if (size >= 3 && !memcmp(srt, "\xEF\xBB\xBF", 3))
        pos = 3;
    for (;;) {
        char num[16];
        AVBPrint text;

        /* empty lines between cues */
        do {
            if (!next_line(srt, size, &pos, &line, &len)) {
                *cues    = c;
                *nb_cues = n;
                return 0;
            }
        } while (!len);
        snprintf(num, sizeof(num), "%d", n + 1);
        if (len != (int)strlen(num) || memcmp(line, num, len)) {
            av_log(logctx, AV_LOG_ERROR, "Captions: the converter's SRT: cue %d: the number line is \"%.*s\"\n",
                   n + 1, FFMIN(len, 40), line);
            goto fail;
        }
        if (!next_line(srt, size, &pos, &line, &len) || len != 29 || memcmp(line + 12, " --> ", 5) ||
            line[2] != ':' || line[5] != ':' || line[19] != ':' || line[22] != ':' || line[8] != ',' ||
            line[25] != ',' || !digits(line, 2) || !digits(line + 3, 2) || !digits(line + 6, 2) ||
            !digits(line + 9, 3) || !digits(line + 17, 2) || !digits(line + 20, 2) || !digits(line + 23, 2) ||
            !digits(line + 26, 3)) {
            av_log(logctx, AV_LOG_ERROR, "Captions: the converter's SRT: cue %d: the timing line is \"%.*s\"\n",
                   n + 1, FFMIN(len, 40), line);
            goto fail;
        }
        if (n == cap) {
            int k = cap ? 2 * cap : 64;
            DRCue *g = av_realloc_array(c, k, sizeof(*c));
            if (!g) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            c   = g;
            cap = k;
        }
        c[n].start_ms = srt_time(line);
        c[n].end_ms   = srt_time(line + 17);
        c[n].text     = NULL;
        av_bprint_init(&text, 0, AV_BPRINT_SIZE_UNLIMITED);
        /* text lines up to an empty line (or the end) */
        while (next_line(srt, size, &pos, &line, &len) && len)
            av_bprintf(&text, "%s%.*s", text.len ? "\n" : "", len, line);
        if (av_bprint_finalize(&text, &c[n].text) < 0) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        n++;
    }
fail:
    ff_disctl_cues_free(&c, n);
    return ret;
}

/* ---- the helper process ---- */

const char *ff_disctl_ccx_default(char *buf, int size)
{
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    char *slash;

    if (n <= 0 || n >= size - 1)
        return NULL;
    buf[n] = 0;
    if (!(slash = strrchr(buf, '/')))
        return NULL;
    slash[1] = 0;
    if (av_strlcat(buf, DR_CCX_PROGRAM, size) >= size || access(buf, X_OK))
        return NULL;
    return buf;
}

/* Reads the helper's output and messages until both end (its own thread:
 * the helper writes while we write to it). */
static void *reader(void *opaque)
{
    DRCcx *c = opaque;
    struct pollfd p[2] = { { c->out, POLLIN, 0 }, { c->err, POLLIN, 0 } };
    AVBPrint *dst[2] = { &c->out_text, &c->err_text };
    int open = 2;
    char buf[4096];

    while (open) {
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int i = 0; i < 2; i++) {
            ssize_t n;
            if (p[i].fd < 0 || !(p[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            n = read(p[i].fd, buf, sizeof(buf));
            if (n > 0) {
                av_bprint_append_data(dst[i], buf, n);
            } else if (n == 0 || errno != EINTR) {
                p[i].fd = -1;
                open--;
            }
        }
    }
    return NULL;
}

int ff_disctl_ccx_start(DRCcx **cp, void *logctx, const char *program)
{
    DRCcx *c;
    int sv[2] = { -1, -1 }, po[2] = { -1, -1 }, pe[2] = { -1, -1 }, ret;
    posix_spawn_file_actions_t fa;
    char *argv[FF_ARRAY_ELEMS(ccx_args) + 2];

    *cp = NULL;
    if (!program || !*program)
        return AVERROR(EINVAL);
    if (!(c = av_mallocz(sizeof(*c))))
        return AVERROR(ENOMEM);
    c->log = logctx;
    c->in = c->out = c->err = -1;
    av_bprint_init(&c->out_text, 0, AV_BPRINT_SIZE_UNLIMITED);
    av_bprint_init(&c->err_text, 0, AV_BPRINT_SIZE_UNLIMITED);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || pipe(po) || pipe(pe)) {
        ret = AVERROR(errno);
        goto fail;
    }
    /* our ends stay out of the helper (and of any other child) */
    if (fcntl(sv[0], F_SETFD, FD_CLOEXEC) || fcntl(po[0], F_SETFD, FD_CLOEXEC) || fcntl(pe[0], F_SETFD, FD_CLOEXEC)) {
        ret = AVERROR(errno);
        goto fail;
    }
    argv[0] = (char *)program;
    for (int i = 0; i < FF_ARRAY_ELEMS(ccx_args); i++)
        argv[i + 1] = (char *)ccx_args[i];
    argv[FF_ARRAY_ELEMS(ccx_args) + 1] = NULL;
    if ((ret = posix_spawn_file_actions_init(&fa))) {
        ret = AVERROR(ret);
        goto fail;
    }
    if (!(ret = posix_spawn_file_actions_adddup2(&fa, sv[1], 0)) &&
        !(ret = posix_spawn_file_actions_adddup2(&fa, po[1], 1)) &&
        !(ret = posix_spawn_file_actions_adddup2(&fa, pe[1], 2)))
        ret = posix_spawn(&c->pid, program, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (ret) {
        av_log(logctx, AV_LOG_ERROR, "Captions: the converter %s cannot be started: %s\n", program, strerror(ret));
        ret = AVERROR(ret);
        goto fail;
    }
    close(sv[1]);
    close(po[1]);
    close(pe[1]);
    c->in  = sv[0];
    c->out = po[0];
    c->err = pe[0];
    if ((ret = pthread_create(&c->reader, NULL, reader, c))) {
        ret = AVERROR(ret);
        ff_disctl_ccx_close(&c);
        return ret;
    }
    c->reader_running = 1;
    av_log(logctx, AV_LOG_VERBOSE, "Captions: converter %s (CCExtractor %s) started\n", program, DR_CCX_VERSION);
    *cp = c;
    return 0;
fail:
    for (int i = 0; i < 2; i++) {
        if (sv[i] >= 0) close(sv[i]);
        if (po[i] >= 0) close(po[i]);
        if (pe[i] >= 0) close(pe[i]);
    }
    av_bprint_finalize(&c->out_text, NULL);
    av_bprint_finalize(&c->err_text, NULL);
    av_free(c);
    return ret;
}

static int send_all(DRCcx *c, const uint8_t *b, size_t n)
{
    while (n) {
        ssize_t w = send(c->in, b, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            c->failed = 1;
            av_log(c->log, AV_LOG_ERROR, "Captions: the converter does not take data: %s\n", strerror(errno));
            return AVERROR(errno);
        }
        b += w;
        n -= w;
    }
    return 0;
}

int ff_disctl_ccx_block(DRCcx *c, int64_t ms, const uint8_t *triplets, int count)
{
    uint8_t head[10];
    int ret;

    if (count <= 0)
        return 0;
    if (c->failed)
        return AVERROR(EPIPE);
    if (count > 0xffff || ms < 0)
        return AVERROR(EINVAL);
    if (!c->started) {
        if ((ret = send_all(c, rcwt_header, sizeof(rcwt_header))) < 0)
            return ret;
        c->started = 1;
    }
    for (int i = 0; i < 8; i++)
        head[i] = (uint64_t)ms >> (8 * i);
    head[8] = count;
    head[9] = count >> 8;
    if ((ret = send_all(c, head, sizeof(head))) < 0)
        return ret;
    return send_all(c, triplets, 3 * count);
}

/* Waits for the helper; its exit status, -1 when it cannot be known. */
static int wait_helper(DRCcx *c)
{
    int st;

    if (c->in >= 0) {
        close(c->in);
        c->in = -1;
    }
    if (c->reader_running) {
        pthread_join(c->reader, NULL);
        c->reader_running = 0;
    }
    if (c->pid <= 0)
        return -1;
    while (waitpid(c->pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    c->pid = 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int ff_disctl_ccx_finish(DRCcx *c, DRCue **cues, int *nb_cues)
{
    int st = wait_helper(c), ret;

    *cues    = NULL;
    *nb_cues = 0;
    if (!av_bprint_is_complete(&c->out_text) || !av_bprint_is_complete(&c->err_text))
        return AVERROR(ENOMEM);
    if (c->err_text.len)
        av_log(c->log, AV_LOG_DEBUG, "Captions: the converter said: %s\n", c->err_text.str);
    if (st == CCX_EXIT_NO_CAPTIONS) {
        av_log(c->log, AV_LOG_VERBOSE, "Captions: the caption data carries no caption text\n");
        return 0;
    }
    if (st) {
        av_log(c->log, AV_LOG_ERROR, "Captions: the converter failed (exit status %d): %.300s\n", st,
               c->err_text.len ? c->err_text.str : "no message");
        return AVERROR_EXTERNAL;
    }
    if (!c->started)
        return 0;
    if ((ret = ff_disctl_srt_parse(c->log, c->out_text.str, c->out_text.len, cues, nb_cues)) < 0)
        return ret;
    av_log(c->log, AV_LOG_VERBOSE, "Captions: the converter gave %d cue(s) (%u bytes of SRT)\n", *nb_cues,
           c->out_text.len);
    return 0;
}

void ff_disctl_ccx_close(DRCcx **cp)
{
    DRCcx *c = *cp;

    if (!c)
        return;
    if (c->pid > 0)
        kill(c->pid, SIGTERM);
    wait_helper(c);
    if (c->out >= 0)
        close(c->out);
    if (c->err >= 0)
        close(c->err);
    av_bprint_finalize(&c->out_text, NULL);
    av_bprint_finalize(&c->err_text, NULL);
    av_freep(cp);
}
