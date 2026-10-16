// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// dsr_engine.c — restartable DSR decode engine for the GTK frontend (dsrdec-gtk).
// Mirrors the dsrdec pipeline from decodermain.c (warm-up gate, DC blocker, slew
// limiter, file pacing, hard resync with cooldown, decoupled ALSA thread, HackRF
// acquisition watchdog), but keeps all state in engine_t and takes commands from
// another thread instead of the tty.
//
// Threads:
//   worker  — source read (+ demod) -> decoder/PA -> PCM ring / WAV; applies commands
//   libusb  — HackRF only: RX callback -> ring in hackrf_rx.c
//   ingress — UDP only: recv -> byte ring (keeps socket drained while worker decodes)
//   audio   — PCM ring -> ALSA (sole caller of alsa_write; blocking is local to it)

#define _POSIX_C_SOURCE 200809L

#include "dsr_engine.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "alsa.h"
#include "demod.h"
#include "pa.h"
#include "wave.h"

#define ENG_IN_RING_CAP        (512u * 1024u)   /* UDP ingress ring (power of 2) */
#define ENG_PCM_RING_CAP       (512u * 1024u)   /* decode -> audio thread (power of 2), ~4 s at 32 kHz */
#define ENG_WAIT_MS            5
#define ENG_STATUS_PERIOD_MS   100
#define ENG_IQ_CHUNK           65536u           /* IQ pairs per fread */
#define ENG_RAW_CHUNK          (64u * 1024u)
#define AUDIO_WARMUP_SEC       2.0
#define RESYNC_INGEST_PAUSE_MS 120
#define RESYNC_MIN_INTERVAL_MS 1000
#define ALSA_PERIOD_MS         16
#define ALSA_BUFFER_MS         128
#define MAX_UPSAMPLE           128
/* UDP jitter buffer: dsrtx paces its output to real time, but on its own clock, and the
 * network adds jitter. Hold this much audio in the PCM ring before starting playback,
 * and again after each xrun, so the 128 ms ALSA buffer does not run dry on a late burst. */
#define UDP_PREFILL_MS         500
/* HackRF delivers evenly (~6 ms USB blocks); the cushion only absorbs decode jitter and
 * the slow drift between HackRF and sound card clocks. */
#define HACKRF_PREFILL_MS      200
/* Receiver: no Williard + SA lock for this long (of signal) -> acquire again. */
#define ACQ_RETRY_SEC          3
#define DSR_AUDIO_RATE         32000            /* samples/s per channel in the DSR mux */

static const float   DC_ALPHA = 0.995f;
static const int16_t MAX_SLEW = 28000;

/* ------------------------------------------------------------------ time helpers */

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_ns(int64_t ns)
{
    if (ns <= 0) return;
    struct timespec ts = { .tv_sec = (time_t)(ns / 1000000000LL), .tv_nsec = (long)(ns % 1000000000LL) };
    nanosleep(&ts, NULL);
}

static void abs_realtime_in_ms(struct timespec *ts, int ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_nsec += (long)ms * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
}

/* ------------------------------------------------------------------ byte ring */

typedef struct {
    uint8_t        *buf;
    size_t          cap;        /* power of 2 */
    uint64_t        head, tail; /* pending = head - tail (under mu) */
    pthread_mutex_t mu;
    pthread_cond_t  cv;         /* data available */
    pthread_cond_t  cv_room;    /* space available */
    atomic_int     *stop;
} ring_t;

static int ring_init(ring_t *q, size_t cap, atomic_int *stop)
{
    memset(q, 0, sizeof(*q));
    q->buf = malloc(cap);
    if (!q->buf) return -1;
    q->cap = cap;
    q->stop = stop;
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    pthread_cond_init(&q->cv_room, NULL);
    return 0;
}

static void ring_destroy(ring_t *q)
{
    if (!q->buf) return;
    pthread_cond_destroy(&q->cv_room);
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mu);
    free(q->buf);
    q->buf = NULL;
}

static void ring_reset(ring_t *q)
{
    if (!q->buf) return;
    pthread_mutex_lock(&q->mu);
    q->head = q->tail = 0;
    pthread_cond_broadcast(&q->cv);
    pthread_cond_broadcast(&q->cv_room);
    pthread_mutex_unlock(&q->mu);
}

static void ring_wake(ring_t *q)
{
    if (!q->buf) return;
    pthread_mutex_lock(&q->mu);
    pthread_cond_broadcast(&q->cv);
    pthread_cond_broadcast(&q->cv_room);
    pthread_mutex_unlock(&q->mu);
}

/* Blocks until everything is queued (never drops — preserves bit sync). */
static void ring_push_blocking(ring_t *q, const uint8_t *p, size_t n)
{
    const uint64_t mask = (uint64_t)q->cap - 1u;
    size_t off = 0;
    while (off < n) {
        pthread_mutex_lock(&q->mu);
        while (q->head - q->tail >= (uint64_t)q->cap) {
            if (atomic_load(q->stop)) { pthread_mutex_unlock(&q->mu); return; }
            struct timespec ts;
            abs_realtime_in_ms(&ts, 100);
            pthread_cond_timedwait(&q->cv_room, &q->mu, &ts);
        }
        size_t free_sp = (size_t)((uint64_t)q->cap - (q->head - q->tail));
        size_t take = (n - off < free_sp) ? n - off : free_sp;
        for (size_t i = 0; i < take; i++) q->buf[(q->head + i) & mask] = p[off + i];
        q->head += take;
        off += take;
        pthread_cond_signal(&q->cv);
        pthread_mutex_unlock(&q->mu);
    }
}

/* Never blocks: on overflow evicts the oldest align-sized chunks. Returns bytes dropped. */
static uint64_t ring_push_dropping(ring_t *q, const uint8_t *p, size_t n, size_t align)
{
    const uint64_t mask = (uint64_t)q->cap - 1u;
    uint64_t dropped = 0;
    pthread_mutex_lock(&q->mu);
    uint64_t used = q->head - q->tail;
    uint64_t free_sp = (uint64_t)q->cap - used;
    if ((uint64_t)n > free_sp) {
        uint64_t need = (uint64_t)n - free_sp;
        if (align > 1) need = ((need + align - 1u) / align) * align;
        if (need > used) need = used;
        q->tail += need;
        dropped = need;
    }
    for (size_t i = 0; i < n; i++) q->buf[(q->head + i) & mask] = p[i];
    q->head += n;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return dropped;
}

static size_t ring_pop(ring_t *q, uint8_t *dst, size_t max)
{
    const uint64_t mask = (uint64_t)q->cap - 1u;
    pthread_mutex_lock(&q->mu);
    uint64_t avail = q->head - q->tail;
    size_t n = (avail < (uint64_t)max) ? (size_t)avail : max;
    for (size_t i = 0; i < n; i++) dst[i] = q->buf[(q->tail + i) & mask];
    q->tail += n;
    if (n) pthread_cond_broadcast(&q->cv_room);
    pthread_mutex_unlock(&q->mu);
    return n;
}

static size_t ring_used(ring_t *q)
{
    pthread_mutex_lock(&q->mu);
    size_t n = (size_t)(q->head - q->tail);
    pthread_mutex_unlock(&q->mu);
    return n;
}

static void ring_wait_data(ring_t *q, int timeout_ms)
{
    struct timespec ts;
    abs_realtime_in_ms(&ts, timeout_ms);
    pthread_mutex_lock(&q->mu);
    while (q->head == q->tail && !atomic_load(q->stop)) {
        if (pthread_cond_timedwait(&q->cv, &q->mu, &ts) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&q->mu);
}

/* ------------------------------------------------------------------ engine */

struct engine {
    engine_cfg_t cfg;
    atomic_int   stop;

    pthread_t worker, ingress, audio;
    bool      worker_started, ingress_started, audio_started;

    /* source */
    FILE     *fp;
    int64_t   file_size;
    int       udp_fd;
    demod_t  *dm;
    hackrf_rx_t *rx;
    uint64_t  rx_drops_seen;
    uint64_t  acq_samples;          /* track: IQ samples since last lock / acquisition start */
    unsigned  acq_retries;
    unsigned  pa_realigns_seen;
    bool      await_recovery;       /* a loss event is in the status bar: report the recovery */
    ring_t    in_ring;
    _Atomic uint64_t ingress_bytes;
    _Atomic int64_t  ingest_resume_ns;  /* >0: drop input until this monotonic time */

    decoder_t *dec;
    pa_t      *pa;
    wav_t      wav;
    bool       wav_on;

    /* audio */
    alsa_out_t *ao;
    ring_t      pcm_ring;
    atomic_int  audio_flush;
    _Atomic uint64_t audio_drop;
    atomic_bool muted;
    atomic_int  gain_q15;           /* live audio gain, 32768 = 1.0 */
    int16_t    *ao_buf;             /* audio thread only */
    size_t      ao_len, ao_cap, ao_period;
    size_t      ao_prefill_bytes;   /* 0 = start playback immediately (file input) */

    /* worker-only state */
    int      parity;
    int      sel_l, sel_r;
    bool     sel_stereo;
    bool     user_mode_set, autodetect_done;
    float    dc_l, dc_r;
    int16_t  last_l, last_r;
    int64_t  warm_t0;               /* 0 = not armed */
    bool     pace;
    int64_t  pace_t0;               /* 0 = not armed */
    uint64_t pace_frames;
    unsigned pace_pending;
    int64_t  last_resync;
    bool     resync_wait;
    uint64_t in_bytes, bits;
    int64_t  last_pub;
    dsr_log_level_t log_level;
    int16_t  outbuf[MAX_UPSAMPLE * 2];
    char     event[256];            /* last event text for status.msg */

    /* commands (GUI -> worker) */
    pthread_mutex_t cmd_mu;
    int  cmd_select_ch;             /* -1 = none */
    bool cmd_select_stereo;
    int  cmd_step;
    int  cmd_stereo;                /* -1 none, 0 mono, 1 stereo */
    bool cmd_resync;
    int  cmd_log;                   /* -1 none */

    /* status (worker -> GUI) */
    pthread_mutex_t st_mu;
    engine_status_t st;
};

static const char *pa_type_names[16] = {
    "None", "News", "Current Affairs", "Information", "Sport", "Education",
    "Drama", "Culture", "Science", "Varied Speech", "Pop Music", "Rock Music",
    "Easy Listening", "Light Classical", "Serious Classical", "Other Music"
};

const char *engine_pa_type_name(int type) { return pa_type_names[type & 0xF]; }

const char *engine_ch_name(int ch, char buf[4])
{
    ch &= 31;
    snprintf(buf, 4, "%d%c", ch / 2 + 1, (ch & 1) ? 'R' : 'L');
    return buf;
}

const char *engine_log_level_name(dsr_log_level_t lvl)
{
    switch (lvl) {
    case DSR_LOG_SYNC: return "sync";
    case DSR_LOG_WARN: return "warn";
    case DSR_LOG_ZI:   return "zi";
    default:           return "off";
    }
}

void engine_default_cfg(engine_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->src = ENGINE_SRC_IQ;
    c->iqrate = 20480000;
    hackrf_rx_default_cfg(&c->rx);
    snprintf(c->host, sizeof(c->host), "0.0.0.0");
    c->port = 9000;
    c->msb_first = true;
    c->descramble = true;
    c->auto_deint = true;
    c->stream_focus = true;
    c->log_level = DSR_LOG_NONE;
    c->rate = 32000;
    c->upsample = 1;
    c->audioout = true;
    c->realtime = true;
    c->start_ch = 0;
    c->start_stereo = true;
}

static void set_event(engine_t *e, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_event(engine_t *e, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->event, sizeof(e->event), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[engine] %s\n", e->event);
}

/* ---------------------------------------------------------------- audio output */

static void audio_request_flush(engine_t *e)
{
    if (!e->ao) return;
    ring_reset(&e->pcm_ring);
    atomic_store(&e->audio_flush, 1);
}

static void *audio_thread(void *arg)
{
    engine_t *e = arg;
    const size_t fb = 2 * sizeof(int16_t);
    bool priming = e->ao_prefill_bytes > 0;
    uint64_t xruns_seen = 0;
    for (;;) {
        if (atomic_exchange(&e->audio_flush, 0)) {
            e->ao_len = 0;
            priming = e->ao_prefill_bytes > 0;
        }
        if (priming) {
            if (ring_used(&e->pcm_ring) < e->ao_prefill_bytes) {
                if (atomic_load(&e->stop)) break;
                sleep_ns((int64_t)ENG_WAIT_MS * 1000000LL);
                continue;
            }
            priming = false;
        }
        size_t room = (e->ao_cap - e->ao_len) * fb;
        size_t n = ring_pop(&e->pcm_ring, (uint8_t *)(e->ao_buf + e->ao_len * 2), room);
        if (n == 0) {
            if (atomic_load(&e->stop)) break;
            ring_wait_data(&e->pcm_ring, ENG_WAIT_MS);
            continue;
        }
        e->ao_len += n / fb;
        if (e->ao_len >= e->ao_period) {
            alsa_write(e->ao, e->ao_buf, e->ao_len);
            e->ao_len = 0;
            uint64_t xr = alsa_get_xruns(e->ao);
            if (xr != xruns_seen) {       /* underrun: rebuild the full cushion once */
                xruns_seen = xr;
                if (e->ao_prefill_bytes > 0) priming = true;
            }
        }
    }
    return NULL;
}

static bool audio_open(engine_t *e)
{
    size_t pf = (size_t)((uint64_t)e->cfg.rate * ALSA_PERIOD_MS / 1000u);
    if (pf < 32) pf = 32;
    e->ao_period = pf;
    e->ao_cap = pf * 8;
    if (e->cfg.src == ENGINE_SRC_UDP)
        e->ao_prefill_bytes = (size_t)((uint64_t)e->cfg.rate * UDP_PREFILL_MS / 1000u) * 2 * sizeof(int16_t);
    else if (e->cfg.src == ENGINE_SRC_HACKRF)
        e->ao_prefill_bytes = (size_t)((uint64_t)e->cfg.rate * HACKRF_PREFILL_MS / 1000u) * 2 * sizeof(int16_t);
    e->ao_buf = malloc(e->ao_cap * 2 * sizeof(int16_t));
    if (!e->ao_buf) return false;
    e->ao = alsa_open(NULL, (unsigned)e->cfg.rate, 2, ALSA_PERIOD_MS, ALSA_BUFFER_MS, 0);
    if (!e->ao) goto fail;
    if (ring_init(&e->pcm_ring, ENG_PCM_RING_CAP, &e->stop) != 0) goto fail;
    if (pthread_create(&e->audio, NULL, audio_thread, e) != 0) goto fail;
    e->audio_started = true;
    return true;
fail:
    ring_destroy(&e->pcm_ring);
    if (e->ao) { alsa_close(e->ao); e->ao = NULL; }
    free(e->ao_buf); e->ao_buf = NULL;
    return false;
}

/* ---------------------------------------------------------------- worker helpers */

static void apply_selection(engine_t *e)
{
    if (e->cfg.stream_focus)
        decoder_set_stream_focus(e->dec, e->sel_l, e->sel_r, e->sel_stereo ? 0 : 1);
    audio_request_flush(e);
}

static void select_channel(engine_t *e, int ch, bool stereo)
{
    ch &= 31;
    e->sel_stereo = stereo;
    if (stereo) { e->sel_l = ch & ~1; e->sel_r = e->sel_l ^ 1; }
    else        { e->sel_l = e->sel_r = ch; }
    apply_selection(e);
}

static bool ingest_allowed(engine_t *e)
{
    int64_t until = atomic_load(&e->ingest_resume_ns);
    if (until == 0) return true;
    if (now_ns() >= until) { atomic_store(&e->ingest_resume_ns, 0); return true; }
    return false;
}

static void warmup_arm(engine_t *e)
{
    if (AUDIO_WARMUP_SEC > 0.0) e->warm_t0 = now_ns();
}

/* Minimum AUDIO_WARMUP_SEC mute, then stay muted until Williard, deint and PA SA
 * cycle/offset are all locked (same gate as dsrdec). */
static bool in_warmup(engine_t *e)
{
    if (AUDIO_WARMUP_SEC <= 0.0 || e->warm_t0 == 0) return false;
    if ((double)(now_ns() - e->warm_t0) * 1e-9 < AUDIO_WARMUP_SEC) return true;
    if (!decoder_is_williard_locked(e->dec)) return true;
    if (!decoder_is_deint_ready(e->dec)) return true;
    if (!(pa_sa_cycle_locked(e->pa) && pa_is_sa_offset_ready(e->pa))) return true;
    return false;
}

static void pace_reset(engine_t *e)
{
    e->pace_t0 = 0;
    e->pace_frames = 0;
    e->pace_pending = 0;
}

/* Throttle file input to real time (see file_pace_tick in decodermain.c). */
static void pace_tick(engine_t *e, unsigned frames)
{
    if (e->pace_t0 == 0) { e->pace_t0 = now_ns(); e->pace_frames = frames; return; }
    e->pace_frames += frames;
    double expected = (double)e->pace_frames / (double)e->cfg.rate;
    double actual = (double)(now_ns() - e->pace_t0) * 1e-9;
    double ahead = expected - actual;
    if (ahead <= 0.0) return;
    if (ahead > 0.5) ahead = 0.5;
    sleep_ns((int64_t)(ahead * 1e9));
}

static inline int16_t slew_limit(int16_t v, int16_t last)
{
    int32_t d = (int32_t)v - (int32_t)last;
    if (d > MAX_SLEW) {
        int32_t x = (int32_t)last + MAX_SLEW;
        return (int16_t)(x > 32767 ? 32767 : x);
    }
    if (d < -MAX_SLEW) {
        int32_t x = (int32_t)last - MAX_SLEW;
        return (int16_t)(x < -32768 ? -32768 : x);
    }
    return v;
}

static inline int16_t clamp16f(float v)
{
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t)v;
}

/* One decoded A/B pair -> selected L/R -> DC blocker, slew limiter -> WAV / PCM ring. */
static void output_pair(engine_t *e, const int16_t pcmA[16], const int16_t pcmB[16])
{
    if (e->pace) {
        e->pace_pending += (unsigned)e->cfg.upsample;
        if (e->pace_pending >= 512) { pace_tick(e, e->pace_pending); e->pace_pending = 0; }
    }

    int16_t l_raw = (e->sel_l < 16) ? pcmA[e->sel_l] : pcmB[e->sel_l - 16];
    int16_t r_raw = !e->sel_stereo ? l_raw : ((e->sel_r < 16) ? pcmA[e->sel_r] : pcmB[e->sel_r - 16]);

    e->dc_l = DC_ALPHA * e->dc_l + (1.0f - DC_ALPHA) * (float)l_raw;
    e->dc_r = DC_ALPHA * e->dc_r + (1.0f - DC_ALPHA) * (float)r_raw;
    int16_t l = slew_limit(clamp16f((float)l_raw - e->dc_l), e->last_l);
    int16_t r = slew_limit(clamp16f((float)r_raw - e->dc_r), e->last_r);
    e->last_l = l;
    e->last_r = r;

    if (in_warmup(e)) return;

    int up = e->cfg.upsample;
    for (int i = 0; i < up; i++) { e->outbuf[2 * i] = l; e->outbuf[2 * i + 1] = r; }

    if (e->wav_on) wav_write_frames(&e->wav, e->outbuf, (size_t)up);
    if (e->ao) {
        int32_t gain = atomic_load(&e->gain_q15);
        if (atomic_load(&e->muted) || gain <= 0) {
            memset(e->outbuf, 0, (size_t)up * 2 * sizeof(int16_t));
        } else if (gain < 32768) {
            for (int i = 0; i < up * 2; i++)
                e->outbuf[i] = (int16_t)(((int32_t)e->outbuf[i] * gain) >> 15);
        }
        uint64_t d = ring_push_dropping(&e->pcm_ring, (const uint8_t *)e->outbuf,
                                        (size_t)up * 2 * sizeof(int16_t), 2 * sizeof(int16_t));
        if (d) atomic_fetch_add(&e->audio_drop, d);
    }
}

static size_t flush_udp_backlog(int fd)
{
    if (fd < 0) return 0;
    uint8_t tmp[2048];
    size_t dropped = 0;
    for (int i = 0; i < 4096; i++) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n <= 0) break;
        dropped += (size_t)n;
    }
    return dropped;
}

static void hard_resync(engine_t *e)
{
    if (e->resync_wait) { set_event(e, "Resync ignored (waiting for lock recovery)"); return; }
    int64_t now = now_ns();
    if (e->last_resync && (now - e->last_resync) < (int64_t)RESYNC_MIN_INTERVAL_MS * 1000000LL) {
        set_event(e, "Resync ignored (cooldown)");
        return;
    }
    if (atomic_load(&e->ingest_resume_ns) != 0) { set_event(e, "Resync already in progress"); return; }

    e->last_resync = now;
    size_t dropped = flush_udp_backlog(e->udp_fd);
    if (e->in_ring.buf) ring_reset(&e->in_ring);
    if (e->rx) hackrf_rx_flush(e->rx);
    if (e->dm && e->cfg.iq_track) demod_reset(e->dm);
    e->acq_samples = 0;
    decoder_reset_sync(e->dec);
    pa_reset(e->pa);
    e->parity = 0;
    e->dc_l = e->dc_r = 0.0f;
    e->last_l = e->last_r = 0;
    audio_request_flush(e);
    e->resync_wait = true;
    atomic_store(&e->ingest_resume_ns, now + (int64_t)RESYNC_INGEST_PAUSE_MS * 1000000LL);
    warmup_arm(e);
    if (dropped) set_event(e, "Resync: state cleared, dropped %zu queued UDP bytes", dropped);
    else         set_event(e, "Resync: state cleared, cold sync");
}

/* Once after first lock: pick stereo/mono for the current pair from PA (like dsrdec). */
static void auto_detect_mode(engine_t *e)
{
    int base = e->sel_l & ~1;
    int tL = 0, mL = 0, modeL = 0, tR = 0, mR = 0, modeR = 0;
    bool vL = pa_get_pa(e->pa, base, &tL, &mL, &modeL);
    bool vR = pa_get_pa(e->pa, base ^ 1, &tR, &mR, &modeR);
    if (!vL && !vR) return;
    bool stereo = vL && vR && modeL == 1 && modeR == 2;
    if (stereo != e->sel_stereo) {
        select_channel(e, base, stereo);
        char nb[4];
        set_event(e, "Auto: %s detected on ch %s", stereo ? "stereo pair" : "mono channel", engine_ch_name(base, nb));
    }
}

static void poll_commands(engine_t *e)
{
    pthread_mutex_lock(&e->cmd_mu);
    int sel_ch = e->cmd_select_ch;  bool sel_st = e->cmd_select_stereo;
    int step = e->cmd_step;         int st = e->cmd_stereo;
    bool rs = e->cmd_resync;        int lg = e->cmd_log;
    e->cmd_select_ch = -1; e->cmd_step = 0; e->cmd_stereo = -1; e->cmd_resync = false; e->cmd_log = -1;
    pthread_mutex_unlock(&e->cmd_mu);

    if (sel_ch >= 0) {
        e->user_mode_set = true;
        select_channel(e, sel_ch, sel_st);
    }
    if (step) {
        if (e->sel_stereo) select_channel(e, (e->sel_l & ~1) + 2 * step + 32, true);
        else               select_channel(e, e->sel_l + step + 32, false);
    }
    if (st >= 0) {
        e->user_mode_set = true;
        select_channel(e, e->sel_l, st == 1);
    }
    if (rs) hard_resync(e);
    if (lg >= 0) {
        e->log_level = (dsr_log_level_t)lg;
        decoder_set_log_level(e->dec, e->log_level);
    }
}

static void publish_status(engine_t *e, bool force)
{
    int64_t now = now_ns();
    if (!force && now - e->last_pub < (int64_t)ENG_STATUS_PERIOD_MS * 1000000LL) return;
    e->last_pub = now;

    bool will = decoder_is_williard_locked(e->dec);
    bool sa = pa_sa_cycle_locked(e->pa);
    bool off = pa_is_sa_offset_ready(e->pa);

    if (e->resync_wait && will && sa && off) {
        e->resync_wait = false;
        e->await_recovery = false;
        set_event(e, "Resync: lock recovered");
    }
    if (!e->autodetect_done && will && sa) {
        e->autodetect_done = true;
        if (!e->user_mode_set) auto_detect_mode(e);
    }

    engine_status_t s;
    pthread_mutex_lock(&e->st_mu);
    s = e->st;
    pthread_mutex_unlock(&e->st_mu);

    s.src = e->cfg.src;
    s.in_bytes = (e->cfg.src == ENGINE_SRC_UDP) ? atomic_load(&e->ingress_bytes) : e->in_bytes;
    s.rf_valid = e->dm && e->cfg.iq_track;
    if (s.rf_valid) {
        demod_track_info_t ti;
        demod_get_track_info(e->dm, &ti);
        s.rf_level_dbfs = ti.level_dbfs;
        s.rf_cfo_hz = ti.cfo_hz;
        s.rf_clock_ppm = ti.clock_ppm;
        s.rf_evm = ti.evm;
        s.rf_acq_retries = e->acq_retries;
    }
    if (e->rx) {
        hackrf_rx_get_gains(e->rx, &s.rf_lna, &s.rf_vga, &s.rf_amp);
        s.rf_freq_hz = e->cfg.rx.freq_hz;
        s.rf_drop_bytes = hackrf_rx_dropped_bytes(e->rx);
    }
    s.bits = e->bits;
    s.progress = -1.0;
    if (e->fp && e->file_size > 0) {
        off_t pos = ftello(e->fp);
        if (pos >= 0) s.progress = (double)pos / (double)e->file_size;
    }
    decoder_get_stats(e->dec, &s.bch_total, &s.bch_bad);
    s.bch_corr = decoder_get_bch_corrected(e->dec);
    s.sf = decoder_get_superframe_index(e->dec);
    s.sf_err = decoder_get_superframe_err_count(e->dec);
    s.will_locked = will;
    s.deint_ready = decoder_is_deint_ready(e->dec);
    s.sa_locked = sa;
    s.sa_offset_ready = off;
    pa_status_t ps;
    pa_get_status(e->pa, &ps);
    s.pa_aligned = ps.aligned;
    s.pa_names = ps.names;
    s.pa_attrs = ps.attrs;
    s.pa_realigns = ps.realigns;
    if (ps.realigns != e->pa_realigns_seen) {
        e->pa_realigns_seen = ps.realigns;
        e->await_recovery = true;
        set_event(e, "SA alignment lost - station list kept, realigning");
    }
    /* Loss messages (realign, mux search, USB drops) would otherwise stay in the status
     * bar after reception is back: replace them once everything is locked again. */
    if (e->await_recovery && will && sa && off && ps.aligned) {
        e->await_recovery = false;
        if (ps.names) set_event(e, "Reception restored - mux locked, station list confirmed");
        else          set_event(e, "DSR mux locked");
    }
    s.warmup = in_warmup(e);
    s.resync_wait = e->resync_wait;
    s.audio_on = e->ao != NULL;
    s.wav_on = e->wav_on;
    s.muted = atomic_load(&e->muted);
    s.xruns = e->ao ? alsa_get_xruns(e->ao) : 0;
    s.audio_drop_bytes = atomic_load(&e->audio_drop);
    s.sel_l = e->sel_l;
    s.sel_r = e->sel_r;
    s.sel_stereo = e->sel_stereo;
    s.log_level = e->log_level;
    for (int ch = 0; ch < ENGINE_NUM_CH; ch++) {
        engine_chan_t *c = &s.ch[ch];
        memset(c, 0, sizeof(*c));
        int n = pa_get_name_utf8(e->pa, ch, c->name, (int)sizeof(c->name));
        while (n > 0 && c->name[n - 1] == ' ') c->name[--n] = 0;  /* labels are space-padded to 8 */
        c->valid = pa_get_pa(e->pa, ch, &c->type, &c->music, &c->mode) || c->name[0];
    }
    if (e->event[0]) snprintf(s.msg, sizeof(s.msg), "%s", e->event);

    pthread_mutex_lock(&e->st_mu);
    if (e->st.state != ENGINE_RUNNING) {  /* keep terminal state + its message */
        s.state = e->st.state;
        memcpy(s.msg, e->st.msg, sizeof(s.msg));
    }
    e->st = s;
    pthread_mutex_unlock(&e->st_mu);
}

/* Sets the terminal state; callable from any engine thread (worker publishes on exit). */
static void finish(engine_t *e, engine_state_t state, const char *msg)
{
    pthread_mutex_lock(&e->st_mu);
    if (e->st.state == ENGINE_RUNNING) {
        e->st.state = state;
        snprintf(e->st.msg, sizeof(e->st.msg), "%s", msg);
    }
    pthread_mutex_unlock(&e->st_mu);
    fprintf(stderr, "[engine] %s\n", msg);
}

/* file EOF: rewind if repeat, else finish. Returns true to continue. */
static bool handle_eof(engine_t *e)
{
    if (!e->cfg.repeat) {
        finish(e, ENGINE_FINISHED, "End of file");
        return false;
    }
    clearerr(e->fp);
    rewind(e->fp);
    if (e->dm) demod_reset(e->dm);
    pa_reset(e->pa);
    e->in_bytes = 0;
    e->bits = 0;
    pace_reset(e);
    warmup_arm(e);
    set_event(e, "End of file: rewind (repeat)");
    return true;
}

/* ---------------------------------------------------------------- worker loops */

/* Receiver acquisition (track): with no Williard + SA lock for ACQ_RETRY_SEC of signal,
 * start over — the auto-map may have been decided on noise, a level transient or before
 * the loops settled. Williard alone is no proof: on garbage it locks on a chance 16-bit
 * match and drops out 24 checks later. */
static void acquisition_watchdog(engine_t *e)
{
    if (decoder_is_williard_locked(e->dec) && pa_sa_cycle_locked(e->pa)) {
        e->acq_samples = 0;
        return;
    }
    if (e->acq_samples <= (uint64_t)ACQ_RETRY_SEC * e->cfg.iqrate) return;
    e->acq_samples = 0;
    e->acq_retries++;
    if (e->rx) hackrf_rx_flush(e->rx);
    demod_reset(e->dm);
    decoder_reset_sync(e->dec);
    pa_reset(e->pa);
    e->parity = 0;
    audio_request_flush(e);
    warmup_arm(e);
    demod_track_info_t ti;
    demod_get_track_info(e->dm, &ti);
    e->await_recovery = true;
    if (e->rx)
        set_event(e, "No DSR mux at %.3f MHz (level %.0f dBFS) - searching again",
                  (double)e->cfg.rx.freq_hz / 1e6, ti.level_dbfs);
    else
        set_event(e, "No DSR mux locked - searching again");
}

/* HackRF: one read; returns IQ pairs (0 = nothing yet), or (size_t)-1 to stop. */
static size_t hackrf_read(engine_t *e, int8_t *iqbuf)
{
    size_t got = hackrf_rx_read(e->rx, iqbuf, ENG_IQ_CHUNK, 50);
    if (got == (size_t)-1) {
        finish(e, ENGINE_FAILED, "HackRF stopped streaming (unplugged?)");
        return got;
    }
    demod_track_info_t ti;
    demod_get_track_info(e->dm, &ti);
    if (hackrf_rx_auto_gain(e->rx, ti.level_dbfs)) {
        unsigned lna, vga; bool amp;
        hackrf_rx_get_gains(e->rx, &lna, &vga, &amp);
        set_event(e, "Auto gain: level %.0f dBFS -> LNA %u dB, VGA %u dB", ti.level_dbfs, lna, vga);
    }
    /* A USB overflow cuts a hole into the bitstream; PA would keep a shifted SA assembly
     * (garbled labels), so rebuild everything like a manual resync. */
    uint64_t drops = hackrf_rx_dropped_bytes(e->rx);
    if (drops != e->rx_drops_seen) {
        e->rx_drops_seen = drops;
        e->last_resync = 0;   /* not subject to the manual cooldown/relock wait */
        e->resync_wait = false;
        hard_resync(e);
        e->resync_wait = false;     /* reported below via await_recovery instead */
        e->await_recovery = true;
        set_event(e, "HackRF samples dropped (USB/CPU too slow) - resync");
        return 0;
    }
    return got;
}

static void run_iq(engine_t *e)
{
    size_t item = e->cfg.iq_fmt == 1 ? sizeof(float) : e->cfg.iq_fmt == 2 ? sizeof(int8_t) : sizeof(int16_t);
    /* track: fs may be below 2*sym (HackRF at 20 MS/s) -> slightly more bits than samples */
    const size_t bitcap = 2 * ENG_IQ_CHUNK;
    void *iqbuf = malloc(ENG_IQ_CHUNK * 2 * item);
    uint8_t *bitbuf = malloc(bitcap);
    if (!iqbuf || !bitbuf) {
        free(iqbuf); free(bitbuf);
        finish(e, ENGINE_FAILED, "Out of memory (IQ buffers)");
        return;
    }
    while (!atomic_load(&e->stop)) {
        poll_commands(e);
        if (e->cfg.iq_track) acquisition_watchdog(e);
        size_t got;
        if (e->rx) {
            got = hackrf_read(e, iqbuf);
            if (got == (size_t)-1) break;
            if (got == 0) { publish_status(e, false); continue; }
        } else {
            got = fread(iqbuf, 2 * item, ENG_IQ_CHUNK, e->fp);
            if (got == 0) {
                if (ferror(e->fp)) { finish(e, ENGINE_FAILED, "Read error on IQ file"); break; }
                if (!handle_eof(e)) break;
                continue;
            }
        }
        e->in_bytes += got * 2 * item;
        e->acq_samples += got;
        size_t nbits = demod_process(e->dm, iqbuf, got, bitbuf, bitcap);
        if (ingest_allowed(e)) {
            int16_t pcmA[16], pcmB[16];
            size_t k = 0;
            while (k < nbits) {
                bool emitted = false;
                k += decoder_push_ab_bits(e->dec, bitbuf + k, nbits - k, &e->parity,
                                          pcmA, pcmB, &emitted);
                if (emitted) output_pair(e, pcmA, pcmB);
            }
            e->bits += nbits;
        }
        publish_status(e, false);
    }
    free(iqbuf);
    free(bitbuf);
}

static void run_raw(engine_t *e)
{
    static uint8_t buf[ENG_RAW_CHUNK]; /* one engine at a time */
    while (!atomic_load(&e->stop)) {
        poll_commands(e);
        size_t n = fread(buf, 1, sizeof(buf), e->fp);
        if (n == 0) {
            if (ferror(e->fp)) { finish(e, ENGINE_FAILED, "Read error on raw file"); break; }
            if (!handle_eof(e)) break;
            continue;
        }
        e->in_bytes += n;
        if (ingest_allowed(e)) {
            for (size_t i = 0; i < n; i++) {
                int16_t pcmA[16], pcmB[16];
                if (decoder_push_ab_u8(e->dec, buf[i], e->cfg.msb_first, &e->parity, pcmA, pcmB))
                    output_pair(e, pcmA, pcmB);
            }
            e->bits += (uint64_t)n * 8u;
        }
        publish_status(e, false);
    }
}

static void *udp_ingress_thread(void *arg)
{
    engine_t *e = arg;
    static uint8_t scratch[64 * 1024u]; /* one engine at a time */
    while (!atomic_load(&e->stop)) {
        ssize_t n = recv(e->udp_fd, scratch, sizeof(scratch), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                sleep_ns(1000000LL);
                continue;
            }
            char msg[128];
            snprintf(msg, sizeof(msg), "UDP receive failed: %s", strerror(errno));
            finish(e, ENGINE_FAILED, msg);
            atomic_store(&e->stop, 1);
            ring_wake(&e->in_ring);
            break;
        }
        if (n == 0) continue;
        atomic_fetch_add(&e->ingress_bytes, (uint64_t)n);
        if (ingest_allowed(e)) ring_push_blocking(&e->in_ring, scratch, (size_t)n);
    }
    return NULL;
}

static void run_udp(engine_t *e)
{
    uint8_t batch[8 * 1024u];
    while (!atomic_load(&e->stop)) {
        poll_commands(e);
        size_t n = ring_pop(&e->in_ring, batch, sizeof(batch));
        if (n == 0) {
            ring_wait_data(&e->in_ring, ENG_WAIT_MS);
            publish_status(e, false);
            continue;
        }
        if (ingest_allowed(e)) {
            for (size_t i = 0; i < n; i++) {
                int16_t pcmA[16], pcmB[16];
                if (decoder_push_ab_u8(e->dec, batch[i], e->cfg.msb_first, &e->parity, pcmA, pcmB))
                    output_pair(e, pcmA, pcmB);
            }
            e->bits += (uint64_t)n * 8u;
        }
        publish_status(e, false);
    }
}

static void *worker_thread(void *arg)
{
    engine_t *e = arg;
    warmup_arm(e);
    switch (e->cfg.src) {
    case ENGINE_SRC_IQ:
    case ENGINE_SRC_HACKRF: run_iq(e); break;
    case ENGINE_SRC_RAW: run_raw(e); break;
    case ENGINE_SRC_UDP: run_udp(e); break;
    }
    publish_status(e, true);
    return NULL;
}

/* ---------------------------------------------------------------- lifecycle */

static int udp_bind(const char *host, int port, char *err, size_t errlen)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { snprintf(err, errlen, "socket: %s", strerror(errno)); return -1; }
    int sz = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        snprintf(err, errlen, "Invalid host address: %s", host);
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        snprintf(err, errlen, "bind %s:%d: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

static void engine_free_all(engine_t *e)
{
    atomic_store(&e->stop, 1);
    if (e->in_ring.buf) ring_wake(&e->in_ring);
    if (e->ingress_started) pthread_join(e->ingress, NULL);
    if (e->worker_started)  pthread_join(e->worker, NULL);
    if (e->audio_started) {
        ring_reset(&e->pcm_ring);  /* drop queued audio so the thread exits promptly */
        pthread_join(e->audio, NULL);
    }
    if (e->ao) { alsa_stream_reset(e->ao); alsa_close(e->ao); }
    ring_destroy(&e->pcm_ring);
    free(e->ao_buf);
    ring_destroy(&e->in_ring);
    if (e->wav_on) wav_close(&e->wav);
    if (e->rx) hackrf_rx_close(e->rx);
    if (e->dm) demod_free(e->dm);
    if (e->fp) fclose(e->fp);
    if (e->udp_fd >= 0) close(e->udp_fd);
    if (e->pa) pa_free(e->pa);
    if (e->dec) decoder_free(e->dec);
    pthread_mutex_destroy(&e->cmd_mu);
    pthread_mutex_destroy(&e->st_mu);
    free(e);
}

engine_t *engine_start(const engine_cfg_t *cfg, char *err, size_t errlen)
{
    char dummy[8];
    if (!err) { err = dummy; errlen = sizeof(dummy); }
    err[0] = 0;

    engine_t *e = calloc(1, sizeof(*e));
    if (!e) { snprintf(err, errlen, "Out of memory"); return NULL; }
    e->cfg = *cfg;
    if (e->cfg.upsample < 1) e->cfg.upsample = 1;
    if (e->cfg.upsample > MAX_UPSAMPLE) e->cfg.upsample = MAX_UPSAMPLE;
    if (e->cfg.rate <= 0) e->cfg.rate = 32000;
    e->udp_fd = -1;
    e->log_level = cfg->log_level;
    e->cmd_select_ch = -1;
    e->cmd_stereo = -1;
    e->cmd_log = -1;
    pthread_mutex_init(&e->cmd_mu, NULL);
    pthread_mutex_init(&e->st_mu, NULL);
    e->st.state = ENGINE_RUNNING;
    atomic_store(&e->gain_q15, 32768);

    /* source */
    if (cfg->src == ENGINE_SRC_UDP) {
        e->udp_fd = udp_bind(cfg->host, cfg->port, err, errlen);
        if (e->udp_fd < 0) goto fail;
        if (ring_init(&e->in_ring, ENG_IN_RING_CAP, &e->stop) != 0) {
            snprintf(err, errlen, "Out of memory (ingress ring)");
            goto fail;
        }
    } else if (cfg->src == ENGINE_SRC_HACKRF) {
        e->cfg.iqrate = e->cfg.rx.sample_rate ? e->cfg.rx.sample_rate : HACKRF_RX_DEFAULT_RATE;
        e->cfg.rx.sample_rate = e->cfg.iqrate;
        e->cfg.iq_fmt = 2;
        e->cfg.iq_track = true;
    } else {
        if (!cfg->path[0]) { snprintf(err, errlen, "No input file selected"); goto fail; }
        e->fp = fopen(cfg->path, "rb");
        if (!e->fp) { snprintf(err, errlen, "%s: %s", cfg->path, strerror(errno)); goto fail; }
        struct stat sb;
        if (fstat(fileno(e->fp), &sb) == 0 && S_ISREG(sb.st_mode)) e->file_size = (int64_t)sb.st_size;
    }
    if (cfg->src == ENGINE_SRC_IQ || cfg->src == ENGINE_SRC_HACKRF) {
        demod_cfg_t dc = {
            .fs_hz = e->cfg.iqrate,
            .sym_hz = e->cfg.iq_track ? DSR_SYMBOL_RATE_HZ : e->cfg.iqrate / 2,
            .rrc_beta = 0.50f, .rrc_span = 8,
            .fmt = (demod_iqfmt_t)e->cfg.iq_fmt,
            .agc_ref = 0.7f, .agc_tau = 0.05f,
            .track = e->cfg.iq_track ? 1 : 0,
        };
        e->dm = demod_create(&dc);
        if (!e->dm) { snprintf(err, errlen, "Demodulator init failed"); goto fail; }
    }
    /* Opened last among the inputs: from here on the device streams into its ring. */
    if (cfg->src == ENGINE_SRC_HACKRF) {
        e->rx = hackrf_rx_open(&e->cfg.rx, err, errlen);
        if (!e->rx) goto fail;
    }

    decoder_opts_t dopts = {
        .descramble = cfg->descramble,
        .bch_check  = cfg->bch_check,
        .log_level  = cfg->log_level,
        .auto_deint = cfg->auto_deint,
    };
    e->dec = decoder_new(&dopts);
    e->pa = pa_new();
    if (!e->dec || !e->pa) { snprintf(err, errlen, "Decoder init failed"); goto fail; }
    decoder_attach_pa(e->dec, e->pa);

    if (cfg->wav[0]) {
        if (wav_open(&e->wav, cfg->wav, 2, e->cfg.rate) < 0) {
            snprintf(err, errlen, "Cannot open WAV file %s", cfg->wav);
            goto fail;
        }
        e->wav_on = true;
    }
    if (cfg->audioout && !audio_open(e))
        set_event(e, "ALSA open failed — running without audio");

    e->pace = (cfg->src == ENGINE_SRC_IQ || cfg->src == ENGINE_SRC_RAW) && (e->ao || cfg->realtime);
    if (e->cfg.rate != DSR_AUDIO_RATE * e->cfg.upsample)
        set_event(e, "Output rate %d Hz != %d x upsample %d: audio runs %s%s", e->cfg.rate,
                  DSR_AUDIO_RATE, e->cfg.upsample,
                  e->cfg.rate > DSR_AUDIO_RATE * e->cfg.upsample ? "fast" : "slow",
                  (cfg->src == ENGINE_SRC_UDP || cfg->src == ENGINE_SRC_HACKRF) ? " and ALSA will xrun/drop" : "");
    e->sel_l = cfg->start_ch & 31;
    select_channel(e, cfg->start_ch, cfg->start_stereo);

    if (cfg->src == ENGINE_SRC_UDP) {
        if (pthread_create(&e->ingress, NULL, udp_ingress_thread, e) != 0) {
            snprintf(err, errlen, "Cannot start ingress thread");
            goto fail;
        }
        e->ingress_started = true;
    }
    if (pthread_create(&e->worker, NULL, worker_thread, e) != 0) {
        snprintf(err, errlen, "Cannot start decode thread");
        goto fail;
    }
    e->worker_started = true;
    return e;

fail:
    engine_free_all(e);
    return NULL;
}

void engine_stop(engine_t *e)
{
    if (e) engine_free_all(e);
}

void engine_get_status(engine_t *e, engine_status_t *out)
{
    pthread_mutex_lock(&e->st_mu);
    *out = e->st;
    pthread_mutex_unlock(&e->st_mu);
}

void engine_select(engine_t *e, int ch, bool stereo)
{
    pthread_mutex_lock(&e->cmd_mu);
    e->cmd_select_ch = ch & 31;
    e->cmd_select_stereo = stereo;
    pthread_mutex_unlock(&e->cmd_mu);
}

void engine_step(engine_t *e, int dir)
{
    pthread_mutex_lock(&e->cmd_mu);
    e->cmd_step += dir > 0 ? 1 : -1;
    pthread_mutex_unlock(&e->cmd_mu);
}

void engine_set_stereo(engine_t *e, bool stereo)
{
    pthread_mutex_lock(&e->cmd_mu);
    e->cmd_stereo = stereo ? 1 : 0;
    pthread_mutex_unlock(&e->cmd_mu);
}

void engine_resync(engine_t *e)
{
    pthread_mutex_lock(&e->cmd_mu);
    e->cmd_resync = true;
    pthread_mutex_unlock(&e->cmd_mu);
}

void engine_set_log_level(engine_t *e, dsr_log_level_t lvl)
{
    pthread_mutex_lock(&e->cmd_mu);
    e->cmd_log = (int)lvl;
    pthread_mutex_unlock(&e->cmd_mu);
}

void engine_set_mute(engine_t *e, bool mute)
{
    atomic_store(&e->muted, mute);
}

void engine_set_volume(engine_t *e, float gain)
{
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    atomic_store(&e->gain_q15, (int)(gain * 32768.0f + 0.5f));
}
