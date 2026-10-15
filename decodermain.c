// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// decodermain.c — dsrdec: UDP/raw/IQ/HackRF input -> audio/WAV output
// Options and flow originally adopted from main.c; IQ input (--iqfile, --hackrf) is
// demodulated internally.

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>

#include "decoder.h"
#include "wave.h"
#include "alsa.h"
#include "pa.h"
#include "demod.h"
#include "hackrf_rx.h"

/* static helpers below; UDP threads are defined before their bodies (here need protos) */
static inline void output_audio_fast(decoder_t *dec, const pa_t *pa,
                                     wav_t *wout, alsa_out_t *ao, const int16_t pcmA[16],
                                     const int16_t pcmB[16], int upsample);
static void handle_keypress(char c, pa_t *pa, decoder_t *dec, int *parity_io, int udp_fd);
static void maybe_print_stats(const char *srcname, uint64_t total_bytes, uint64_t total_bits,
                              decoder_t *dec, const pa_t *pa);
static void try_clear_resync_relock(decoder_t *dec, const pa_t *pa);
static void alsa_client_enqueue(const int16_t *interleaved_samples, size_t frames);
static void *audio_output_thread(void *arg);
static void alsa_client_submit(const int16_t *interleaved_samples, size_t frames);

/* Stop flag + ALSA handle: declared early for SPSC/UDP thread helpers below */
static volatile sig_atomic_t g_stop = 0;
static alsa_out_t *g_ao = NULL;

#define APP_VER "0.8.8p-dec"
#define PA_PERIOD_BYTES (10u*1024u*1024u)  /* every ~10 MiB */

/* Wall-clock warm-up: decode/PA run; WAV + ALSA muted so sync can settle. Set 0.0 to disable. */
#define AUDIO_WARMUP_SEC 2.0

static struct timespec g_audio_warmup_t0;
static int             g_audio_warmup_t0_set;
static int             g_audio_warmup_unmute_logged;

static void audio_warmup_arm_once(void)
{
    if (g_audio_warmup_t0_set) {
        return;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &g_audio_warmup_t0) == 0) {
        g_audio_warmup_t0_set = 1;
        if (AUDIO_WARMUP_SEC > 0.0) {
            printf("[audio] %.0fs warm-up: no WAV/ALSA (decode + PA active)\n", AUDIO_WARMUP_SEC);
            fflush(stdout);
        }
    }
}

static void audio_warmup_reset(void)
{
    g_audio_warmup_t0_set = 0;
    g_audio_warmup_unmute_logged = 0;
}

/* After hard resync: same as initial run — mute playout for AUDIO_WARMUP_SEC while decode/PA settle. */
static void audio_warmup_restart_after_resync(void)
{
    if (AUDIO_WARMUP_SEC <= 0.0) {
        return;
    }
    g_audio_warmup_t0_set = 0;
    g_audio_warmup_unmute_logged = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &g_audio_warmup_t0) == 0) {
        g_audio_warmup_t0_set = 1;
        printf("[audio] %.0fs warm-up after resync: no WAV/ALSA (decode + PA active)\n",
               AUDIO_WARMUP_SEC);
        fflush(stdout);
    }
}

/* AUDIO_WARMUP_SEC is a MINIMUM mute period, not the actual gate: on live UDP input,
 * lock acquisition time varies with tuner/network conditions (unlike a file, which is
 * always the same bytes). Unmuting on a fixed timer regardless of decoder state used to
 * play out garbled audio on every start where lock took longer than the timer — Williard
 * lock, auto-deint (EVEN/ODD) decision, and PA SA-cycle lock all have to be in before
 * audio is any good. Keep muted until all of them report ready, however long that takes. */
static int audio_in_warmup(decoder_t *dec, const pa_t *pa)
{
    if (AUDIO_WARMUP_SEC <= 0.0) {
        return 0;
    }
    if (!g_audio_warmup_t0_set) {
        return 0;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    double elapsed = (double)(now.tv_sec - g_audio_warmup_t0.tv_sec)
        + 1e-9 * (double)(now.tv_nsec - g_audio_warmup_t0.tv_nsec);
    if (elapsed < AUDIO_WARMUP_SEC) return 1;

    if (!decoder_is_williard_locked(dec)) return 1;
    if (!decoder_is_deint_ready(dec)) return 1;
    if (pa && !(pa_sa_cycle_locked(pa) && pa_is_sa_offset_ready(pa))) return 1;
    return 0;
}

/* Real-time pacing for FILE input: fread()+decode races through a file at CPU/disk
 * speed with no natural pacing (unlike UDP, which arrives paced by the sender in real
 * time). With --audioout that outruns the ALSA drain rate by orders of magnitude and
 * permanently overflows the small 128 KiB PCM ring (see PCM_RING_CAP_BYTES) instead of
 * just bursting once at startup. Throttle ingestion to the rate audio is actually being
 * produced at, so the file behaves like a live capture. Only needed for live playback;
 * WAV-only conversion should stay at full speed. */
static uint64_t      g_pace_frames_emitted = 0;
static struct timespec g_pace_t0;
static int            g_pace_t0_set = 0;

static void file_pace_reset(void)
{
    g_pace_frames_emitted = 0;
    g_pace_t0_set = 0;
}

static void file_pace_tick(unsigned frames, unsigned sample_rate)
{
    if (sample_rate == 0) return;
    if (!g_pace_t0_set) {
        if (clock_gettime(CLOCK_MONOTONIC, &g_pace_t0) == 0) g_pace_t0_set = 1;
        g_pace_frames_emitted = frames;
        return;
    }
    g_pace_frames_emitted += frames;

    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
    double actual = (double)(now.tv_sec - g_pace_t0.tv_sec)
        + 1e-9 * (double)(now.tv_nsec - g_pace_t0.tv_nsec);
    double expected = (double)g_pace_frames_emitted / (double)sample_rate;
    double ahead = expected - actual;
    if (ahead <= 0.0) return;
    /* Cap a single sleep so a resync/rewind hiccup can't stall the loop for long. */
    if (ahead > 0.5) ahead = 0.5;
    struct timespec ts;
    ts.tv_sec  = (time_t)ahead;
    ts.tv_nsec = (long)((ahead - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/* SPSC byte ring (UDP ingress -> decode thread). Power-of-2; push blocks when full. */
#define SPSC_CAP_BYTES  (512u * 1024u)
/* Set >0 to delay first decode until the ring has this many bytes (jitter prefill; 0 = off). */
#define SPSC_MIN_PREFILL_BYTES  0
/* Pop batch; consumer also polls tty if nothing arrived for this long (ms) */
#define SPSC_CONSUMER_IO_WAIT_MS 5

typedef struct spsc_bytes {
    uint8_t   *buf;
    size_t     cap;          /* power of 2 */
    uint64_t   head, tail;   /* bytes pending = head - tail (under mu) */
    pthread_mutex_t mu;
    pthread_cond_t  cv;      /* not empty: consumer wait */
    pthread_cond_t  cv_room; /* space: file ingress wait (no drop) */
} spsc_bytes_t;

static int  spsc_init(spsc_bytes_t *q, size_t cap);
static void spsc_destroy(spsc_bytes_t *q);
static void spsc_reset(spsc_bytes_t *q);
/* Push all bytes; block until space (never drop — preserves bit sync). */
static void spsc_push_blocking(spsc_bytes_t *q, const uint8_t *p, size_t n);
/* Copy up to max bytes from ring; returns bytes copied. */
static size_t spsc_pop(spsc_bytes_t *q, uint8_t *dst, size_t max);
/* Block until data, timeout, g_stop, or cond broadcast. */
static void spsc_wait_data_or_stopped(spsc_bytes_t *q, int timeout_ms);

static _Atomic uint64_t g_ingress_bytes = 0;
static spsc_bytes_t  *g_ingress_spsc = NULL; /* SPSC ring (UDP or file); hard_resync clears it */
static _Atomic uint64_t g_audio_drop_bytes = 0; /* PCM bytes dropped by the output ring (see spsc_push_dropping) */

static const char *opt_host = "0.0.0.0";
static int  opt_port = 9000;
static int  tty_fd = -1;  /* File descriptor for terminal input (when STDIN is a pipe) */
static bool opt_msb_first = true;
static dsr_log_level_t opt_log_level = DSR_LOG_NONE;

static const char *log_level_name(dsr_log_level_t level){
    switch(level){
    case DSR_LOG_SYNC: return "sync";
    case DSR_LOG_WARN: return "warn";
    case DSR_LOG_ZI:   return "zi";
    default:           return "off";
    }
}

static int parse_log_level(const char *s){
    if(!s) return -1;
    if(!strcmp(s,"0") || !strcmp(s,"off") || !strcmp(s,"none")) return DSR_LOG_NONE;
    if(!strcmp(s,"1") || !strcmp(s,"sync")) return DSR_LOG_SYNC;
    if(!strcmp(s,"2") || !strcmp(s,"warn")) return DSR_LOG_WARN;
    if(!strcmp(s,"3") || !strcmp(s,"zi"))   return DSR_LOG_ZI;
    return -1;
}
static bool opt_repeat = false;   /* loop file at EOF */
static bool opt_bch_check = false;
static bool opt_descramble = true;
static const char *opt_wav = NULL;
static int  opt_rate = 32000;
static int  opt_mono = -1;
static int  opt_stereo_l = -1, opt_stereo_r = -1;
static int  opt_upsample = 1;       /* N times repeat */
static bool opt_no_auto_deint = false;
/* Decode only mux pairs needed for current L/R (less CPU). Default on; --no-stream-focus = full mux. */
static bool opt_stream_focus = true;

/* Audio output option */
static bool opt_audioout = false;

/* --- Runtime stereo-browse selector --- */
static int  sel_pair = 0;                   /* L=sel_pair, R=(sel_pair+1)%32 */
static bool sel_runtime_stereo = true;      /* default true unless --mono */

/* --- Cached channel selection --- */
static int cached_Lch = 0;
static int cached_Rch = 1;
static bool cached_is_mono = false;

/* input file (positional or --in). If NULL -> UDP; "-" -> stdin */
static const char *opt_infile = NULL;

/* raw IQ input (--iqfile). When set, takes priority over opt_infile/UDP: dsrdec
 * demodulates internally (formerly the separate dsrdemod binary) and feeds bits
 * straight into the decoder via decoder_push_ab_bits — no intermediate byte-packed
 * bitstream, no pipe. */
static const char *opt_iqfile = NULL;
static unsigned    opt_iqrate = 20480000;   /* IQ sample rate, default = encoder fs */
static const char *opt_iqfmt  = "s16le";    /* s16le|f32le|s8 */
/* Receiver path of the demodulator (timing + carrier recovery). Needed for anything that
 * went over the air or through an SDR; encoder IQ files decode bit-exactly without it. */
static bool        opt_track  = false;

/* HackRF One as live source (--hackrf): IQ path with track on, s8 samples. */
static bool        opt_hackrf = false;
static hackrf_rx_cfg_t opt_rx;
static bool        opt_iqrate_set = false;
/* --track/--hackrf: no Williard lock for this long (of signal) -> reset demod + decoder
 * and acquire again (fresh strobe, loops and auto-map). */
#define ACQ_RETRY_SEC 3

static demod_t     *g_track_dm = NULL;   /* for the [rf] stat line */
static hackrf_rx_t *g_rx = NULL;

static uint64_t last_stat_bucket = UINT64_MAX;     // so we print immediately on first call
static const uint64_t STAT_STEP_BITS = 40ULL * 1000ULL * 1000ULL; // every 40 Mbit
static bool g_initial_autodetect_done = false;

/* After 'r' resync: drop bits from decoder for this long (wall), then cold re-acquire */
#define RESYNC_INGEST_PAUSE_MS 120
#define RESYNC_MIN_INTERVAL_MS 1000
static struct timespec g_ingest_resume_mono;
static struct timespec g_last_resync_mono;
static int g_ingest_paused = 0;
static int g_wait_resync_relock = 0;

static void arm_ingest_pause_ms(int ms)
{
    if (clock_gettime(CLOCK_MONOTONIC, &g_ingest_resume_mono) != 0) {
        g_ingest_paused = 0;
        return;
    }
    g_ingest_resume_mono.tv_nsec += (long)ms * 1000000L;
    while (g_ingest_resume_mono.tv_nsec >= 1000000000L) {
        g_ingest_resume_mono.tv_sec++;
        g_ingest_resume_mono.tv_nsec -= 1000000000L;
    }
    g_ingest_paused = 1;
}

static int ingest_allowed(void)
{
    if (!g_ingest_paused) return 1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        g_ingest_paused = 0;
        return 1;
    }
    if (now.tv_sec > g_ingest_resume_mono.tv_sec ||
        (now.tv_sec == g_ingest_resume_mono.tv_sec && now.tv_nsec >= g_ingest_resume_mono.tv_nsec)) {
        g_ingest_paused = 0;
        return 1;
    }
    return 0;
}

static int elapsed_ms_since(const struct timespec *since, long *out_ms)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    long sec = (long)(now.tv_sec - since->tv_sec);
    long nsec = (long)(now.tv_nsec - since->tv_nsec);
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    if (sec < 0) sec = 0;
    if (out_ms) *out_ms = sec * 1000L + nsec / 1000000L;
    return 1;
}

/* Drop queued UDP datagrams so resync starts on fresh ingress. */
static size_t flush_udp_backlog(int fd)
{
    if (fd < 0) return 0;
    uint8_t tmp[2048];
    size_t dropped = 0;
    for (int i = 0; i < 4096; i++) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n > 0) {
            dropped += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        break;
    }
    return dropped;
}

static int spsc_init(spsc_bytes_t *q, size_t cap)
{
    if (!q || cap < 256u || (cap & (cap - 1u)) != 0) return -1;
    q->buf = (uint8_t *)malloc(cap);
    if (!q->buf) return -1;
    q->cap = cap;
    q->head = q->tail = 0;
    if (pthread_mutex_init(&q->mu, NULL) != 0) {
        free(q->buf);
        q->buf = NULL;
        return -1;
    }
    if (pthread_cond_init(&q->cv, NULL) != 0) {
        pthread_mutex_destroy(&q->mu);
        free(q->buf);
        q->buf = NULL;
        return -1;
    }
    if (pthread_cond_init(&q->cv_room, NULL) != 0) {
        pthread_cond_destroy(&q->cv);
        pthread_mutex_destroy(&q->mu);
        free(q->buf);
        q->buf = NULL;
        return -1;
    }
    return 0;
}

static void spsc_destroy(spsc_bytes_t *q)
{
    if (!q) return;
    pthread_cond_destroy(&q->cv_room);
    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mu);
    free(q->buf);
    q->buf = NULL;
    q->cap = 0;
    q->head = q->tail = 0;
}

static void spsc_reset(spsc_bytes_t *q)
{
    if (!q || !q->buf) return;
    pthread_mutex_lock(&q->mu);
    q->head = q->tail = 0;
    pthread_cond_broadcast(&q->cv);
    pthread_cond_broadcast(&q->cv_room);
    pthread_mutex_unlock(&q->mu);
}

static void spsc_push_blocking(spsc_bytes_t *q, const uint8_t *p, size_t n)
{
    if (!q || !q->buf || n == 0 || !p) return;
    const uint64_t cap = (uint64_t)q->cap;
    const uint64_t mask = cap - 1u;
    size_t off = 0;
    while (off < n) {
        pthread_mutex_lock(&q->mu);
        for (;;) {
            if (g_stop) {
                pthread_mutex_unlock(&q->mu);
                return;
            }
            if (q->head - q->tail < cap) break;
            struct timespec ts;
            (void)clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100L * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += ts.tv_nsec / 1000000000L;
                ts.tv_nsec %= 1000000000L;
            }
            (void)pthread_cond_timedwait(&q->cv_room, &q->mu, &ts);
        }
        uint64_t used = q->head - q->tail;
        size_t free_sp = (size_t)(cap - used);
        size_t need = n - off;
        size_t take = (need < free_sp) ? need : free_sp;
        for (size_t i = 0; i < take; i++) {
            q->buf[(q->head + (uint64_t)i) & mask] = p[off + i];
        }
        q->head += (uint64_t)take;
        off += take;
        pthread_cond_signal(&q->cv);
        pthread_mutex_unlock(&q->mu);
    }
}

/* Push without ever blocking the caller: on overflow, evict the oldest frame-aligned
 * chunk to make room. Used for PCM audio output only — a dropped chunk is a brief,
 * bounded click; unlike spsc_push_blocking, it must never stall the caller, because
 * the caller here is the decode thread and stalling it would backpressure into the
 * UDP ingress path (which is exactly the failure mode that corrupts bit sync / ZI
 * decoding under CPU load — see decoder.c's zi_acc/sc_phase state). */
static void spsc_push_dropping(spsc_bytes_t *q, const uint8_t *p, size_t n, size_t align)
{
    if (!q || !q->buf || n == 0 || !p) return;
    const uint64_t cap  = (uint64_t)q->cap;
    const uint64_t mask = cap - 1u;
    pthread_mutex_lock(&q->mu);
    uint64_t used = q->head - q->tail;
    uint64_t free_sp = cap - used;
    if ((uint64_t)n > free_sp) {
        uint64_t need = (uint64_t)n - free_sp;
        if (align > 1) need = ((need + (uint64_t)align - 1u) / (uint64_t)align) * (uint64_t)align;
        if (need > used) need = used;
        q->tail += need;
        uint64_t dropped_total = atomic_fetch_add(&g_audio_drop_bytes, need) + need;
        static uint64_t warn_count = 0;
        if ((++warn_count % 200) == 1) {
            fprintf(stderr, "[audio] output ring overflow, dropped %" PRIu64 " bytes total "
                            "(ALSA/device can't keep up — brief glitch; decode path unaffected)\n",
                    dropped_total);
        }
    }
    for (size_t i = 0; i < n; i++) {
        q->buf[(q->head + (uint64_t)i) & mask] = p[i];
    }
    q->head += (uint64_t)n;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

static size_t spsc_pop(spsc_bytes_t *q, uint8_t *dst, size_t max)
{
    if (!q || !q->buf || !dst || max == 0) return 0;
    const uint64_t mask = (uint64_t)q->cap - 1u;
    pthread_mutex_lock(&q->mu);
    uint64_t avail = q->head - q->tail;
    size_t n = (avail < (uint64_t)max) ? (size_t)avail : max;
    for (size_t i = 0; i < n; i++) {
        dst[i] = q->buf[(q->tail + (uint64_t)i) & mask];
    }
    q->tail += (uint64_t)n;
    if (n > 0) {
        pthread_cond_broadcast(&q->cv_room);
    }
    pthread_mutex_unlock(&q->mu);
    return n;
}

static void spsc_wait_data_or_stopped(spsc_bytes_t *q, int timeout_ms)
{
    if (!q || !q->buf) return;
    pthread_mutex_lock(&q->mu);
    if (q->head != q->tail || g_stop) {
        pthread_mutex_unlock(&q->mu);
        return;
    }
    struct timespec abs_ts;
    if (clock_gettime(CLOCK_REALTIME, &abs_ts) != 0) {
        pthread_mutex_unlock(&q->mu);
        (void)nanosleep(
            &(struct timespec){ .tv_sec = 0, .tv_nsec = (long)timeout_ms * 1000000L }, NULL);
        return;
    }
    {
        long add_ns = (long)timeout_ms * 1000000L;
        abs_ts.tv_nsec += add_ns;
        if (abs_ts.tv_nsec >= 1000000000L) {
            abs_ts.tv_sec += abs_ts.tv_nsec / 1000000000L;
            abs_ts.tv_nsec %= 1000000000L;
        }
    }
    while (q->head == q->tail && !g_stop) {
        int rc = pthread_cond_timedwait(&q->cv, &q->mu, &abs_ts);
        if (rc == ETIMEDOUT) {
            break;
        }
    }
    pthread_mutex_unlock(&q->mu);
}

/* Context for ingress (UDP recv or file fread) + shared decode consumer (stack in main). */
typedef struct {
    spsc_bytes_t   *ring;
    int             udp_fd;  /* valid for UDP; -1 when using fp */
    FILE           *fp;      /* file/stdin for raw bitstream; NULL for UDP */
    decoder_t      *dec;
    pa_t           *pa;
    wav_t          *wout;
    const char     *stat_src; /* "udp" / "file" for [stat] lines */
    _Atomic uint64_t file_repeat_seq; /* only used when fp!=NULL: increments on --repeat rewind */
} io_threads_ctx_t;

static void spsc_wait_for_min_prefill(spsc_bytes_t *q)
{
#if SPSC_MIN_PREFILL_BYTES == 0
    (void)q;
    return;
#else
    if (!q) return;
    const uint64_t need = (uint64_t)SPSC_MIN_PREFILL_BYTES;
    pthread_mutex_lock(&q->mu);
    while (!g_stop && (q->head - q->tail) < need) {
        struct timespec abs_ts;
        (void)clock_gettime(CLOCK_REALTIME, &abs_ts);
        abs_ts.tv_nsec += 20L * 1000000L;
        if (abs_ts.tv_nsec >= 1000000000L) {
            abs_ts.tv_sec++;
            abs_ts.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&q->cv, &q->mu, &abs_ts);
    }
    pthread_mutex_unlock(&q->mu);
#endif
}

static void *udp_ingress_thread(void *arg)
{
    io_threads_ctx_t *c = (io_threads_ctx_t *)arg;
    spsc_bytes_t *ring = c->ring;
    int fd = c->udp_fd;
    uint8_t scratch[64 * 1024u];
    while (!g_stop) {
        ssize_t n = recv(fd, scratch, sizeof(scratch), 0);
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                (void)nanosleep(
                    &(struct timespec){ .tv_sec = 0, .tv_nsec = 1000000L }, NULL);
                continue;
            }
            if (!g_stop) perror("udp_ingress: recv");
            g_stop = 1;
            pthread_cond_broadcast(&ring->cv);
            break;
        }
        if (n == 0) {
            continue;
        }
        atomic_fetch_add(&g_ingress_bytes, (uint64_t)n);
        if (ingest_allowed()) {
            spsc_push_blocking(ring, scratch, (size_t)n);
        }
    }
    return NULL;
}



static void *decode_consumer_thread(void *arg)
{
    io_threads_ctx_t *c = (io_threads_ctx_t *)arg;
    spsc_bytes_t *ring = c->ring;
    const int key_fd = c->udp_fd; /* -1 for file path resync/keys */
    uint8_t batch[8 * 1024u];
    uint64_t total_bits = 0;
    uint64_t last_file_repeat = 0;
    int parity = 0;

    spsc_wait_for_min_prefill(ring);
    audio_warmup_arm_once();

    while (!g_stop) {
        if (g_stop) {
            break;
        }

        if (c->fp) {
            uint64_t rs = atomic_load(&c->file_repeat_seq);
            if (rs != last_file_repeat) {
                last_file_repeat = rs;
                total_bits = 0;
            }
        }

        size_t n = spsc_pop(ring, batch, sizeof(batch));
        if (n == 0) {
            spsc_wait_data_or_stopped(ring, SPSC_CONSUMER_IO_WAIT_MS);
            if (tty_fd >= 0) {
                char ch;
                if (read(tty_fd, &ch, 1) == 1) {
                    handle_keypress(ch, c->pa, c->dec, &parity, key_fd);
                }
            }
            continue;
        }

        if (ingest_allowed()) {
            for (size_t i = 0; i < n; i++) {
                int16_t pcmA[16], pcmB[16];
                if (decoder_push_ab_u8(c->dec, batch[i], opt_msb_first, &parity, pcmA, pcmB)) {
                    output_audio_fast(c->dec, c->pa, c->wout, g_ao, pcmA, pcmB, opt_upsample);
                }
                total_bits += 8u;
            }
        }

        try_clear_resync_relock(c->dec, c->pa);
        {
            uint64_t tb = atomic_load(&g_ingress_bytes);
            maybe_print_stats(c->stat_src, tb, total_bits, c->dec, c->pa);
        }
        if (tty_fd >= 0) {
            for (;;) {
                char ch;
                if (read(tty_fd, &ch, 1) != 1) {
                    break;
                }
                handle_keypress(ch, c->pa, c->dec, &parity, key_fd);
            }
        }
    }
    return NULL;
}

/* Genre/Type names for DSR PA type field (from DSR specification) */
static const char *pa_type_names[16] = {
    "KEINE",        /* 0  - no programme type */
    "NACHRICH",     /* 1  - news */
    "POLITIK",      /* 2  - politics and current affairs */
    "SPEZWORT",     /* 3  - special word programmes */
    "SPORT",        /* 4  - sports */
    "LERNEN",       /* 5  - education */
    "HOER+LIT",     /* 6  - drama and literature */
    "KULTUR",       /* 7  - culture, religion and society */
    "WISSEN",       /* 8  - science */
    "UNTERHAL",     /* 9  - entertainment speech */
    "POP",          /* 10 - pop music */
    "ROCK",         /* 11 - rock music */
    "U-MUSIK",      /* 12 - easy listening music */
    "L-KLASS",      /* 13 - light classical music */
    "E-KLASS",      /* 14 - serious classical music */
    "SPEZ MUS"      /* 15 - special music programmes */
};

static uint64_t last_pa_reset_bytes = 0;

/* ---- ASCII DSR banner ---- */
static void print_logo(void)
{
    static const char *art =
"   :.  .-   -: .-- .-: :--.:--.:-------:.     .:-======---====-:.  :\n"
"   *-  =%. :%+ -@%.:@# +@%:#@@=#@@@#*#@%%*:  =%@%******%@@@*+#@%%=  \n"
"   *-  =%. :%+ -@%.:@# +@%:#@@=#@%@=  -%@@%--@@@%-.    #@%@- .#@@@: \n"
"   *-  =%: :%+ -@%.:@#.+@%:#@@=#@%@=   *@%@#-%@@@@%%*-.#@%%-.-%%%*  \n"
"   *-  =%: .%+ :@%.:@#.+@%:#@@=#@%@=   +@%@%..=*##%@@@=#@%%=+@@%=   \n"
"   *-  -%: :%+ :@%.:@#.+@%:#@@=#@%@=  :%%%%+      +@@@=#@%@- +@@%-  \n"
"   *-  -@: :%+ :@%.:@%.+@%:#@@=#@@@#=+%@@@%*******%@%= #@@@-  =%@%= \n"
"   =:  :+. .+- .++ .+= -++.=++:=++++++++++++++++++=-.  -===.   -===.\n";
    fputs(art, stdout);
    fputc('\n', stdout);
}

static void on_sigint(int s){ (void)s; g_stop=1; }

static inline int set_nonblock(int fd){
    int fl = fcntl(fd, F_GETFL, 0);
    if(fl<0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
/* --- stereo pair helpers (force even base) --- */
static inline int  pair_even(int v){ return v & ~1; }
static inline void pair_up(void){   sel_pair = (pair_even(sel_pair) + 2)  & 31; }
static inline void pair_down(void){ sel_pair = (pair_even(sel_pair) + 30) & 31; } /* -2 mod 32 */

/* Update cached channel selection (call after any channel change). dec may be NULL (early init). */
static inline void update_cached_channels(decoder_t *dec) {
    if (opt_mono >= 0) {
        cached_is_mono = true;
        cached_Lch = opt_mono & 31;
        cached_Rch = cached_Lch;
    } else if (sel_runtime_stereo) {
        cached_is_mono = false;
        int base = pair_even(sel_pair);
        cached_Lch = base;
        cached_Rch = base ^ 1;
    } else {
        cached_is_mono = false;
        cached_Lch = (opt_stereo_l >= 0) ? (opt_stereo_l & 31) : 0;
        cached_Rch = (opt_stereo_r >= 0) ? (opt_stereo_r & 31) : 1;
    }
    if (dec && opt_stream_focus)
        decoder_set_stream_focus(dec, cached_Lch, cached_Rch, cached_is_mono ? 1 : 0);
}

static inline void print_sel(void){
    int L = pair_even(sel_pair), R = L ^ 1;
    if (sel_runtime_stereo) {
        printf("[stereo] L=%d R=%d\n", L, R);
    } else {
        printf("[mono] ch=%d\n", opt_mono);
    }
}

static void print_prog_for_current(const pa_t *pa)
{
    if (!pa) return;

    if (sel_runtime_stereo) {
        int L = (sel_pair & ~1), R = L ^ 1;
        char ln[64] = "", rn[64] = "";
        int tL=0,mL=0,sL=0, tR=0,mR=0,sR=0;

        pa_get_name_utf8(pa, L, ln, sizeof ln);
        pa_get_name_utf8(pa, R, rn, sizeof rn);
        pa_get_pa(pa, L, &tL,&mL,&sL);
        pa_get_pa(pa, R, &tR,&mR,&sR);

        printf("[prog] L=%02d PA=T%d M%d S%d \"%s\" | R=%02d PA=T%d M%d S%d \"%s\"\n",
               L,tL,mL,sL, ln[0]?ln:"?",
               R,tR,mR,sR, rn[0]?rn:"?");
    } else {
        int ch = opt_mono & 31;
        char nm[64] = ""; int t=0,m=0,s=0;
        pa_get_name_utf8(pa, ch, nm, sizeof(nm));
        pa_get_pa(pa, ch, &t,&m,&s);
        printf("[prog] CH=%02d PA=T%d M%d S%d \"%s\"\n",
               ch, t,m,s, nm[0]?nm:"?");
    }
}

static void auto_detect_mode_dir(const pa_t *pa, int direction)
{
    (void)direction;
    if (!pa) return;

    /* Determine current position based on mode - preserve user's channel selection */
    int current_ch = sel_runtime_stereo ? pair_even(sel_pair) : (opt_mono & 31);
    int base = pair_even(current_ch);
    int L = base, R = base ^ 1;
    int tL=0, mL=0, modeL=0;
    int tR=0, mR=0, modeR=0;

    bool validL = pa_get_pa(pa, L, &tL, &mL, &modeL);
    bool validR = pa_get_pa(pa, R, &tR, &mR, &modeR);

    /* Only make decisions if we have valid PA data for at least one channel */
    if (!validL && !validR) return;

    /* Stereo pair: L has mode=1, R has mode=2 */
    /* Mono: L has mode=1 but R doesn't have mode=2 */
    if (validL && validR && modeL == 1 && modeR == 2) {
        /* Stereo pair detected - switch to stereo mode but keep same channel */
        if (!sel_runtime_stereo) {
            sel_runtime_stereo = true;
            opt_mono = -1;
            sel_pair = base;  /* Keep same channel pair */
            printf("[auto] Stereo pair detected\n");
        }
    } else {
        /* Mono channel(s) detected - switch to mono mode but preserve channel selection */
        if (sel_runtime_stereo && (validL || validR)) {
            sel_runtime_stereo = false;
            /* Preserve the current channel selection when switching to mono */
            /* Use the lower channel (L) to preserve user's selection as much as possible */
            opt_mono = L;
            sel_pair = base;  /* Keep sel_pair in sync */
            printf("[auto] Mono channel detected\n");
        }
    }
}

static void auto_detect_mode(const pa_t *pa)
{
    auto_detect_mode_dir(pa, 0);
}

static void usage(const char *p){
    fprintf(stderr,
    "dsrdec %s\n"
    "Usage: %s [infile|-] [options]\n"
    "  infile              Raw byte file with interleaved A/B bitstream; '-' for stdin\n"
    "  --in PATH           Same as positional infile\n"
    "  --host H            Listen host (default %s)   [ignored if infile/iqfile given]\n"
    "  --port P            UDP port (default %d)      [ignored if infile/iqfile given]\n"
    "  --iqfile PATH       Raw IQ file (s16le|f32le|s8); demodulated internally, no\n"
    "                      separate dsrdemod/pipe needed. Takes priority over infile/UDP.\n"
    "  --iqrate FS         IQ sample rate for --iqfile/--hackrf (default 20480000)\n"
    "  --iqfmt {s16le|f32le|s8}  IQ sample format for --iqfile (default s16le;\n"
    "                      s8 = hackrf_transfer recordings)\n"
    "  --track             Symbol timing + carrier recovery for --iqfile (SDR recordings;\n"
    "                      not needed for encoder IQ files)\n"
    "  --hackrf            Live reception with a HackRF One (built-in demodulator)\n"
    "  --freq MHZ          HackRF centre frequency in MHz (default 118.0)\n"
    "  --lna DB            HackRF LNA gain 0..40, step 8 (default %u; disables auto gain)\n"
    "  --vga DB            HackRF VGA gain 0..62, step 2 (default %u; disables auto gain)\n"
    "  --amp               HackRF RF amplifier on (+14 dB)\n"
    "  --no-auto-gain      Keep LNA/VGA fixed (default: adjusted to the input level)\n"
    "  --bw HZ             HackRF baseband filter (default: from sample rate)\n"
    "  --serial S          HackRF serial number (default: first device)\n"
    "  --lsb               Bits LSB-first within byte (default MSB-first) [infile/UDP only]\n"
    "  --repeat            On EOF (file input) restart from beginning\n"
    "  --no-descramble     Disable PRBS descramble of payload\n"
    "  --bch-check         Enable BCH(63,44) verification (no correction)\n"
    "  --no-auto-deint     Disable BCH-driven auto deinterleave (use EVEN->blk0)\n"
    "  --log-level LVL     Decoder log: off|sync|warn|zi (or 0..3; default off)\n"
    "  --verbose           Alias for --log-level zi\n"
    "  --wav FILE.wav      Write WAV (16-bit PCM, always stereo; mono output is L=R)\n"
    "  --rate SR           Output sample rate (default 32000); must be 32000 * upsample\n"
    "  --mono N            Use mono from subchannel N (0..31)\n"
    "  --stereo L,R        Use stereo from subchannels L,R (0..31)\n"
    "  --upsample N        Repeat each decoded 32 kHz sample N times (default 1);\n"
    "                      set --rate to 32000*N accordingly\n"
    "  --stream-focus      Decode only the selected pair (default)\n"
    "  --no-stream-focus   Decode full mux (all pairs); default is stream-focus on (less CPU)\n"
    "  --audioout          Enable live audio output via ALSA\n"
    "  -h, --help          Show this help\n"
    "\n"
    "Keys: u/d = channel up/down, m/s = force mono/stereo, v = cycle log level,\n"
    "      t = channel table, p = PA debug, r = hard resync, h = key help, q = quit\n"
    , APP_VER, p, opt_host, opt_port, HACKRF_RX_DEFAULT_LNA, HACKRF_RX_DEFAULT_VGA);
}

static int parse_args(int argc, char **argv){
    hackrf_rx_default_cfg(&opt_rx);
    for(int i=1;i<argc;i++){
        if(argv[i][0] != '-'){ opt_infile = argv[i]; continue; }
        if(!strcmp(argv[i],"-")){ opt_infile = "-"; continue; }  /* stdin marker */
        if(!strcmp(argv[i],"--in") && i+1<argc){ opt_infile=argv[++i];
        }else if(!strcmp(argv[i],"--host") && i+1<argc){ opt_host=argv[++i];
        }else if(!strcmp(argv[i],"--port") && i+1<argc){ opt_port=atoi(argv[++i]);
        }else if(!strcmp(argv[i],"--iqfile") && i+1<argc){ opt_iqfile=argv[++i];
        }else if(!strcmp(argv[i],"--iqrate") && i+1<argc){ opt_iqrate=(unsigned)atoi(argv[++i]); opt_iqrate_set=true;
        }else if(!strcmp(argv[i],"--iqfmt") && i+1<argc){ opt_iqfmt=argv[++i];
        }else if(!strcmp(argv[i],"--track")){ opt_track=true;
        }else if(!strcmp(argv[i],"--hackrf")){ opt_hackrf=true;
        }else if(!strcmp(argv[i],"--freq") && i+1<argc){ opt_rx.freq_hz=(uint64_t)(atof(argv[++i])*1e6+0.5);
        }else if(!strcmp(argv[i],"--lna") && i+1<argc){ opt_rx.lna_db=(unsigned)atoi(argv[++i]); opt_rx.auto_gain=false;
        }else if(!strcmp(argv[i],"--vga") && i+1<argc){ opt_rx.vga_db=(unsigned)atoi(argv[++i]); opt_rx.auto_gain=false;
        }else if(!strcmp(argv[i],"--amp")){ opt_rx.amp=true;
        }else if(!strcmp(argv[i],"--no-auto-gain")){ opt_rx.auto_gain=false;
        }else if(!strcmp(argv[i],"--bw") && i+1<argc){ opt_rx.bw_hz=(unsigned)atoi(argv[++i]);
        }else if(!strcmp(argv[i],"--serial") && i+1<argc){ snprintf(opt_rx.serial,sizeof(opt_rx.serial),"%s",argv[++i]);
        }else if(!strcmp(argv[i],"--lsb")){ opt_msb_first=false;
        }else if(!strcmp(argv[i],"--repeat")){ opt_repeat = true;
        }else if(!strcmp(argv[i],"--no-descramble")){ opt_descramble=false;
        }else if(!strcmp(argv[i],"--bch-check")){ opt_bch_check=true;
        }else if(!strcmp(argv[i],"--no-auto-deint")){ opt_no_auto_deint=true;
        }else if(!strcmp(argv[i],"--log-level") && i+1<argc){
            int lvl = parse_log_level(argv[++i]);
            if(lvl < 0){ fprintf(stderr,"Invalid --log-level: %s (use off|sync|warn|zi or 0..3)\n", argv[i]); return -1; }
            opt_log_level = (dsr_log_level_t)lvl;
        }else if(!strcmp(argv[i],"--verbose")){ opt_log_level = DSR_LOG_ZI;
        }else if(!strcmp(argv[i],"--wav") && i+1<argc){ opt_wav=argv[++i];
        }else if(!strcmp(argv[i],"--rate") && i+1<argc){ opt_rate=atoi(argv[++i]);
        }else if(!strcmp(argv[i],"--mono") && i+1<argc){ opt_mono=atoi(argv[++i]);
        }else if(!strcmp(argv[i],"--stereo") && i+1<argc) {
            int L=-1,R=-1; sscanf(argv[++i], "%d,%d",&L,&R); opt_stereo_l=L; opt_stereo_r=R;
        }else if(!strcmp(argv[i],"--upsample") && i+1<argc){ opt_upsample=atoi(argv[++i]);
        }else if(!strcmp(argv[i],"--audioout")){ opt_audioout=true;
        }else if(!strcmp(argv[i],"--no-stream-focus")){ opt_stream_focus=false;
        }else if(!strcmp(argv[i],"--stream-focus")){ opt_stream_focus=true;
        }else if(!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")){ usage(argv[0]); return -1;
        }else{
            fprintf(stderr,"Unknown option: %s\n", argv[i]); usage(argv[0]); return -1;
        }
    }
    if(opt_upsample < 1) opt_upsample = 1;
    if(!strcmp(opt_iqfmt,"cs8")) opt_iqfmt = "s8";
    if(opt_iqfile && strcmp(opt_iqfmt,"s16le")!=0 && strcmp(opt_iqfmt,"f32le")!=0 && strcmp(opt_iqfmt,"s8")!=0){
        fprintf(stderr,"Invalid --iqfmt: %s (use s16le|f32le|s8)\n", opt_iqfmt);
        return -1;
    }
    if(opt_hackrf){
        if(opt_iqfile || opt_infile){
            fprintf(stderr,"--hackrf cannot be combined with --iqfile/infile\n");
            return -1;
        }
        if(opt_rx.freq_hz < 1000000ULL || opt_rx.freq_hz > 7250000000ULL){
            fprintf(stderr,"Invalid --freq (HackRF range 1..7250 MHz)\n");
            return -1;
        }
        if(opt_iqrate_set) opt_rx.sample_rate = opt_iqrate;
        opt_iqrate = opt_rx.sample_rate;
        opt_iqfmt = "s8";
        opt_track = true;
    }

    /* Init runtime stereo selection */
    if (opt_mono >= 0) {
        sel_runtime_stereo = false;
    } else {
        sel_runtime_stereo = true;
        int base;
        if (opt_stereo_l >= 0 && opt_stereo_r >= 0) {
            base = (opt_stereo_l < opt_stereo_r ? opt_stereo_l : opt_stereo_r) & 31;
        } else if (opt_stereo_l >= 0) {
            base = opt_stereo_l & 31;
        } else {
            base = 0;
        }
        sel_pair = pair_even(base);
    }
    update_cached_channels(NULL);
    return 0;
}

static int udp_bind_socket(const char *host, int port){
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd<0){ perror("socket"); return -1; }

    int sz = 4<<20;  // 4 MiB
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));

    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port); addr.sin_addr.s_addr = inet_addr(host);
    if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("bind"); close(fd); return -1; }
    if(set_nonblock(fd)<0){ perror("fcntl"); close(fd); return -1; }
    return fd;
}

/* --- Upsampled output buffer --- */
#define MAX_UPSAMPLE 128
static int16_t g_upsample_stereo[MAX_UPSAMPLE * 2];

/* Keyboard noncanonical */
static struct termios oldt;
static void tty_raw(){ struct termios t; tcgetattr(STDIN_FILENO,&oldt); t=oldt; t.c_lflag &= ~(ICANON|ECHO); tcsetattr(STDIN_FILENO,TCSANOW,&t); }
static void tty_restore(){ tcsetattr(STDIN_FILENO,TCSANOW,&oldt); }

/* ALSA output bundling (small jitter buffer on user side) */
static int g_ao_channels = 0;
static size_t g_ao_period_frames = 0;
static int16_t *g_ao_buf = NULL;
static size_t g_ao_len_frames = 0;
static size_t g_ao_cap_frames = 0;

/* Decoupled audio output: the decode thread only ever pushes into this ring
 * (spsc_push_dropping, non-blocking). A dedicated audio_output_thread is the sole
 * caller of alsa_write, so a stalled/blocking ALSA device can never stall the decode
 * thread or, transitively, the UDP ingress thread that feeds it. */
#define PCM_RING_CAP_BYTES (128u * 1024u)
static spsc_bytes_t *g_pcm_ring = NULL;
static pthread_t     g_audio_thread;
static int           g_audio_thread_running = 0;
/* g_ao_buf/g_ao_len_frames belong to audio_output_thread alone. Other threads (e.g.
 * decode_consumer_thread on a channel-change keypress) must never touch them directly —
 * that used to be safe when one thread did both decode and ALSA I/O, but is a data race
 * (and heap corruption via the buffered memcpy/memmove in alsa_client_enqueue) now that
 * they're separate threads. Request a flush through this flag instead. */
static _Atomic int   g_audio_flush_requested = 0;
static void audio_request_flush(void){
    if(g_pcm_ring) spsc_reset(g_pcm_ring);
    atomic_store(&g_audio_flush_requested, 1);
}

/* DC-Blocker filter state (removes DC offset to prevent distortion) */
static float g_dc_left = 0.0f;
static float g_dc_right = 0.0f;
static const float DC_ALPHA = 0.995f;  /* Filter coefficient for DC estimation */

/* Glitch reduction: only clamp physically-implausible single-sample jumps (near
 * full-scale), as a last-resort backstop against genuine decode garbage. Measured
 * against real captured DSR audio, a fixed 8000 threshold (~24% of full scale) fires
 * on ~8% of ALL samples in bursts of ~20 consecutive samples — the signature of
 * ordinary loud/bright musical transients, not decode errors, and was itself the
 * dominant source of audible clicking on loud passages (see MAX_SLEW usage below). */
static int16_t g_last_left = 0;
static int16_t g_last_right = 0;
static const int16_t MAX_SLEW = 28000;  /* ~85% of full scale: catches only near-full-scale garbage jumps */

//ALSA Buffer Size
#define ALSA_PERIOD_MS 16
#define ALSA_BUFFER_MS 128 

static int alsa_client_open(unsigned rate, int channels){
    if(!opt_audioout) return 0;
    g_ao_channels = channels;
    /* Reset DC-blocker and glitch reduction state */
    g_dc_left = 0.0f;
    g_dc_right = 0.0f;
    g_last_left = 0;
    g_last_right = 0;
    size_t pf = (size_t)((uint64_t)rate * (uint64_t)ALSA_PERIOD_MS / 1000u);
    if(pf < 32) pf = 32;
    g_ao_period_frames = pf;
    /* WSL Performance: Larger buffer capacity to handle timing jitter */
    g_ao_cap_frames = g_ao_period_frames * 8;  /* Increased from 4x to 8x for WSL */
    g_ao_buf = (int16_t*)malloc(g_ao_cap_frames * (size_t)g_ao_channels * sizeof(int16_t));
    if(!g_ao_buf){ fprintf(stderr,"[alsa] malloc failed\n"); return -1; }
    g_ao_len_frames = 0;

    /* Blocking playback: write waits for space in the device buffer (no sample discard on EAGAIN).
     * Old non-blocking mode caused periodic clicks: alsa.c dropped the tail of each write when
     * the PCM buffer was full. Blocking here is safe ONLY because alsa_write is now called
     * exclusively from audio_output_thread (below) — never from the decode thread — so a
     * blocked write cannot backpressure into the UDP ingress path.
     */
    g_ao = alsa_open(NULL, rate, (unsigned)channels,
                     ALSA_PERIOD_MS, ALSA_BUFFER_MS, 0);  /* nonblock=0 */
    if(!g_ao){
        fprintf(stderr,"[alsa] open failed — running without ALSA\n");
        free(g_ao_buf); g_ao_buf=NULL; g_ao_cap_frames=g_ao_period_frames=g_ao_len_frames=0;
        opt_audioout = false;
        return 0;
    }

    g_pcm_ring = (spsc_bytes_t*)calloc(1, sizeof(*g_pcm_ring));
    if(!g_pcm_ring || spsc_init(g_pcm_ring, PCM_RING_CAP_BYTES) != 0){
        fprintf(stderr,"[audio] output ring alloc failed — running without ALSA\n");
        free(g_pcm_ring); g_pcm_ring=NULL;
        alsa_close(g_ao); g_ao=NULL;
        free(g_ao_buf); g_ao_buf=NULL; g_ao_cap_frames=g_ao_period_frames=g_ao_len_frames=0;
        opt_audioout = false;
        return 0;
    }
    atomic_store(&g_audio_drop_bytes, 0);
    if(pthread_create(&g_audio_thread, NULL, audio_output_thread, NULL) != 0){
        fprintf(stderr,"[audio] failed to start output thread — running without ALSA\n");
        spsc_destroy(g_pcm_ring); free(g_pcm_ring); g_pcm_ring=NULL;
        alsa_close(g_ao); g_ao=NULL;
        free(g_ao_buf); g_ao_buf=NULL; g_ao_cap_frames=g_ao_period_frames=g_ao_len_frames=0;
        opt_audioout = false;
        return 0;
    }
    g_audio_thread_running = 1;

    fprintf(stderr,"[alsa] using device=default rate=%u ch=%d period~%zu frames | output ring=%u KiB (dedicated thread)\n",
            rate, channels, g_ao_period_frames, (unsigned)(PCM_RING_CAP_BYTES / 1024u));
    return 0;
}
static void alsa_client_enqueue(const int16_t *interleaved_samples, size_t frames){
    if(!g_ao) return;
    size_t left = frames;
    const int16_t *p = interleaved_samples;
    while(left){
        size_t space = (g_ao_cap_frames - g_ao_len_frames);
        if(space == 0){
            /* Buffer full - try to write aggressively to make room */
            /* WSL Performance: Try harder to write before dropping samples */
            if(g_ao_len_frames > 0){
                /* Try to write even if less than period_frames to prevent overflow */
                size_t write_size = g_ao_len_frames;
                if(write_size > g_ao_period_frames * 2) write_size = g_ao_period_frames * 2;
                ssize_t wrote = alsa_write(g_ao, g_ao_buf, write_size);
                if(wrote > 0){
                    /* Remove only the written samples, the rest stays in the buffer */
                    size_t wrote_frames = (size_t)wrote;
                    if(wrote_frames < g_ao_len_frames){
                        memmove(g_ao_buf, 
                                g_ao_buf + wrote_frames * (size_t)g_ao_channels,
                                (g_ao_len_frames - wrote_frames) * (size_t)g_ao_channels * sizeof(int16_t));
                    }
                    g_ao_len_frames -= wrote_frames;
                    space = (g_ao_cap_frames - g_ao_len_frames);
                } else {
                    /* Should be rare with blocking ALSA; if write fails, drop this enqueue */
                    return;
                }
            } else {
                /* Buffer full but empty - should not happen */
                return;
            }
        }
        
        size_t take = left;
        if(take > space) take = space;
        memcpy(g_ao_buf + g_ao_len_frames * (size_t)g_ao_channels,
               p, take * (size_t)g_ao_channels * sizeof(int16_t));
        g_ao_len_frames += take;
        p += take * (size_t)g_ao_channels;
        left -= take;

        /* WSL Performance: Write more aggressively to prevent buffer overflow */
        /* Write when we have at least one period, or when buffer is getting full */
        if(g_ao_len_frames >= g_ao_period_frames || 
           (g_ao_len_frames > 0 && g_ao_len_frames >= g_ao_cap_frames * 7 / 8)){
            ssize_t wrote = alsa_write(g_ao, g_ao_buf, g_ao_len_frames);
            if(wrote > 0){
                size_t wrote_frames = (size_t)wrote;
                if(wrote_frames < g_ao_len_frames){
                    memmove(g_ao_buf, 
                            g_ao_buf + wrote_frames * (size_t)g_ao_channels,
                            (g_ao_len_frames - wrote_frames) * (size_t)g_ao_channels * sizeof(int16_t));
                }
                g_ao_len_frames -= wrote_frames;
            }
        }
    }
}

/* Producer side — called from the decode thread (or the file-input main loop).
 * Never blocks: overflow drops the oldest queued audio instead of stalling the caller. */
static void alsa_client_submit(const int16_t *interleaved_samples, size_t frames){
    if(!g_ao || !g_pcm_ring || frames == 0) return;
    size_t frame_bytes = (size_t)g_ao_channels * sizeof(int16_t);
    spsc_push_dropping(g_pcm_ring, (const uint8_t*)interleaved_samples,
                       frames * frame_bytes, frame_bytes);
}

/* Consumer side — the only thread allowed to call alsa_write/alsa_client_enqueue.
 * Any stall here (device underrun, WSL audio jitter, scheduling delay under load)
 * stays local to this thread and the small g_pcm_ring; it can never propagate back
 * into the UDP ingress / bit-decode path. */
static void *audio_output_thread(void *arg){
    (void)arg;
    uint8_t batch[8 * 1024];
    size_t frame_bytes = (size_t)g_ao_channels * sizeof(int16_t);
    for(;;){
        if(atomic_exchange(&g_audio_flush_requested, 0)){
            g_ao_len_frames = 0;  /* safe: only this thread touches g_ao_buf/g_ao_len_frames */
        }
        size_t n = spsc_pop(g_pcm_ring, batch, sizeof(batch));
        if(n == 0){
            if(g_stop) break;
            spsc_wait_data_or_stopped(g_pcm_ring, SPSC_CONSUMER_IO_WAIT_MS);
            continue;
        }
        alsa_client_enqueue((const int16_t*)batch, n / frame_bytes);
    }
    return NULL;
}

static void alsa_client_close(void){
    if(g_audio_thread_running){
        /* alsa_client_close only runs at shutdown/error paths, after any UDP/file
         * loop has already exited — forcing g_stop here just wakes/drains the
         * audio thread so it can be joined deterministically. */
        g_stop = 1;
        if(g_pcm_ring) pthread_cond_broadcast(&g_pcm_ring->cv);
        pthread_join(g_audio_thread, NULL);
        g_audio_thread_running = 0;
    }
    if(g_pcm_ring){
        spsc_destroy(g_pcm_ring);
        free(g_pcm_ring);
        g_pcm_ring = NULL;
    }
    if(g_ao){
        if(g_ao_len_frames) alsa_write(g_ao, g_ao_buf, g_ao_len_frames);
        alsa_drain(g_ao);
        alsa_close(g_ao);
        g_ao = NULL;
    }
    free(g_ao_buf); g_ao_buf=NULL;
    g_ao_cap_frames=g_ao_period_frames=g_ao_len_frames=0;
}

/* Output helpers */
static inline void output_stereo_upsampled(wav_t *wout, alsa_out_t *ao,
                                           int16_t L, int16_t R, int upsample)
{
    if (upsample > MAX_UPSAMPLE) upsample = MAX_UPSAMPLE;
    if (upsample == 1) {
        g_upsample_stereo[0] = L;
        g_upsample_stereo[1] = R;
    } else {
        for (int i = 0; i < upsample; ++i) {
            g_upsample_stereo[i * 2 + 0] = L;
            g_upsample_stereo[i * 2 + 1] = R;
        }
    }
    if (wout && wout->fp) wav_write_frames(wout, g_upsample_stereo, (size_t)upsample);
    if (ao) alsa_client_submit(g_upsample_stereo, (size_t)upsample);
}

static inline void output_audio_fast(decoder_t *dec, const pa_t *pa,
                                     wav_t *wout, alsa_out_t *ao,
                                     const int16_t pcmA[16], const int16_t pcmB[16],
                                     int upsample)
{
    int16_t L_raw = (cached_Lch < 16) ? pcmA[cached_Lch] : pcmB[cached_Lch - 16];
    int16_t R_raw = cached_is_mono ? L_raw : ((cached_Rch < 16) ? pcmA[cached_Rch] : pcmB[cached_Rch - 16]);
    
    /* DC-Blocker: Remove DC offset to prevent distortion */
    /* Simple high-pass filter: y[n] = x[n] - dc_estimate */
    /* DC estimate is updated with exponential moving average */
    float L_float = (float)L_raw;
    float R_float = (float)R_raw;
    
    g_dc_left = DC_ALPHA * g_dc_left + (1.0f - DC_ALPHA) * L_float;
    g_dc_right = DC_ALPHA * g_dc_right + (1.0f - DC_ALPHA) * R_float;
    
    float L_adj = L_float - g_dc_left;
    float R_adj = R_float - g_dc_right;
    if (L_adj > 32767.0f) L_adj = 32767.0f;
    if (L_adj < -32768.0f) L_adj = -32768.0f;
    if (R_adj > 32767.0f) R_adj = 32767.0f;
    if (R_adj < -32768.0f) R_adj = -32768.0f;
    int16_t L_dc = (int16_t)L_adj;
    int16_t R_dc = (int16_t)R_adj;
    
    /* Glitch reduction: Limit sudden amplitude changes (slew rate limiting) */
    /* This prevents clicks and pops from abrupt transitions.
     * Must do this math in 32-bit: L_dc/g_last_left are int16_t, and a swing between
     * opposite full-scale samples (e.g. +32000 to -32000) differs by up to 65535, which
     * does not fit in int16_t. The old int16_t L_diff silently wrapped such swings back
     * into +/-MAX_SLEW range, so the very jumps this limiter exists to catch slipped
     * through unclamped — and the g_last_left +/- MAX_SLEW clamp target itself could
     * likewise overflow int16_t and wrap to the opposite sign, turning an intended clamp
     * near +32767 into a spurious large negative sample. Both together produced the
     * "overdrive" full-scale spikes/clicks. */
    int32_t L_diff = (int32_t)L_dc - (int32_t)g_last_left;
    int32_t R_diff = (int32_t)R_dc - (int32_t)g_last_right;

    if (L_diff > MAX_SLEW) {
        int32_t v = (int32_t)g_last_left + MAX_SLEW;
        if (v > 32767) v = 32767;
        L_dc = (int16_t)v;
    } else if (L_diff < -MAX_SLEW) {
        int32_t v = (int32_t)g_last_left - MAX_SLEW;
        if (v < -32768) v = -32768;
        L_dc = (int16_t)v;
    }

    if (R_diff > MAX_SLEW) {
        int32_t v = (int32_t)g_last_right + MAX_SLEW;
        if (v > 32767) v = 32767;
        R_dc = (int16_t)v;
    } else if (R_diff < -MAX_SLEW) {
        int32_t v = (int32_t)g_last_right - MAX_SLEW;
        if (v < -32768) v = -32768;
        R_dc = (int16_t)v;
    }
    
    g_last_left = L_dc;
    g_last_right = R_dc;

    if (audio_in_warmup(dec, pa)) {
        return;
    }
    if (AUDIO_WARMUP_SEC > 0.0 && g_audio_warmup_t0_set && !g_audio_warmup_unmute_logged) {
        g_audio_warmup_unmute_logged = 1;
        printf("[audio] playout on (after %.0fs warm-up: decode/PA only before)\n", AUDIO_WARMUP_SEC);
        fflush(stdout);
    }
    output_stereo_upsampled(wout, ao, L_dc, R_dc, upsample);
}

/* After 'r': allow next resync only when Williard + SA cycle + blank offset are all stable. */
static void try_clear_resync_relock(decoder_t *dec, const pa_t *pa)
{
    if (!g_wait_resync_relock || !dec) return;
    if (!decoder_is_williard_locked(dec)) return;
    if (!pa || !pa_sa_cycle_locked(pa) || !pa_is_sa_offset_ready(pa)) return;
    g_wait_resync_relock = 0;
    printf("[resync] lock recovered (will=1 sa=1 offset ready)\n");
    fflush(stdout);
}

/* Receiver loop state (--track / --hackrf) under the [stat] line. */
static void print_rf_stats(void)
{
    if (!g_track_dm) return;
    demod_track_info_t ti;
    demod_get_track_info(g_track_dm, &ti);
    printf("[rf]   level %.1f dBFS | CFO %+.2f kHz | clock %+.1f ppm | EVM %.0f %%",
           ti.level_dbfs, ti.cfo_hz / 1000.0f, ti.clock_ppm, ti.evm * 100.0f);
    if (g_rx) {
        unsigned lna, vga; bool amp;
        hackrf_rx_get_gains(g_rx, &lna, &vga, &amp);
        printf(" | LNA %u VGA %u amp %s | drops %.1f MB", lna, vga, amp ? "on" : "off",
               (double)hackrf_rx_dropped_bytes(g_rx) / (1024.0 * 1024.0));
    }
    printf("\n");
}

/* periodic stats (shared for all sources) */
static void maybe_print_stats(const char *srcname,
                              uint64_t total_bytes,
                              uint64_t total_bits,
                              decoder_t *dec,
                              const pa_t *pa)
{
    /* optional: periodic PA reset window */
    if (pa && (total_bytes - last_pa_reset_bytes) >= (uint64_t)PA_PERIOD_BYTES) {
        bool lk = decoder_is_williard_locked(dec);
        if (lk) {
            /* pa_reset(pa); // re-enable if needed */
        }
        last_pa_reset_bytes = total_bytes;
    }

    try_clear_resync_relock(dec, pa);

    /* every STAT_STEP_BITS bits */
    uint64_t bucket = total_bits / STAT_STEP_BITS;
    if (bucket == last_stat_bucket) return;
    last_stat_bucket = bucket;

    uint64_t sf = decoder_get_superframe_index(dec);
    uint64_t sf_err = decoder_get_superframe_err_count(dec);
    bool     lk = decoder_is_williard_locked(dec);
    bool     sa_locked = pa ? pa_sa_cycle_locked(pa) : false;
    uint64_t bt, bb; decoder_get_stats(dec,&bt,&bb);
    uint64_t bcorr = decoder_get_bch_corrected(dec);
    uint64_t xruns = g_ao ? alsa_get_xruns(g_ao) : 0;

    /* Auto-detect stereo/mono mode once PA data is available and SA-Cycle is locked */
    if (lk && sa_locked && !g_initial_autodetect_done && pa) {
        auto_detect_mode(pa);
        g_initial_autodetect_done = true;
        update_cached_channels(dec);
        print_sel();
    }

    double mb = (double)total_bytes / (1024.0 * 1024.0);
    double mbit = (double)total_bits / 1e6;

    if (sel_runtime_stereo || opt_mono < 0) {
        int Lch, Rch;
        if (sel_runtime_stereo) { int base = pair_even(sel_pair); Lch = base; Rch = base ^ 1; }
        else {
            Lch = (opt_stereo_l>=0)? (opt_stereo_l&31) : 0;
            Rch = (opt_stereo_r>=0)? (opt_stereo_r&31) : 1;
        }
        char psL[64] = "", psR[64] = "";
        int tL=0,mL=0,sL=0, tR=0,mR=0,sR=0;

        if (pa){
            pa_get_name_utf8(pa, Lch, psL, sizeof(psL));
            pa_get_name_utf8(pa, Rch, psR, sizeof(psR));
            pa_get_pa(pa, Lch, &tL,&mL,&sL);
            pa_get_pa(pa, Rch, &tR,&mR,&sR);
        }

        bool same_label = (strcmp(psL, psR) == 0);
        if (same_label && psL[0]) {
            printf("[stat] src=%s %.1f MB | %.2f Mbit | "
                "BCH ok=%" PRIu64 " bad=%" PRIu64 " corr=%" PRIu64 " | "
                "L=%02d R=%02d \"%s\" | SF=%" PRIu64 " SF_ERR=%" PRIu64 " will=%d sa=%d",
                srcname, mb, mbit,
                (uint64_t)(bt - bb), (uint64_t)bb, bcorr,
                Lch, Rch, psL,
                (uint64_t)sf, (uint64_t)sf_err, lk?1:0, sa_locked?1:0);
            if (xruns > 0) printf(" xruns=%" PRIu64, xruns);
            printf("\n");
        } else {
            printf("[stat] src=%s %.1f MB | %.2f Mbit | "
                "BCH ok=%" PRIu64 " bad=%" PRIu64 " corr=%" PRIu64 " | "
                "L=%02d \"%s\" R=%02d \"%s\" | SF=%" PRIu64 " SF_ERR=%" PRIu64 " will=%d sa=%d",
                srcname, mb, mbit,
                (uint64_t)(bt - bb), (uint64_t)bb, bcorr,
                Lch, psL[0]?psL:"?", Rch, psR[0]?psR:"?",
                (uint64_t)sf, (uint64_t)sf_err, lk?1:0, sa_locked?1:0);
            if (xruns > 0) printf(" xruns=%" PRIu64, xruns);
            printf("\n");
        }

    } else {
        int ch = opt_mono & 31;
        char ps[64] = ""; int t=0,m=0,s=0;
        if (pa){
            pa_get_name_utf8(pa, ch, ps, sizeof(ps));
            pa_get_pa(pa, ch, &t,&m,&s);
        }

        printf("[stat] src=%s %.1f MB | %.1f Mbit | "
            "BCH ok=%" PRIu64 " bad=%" PRIu64 " corr=%" PRIu64 " | "
            "CH=%02d \"%s\" | SF=%" PRIu64 " SF_ERR=%" PRIu64 " will=%d sa=%d",
            srcname, mb, mbit,
            (uint64_t)(bt - bb), (uint64_t)bb, bcorr,
            ch, ps[0]?ps:"?",
            (uint64_t)sf, (uint64_t)sf_err, lk?1:0, sa_locked?1:0);
        if (xruns > 0) printf(" xruns=%" PRIu64, xruns);
        printf("\n");
    }
    if (pa) {
        pa_status_t ps;
        pa_get_status(pa, &ps);
        printf("[pa]   station list: %d names, %d attributes confirmed | %s | realigns %u\n",
               ps.names, ps.attrs,
               ps.aligned ? "aligned" : (ps.names || ps.attrs) ? "FROZEN (SA realign)" : "acquiring",
               ps.realigns);
    }
    print_rf_stats();
    fflush(stdout);
}

static void print_channel_table(const pa_t *pa)
{
    printf("\n");
    printf("╔════╤══════════╤════════╤═══════╤════════════════╗\n");
    printf("║ Ch │ Label    │ Mode   │ M / S │ Genre          ║\n");
    printf("╠════╪══════════╪════════╪═══════╪════════════════╣\n");

    int modes[32] = {0};
    int musics[32] = {0};
    for (int ch = 0; ch < 32; ++ch) {
        if (pa) {
            int t, m, mode;
            pa_get_pa(pa, ch, &t, &m, &mode);
            modes[ch] = mode;
            musics[ch] = m;
        }
    }

    for (int ch = 0; ch < 32; ++ch) {
        char name[64] = "";
        int type = 0, music = 0, mode = 0;

        if (pa) {
            pa_get_name_utf8(pa, ch, name, sizeof(name));
            pa_get_pa(pa, ch, &type, &music, &mode);
        }

        char label[12];
        snprintf(label, sizeof(label), "%-8.8s", name[0] ? name : "--------");

        const char *mode_str;
        int is_even = (ch & 1) == 0;
        int partner_mode = is_even ? modes[ch + 1] : modes[ch - 1];
        int is_stereo = 0;

        if (mode == 2) {
            mode_str = "Stereo";
            is_stereo = 1;
        } else if (mode == 1 && is_even && partner_mode == 2) {
            mode_str = "Stereo";
            is_stereo = 1;
        } else if (mode == 1) {
            mode_str = "Mono  ";
        } else {
            mode_str = "------";
        }

        int display_music = music;
        if (is_stereo && !is_even) {
            display_music = musics[ch - 1];
        }

        const char *genre = pa_type_names[type & 0xF];

        printf("║ %2d │ %s │ %-6s │ %c / %c │ %-14.14s ║\n",
               ch, label, mode_str,
               display_music ? 'M' : '-',
               display_music ? '-' : 'S',
               genre);
    }

    printf("╚════╧══════════╧════════╧═══════╧════════════════╝\n");
    printf("M=Music, S=Speech\n\n");
    fflush(stdout);
}

static void hard_resync_all(decoder_t *dec, pa_t *pa, int *parity_io, int udp_fd)
{
    long since_last_ms = 0;
    if (g_last_resync_mono.tv_sec != 0 || g_last_resync_mono.tv_nsec != 0) {
        if (elapsed_ms_since(&g_last_resync_mono, &since_last_ms) &&
            since_last_ms < RESYNC_MIN_INTERVAL_MS) {
            printf("[resync] ignored (cooldown %ld/%d ms)\n",
                   since_last_ms, RESYNC_MIN_INTERVAL_MS);
            fflush(stdout);
            return;
        }
    }
    if (g_ingest_paused) {
        printf("[resync] ignored (resync already in progress)\n");
        fflush(stdout);
        return;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &g_last_resync_mono);
    size_t dropped_udp = flush_udp_backlog(udp_fd);
    if (g_ingress_spsc) {
        spsc_reset(g_ingress_spsc);
    }
    if (g_rx) hackrf_rx_flush(g_rx);
    if (g_track_dm) demod_reset(g_track_dm);
    decoder_reset_sync(dec);
    if (pa) pa_reset(pa);
    if (parity_io) *parity_io = 0;
    g_dc_left = 0.0f;
    g_dc_right = 0.0f;
    g_last_left = 0;
    g_last_right = 0;
    audio_request_flush();
    /* keep current manual/auto mode selection stable across resync */
    last_stat_bucket = UINT64_MAX;
    last_pa_reset_bytes = 0;
    g_wait_resync_relock = 1;
    arm_ingest_pause_ms(RESYNC_INGEST_PAUSE_MS);
    audio_warmup_restart_after_resync();
    if (dropped_udp > 0) {
        printf("[resync] cleared decoder+PA+ALSA staging; dropped %zu queued UDP bytes; ingress paused %d ms\n",
               dropped_udp, RESYNC_INGEST_PAUSE_MS);
    } else {
        printf("[resync] cleared decoder+PA+ALSA staging; ingress paused %d ms for cold sync\n",
               RESYNC_INGEST_PAUSE_MS);
    }
    fflush(stdout);
}

static void print_key_help(void)
{
    printf("\n");
    printf("--- Keyboard commands (runtime) ---\n");
    printf("  u / d     Channel up / down (stereo pair or mono channel)\n");
    printf("  m         Force mono mode (current channel)\n");
    printf("  s         Force stereo mode\n");
    printf("  v         Cycle decoder log level: off -> sync -> warn -> zi\n");
    printf("  t         Show channel table (all 32 subchannels)\n");
    printf("  p         Toggle PA debug (raw SA bytes on stderr)\n");
    printf("  r         Hard resync (clears state; next r blocked until will+sa+offset lock)\n");
    printf("  q         Quit\n");
    printf("  h         Show this help\n");
    printf("---------------------------------\n\n");
    fflush(stdout);
}

static void handle_keypress(char c, pa_t *pa, decoder_t *dec, int *parity_io, int udp_fd){
    if (c == 'r' || c == 'R') {
        if (g_wait_resync_relock) {
            printf("[resync] ignored (waiting for lock recovery)\n");
            fflush(stdout);
            return;
        }
        hard_resync_all(dec, pa, parity_io, udp_fd);
    } else if(c=='u'){
        if (sel_runtime_stereo) {
            pair_up();
        } else {
            opt_mono = (opt_mono + 1) & 31;
            sel_pair = pair_even(opt_mono);
        }
        /* Don't auto-detect mode on manual channel change - keep current mode */
        update_cached_channels(dec);
        print_sel();
        if (g_ao) audio_request_flush();
        print_prog_for_current(pa);
    } else if(c=='d'){
        if (sel_runtime_stereo) {
            pair_down();
        } else {
            opt_mono = (opt_mono + 31) & 31;
            sel_pair = pair_even(opt_mono);
        }
        /* Don't auto-detect mode on manual channel change - keep current mode */
        update_cached_channels(dec);
        print_sel();
        if (g_ao) audio_request_flush();
        print_prog_for_current(pa);
    } else if(c=='v'){
        static const dsr_log_level_t cycle[] = {
            DSR_LOG_NONE, DSR_LOG_SYNC, DSR_LOG_WARN, DSR_LOG_ZI
        };
        int idx = 0;
        for(; idx < 4; idx++){
            if(cycle[idx] == opt_log_level) break;
        }
        if(idx >= 4) idx = 0;
        opt_log_level = cycle[(idx + 1) % 4];
        decoder_set_log_level(dec, opt_log_level);
        printf("[log] %s\n", log_level_name(opt_log_level));
    } else if (c == 's') {
        sel_runtime_stereo = true;
        opt_mono = -1;
        update_cached_channels(dec);
        printf("[manual] stereo mode forced\n");
        print_sel();
        print_prog_for_current(pa);
    } else if (c == 'm') {
        opt_mono = pair_even(sel_pair);
        sel_runtime_stereo = false;
        update_cached_channels(dec);
        printf("[manual] mono mode forced, ch=%d\n", opt_mono);
        print_prog_for_current(pa);
    } else if(c=='q'){
        g_stop = 1;
    } else if(c=='t'){
        print_channel_table(pa);
    } else if(c=='p'){
        static int pa_debug_on = 0;
        pa_debug_on = !pa_debug_on;
        pa_enable_debug(pa_debug_on);
        printf("[PA debug] %s - watch stderr for raw PA bytes\n", pa_debug_on ? "ON" : "OFF");
    } else if(c=='h' || c=='H'){
        print_key_help();
    }
}

int main(int argc, char **argv){
    setvbuf(stderr, NULL, _IONBF, 0);

    if(parse_args(argc,argv)<0) return 1;
    signal(SIGINT,on_sigint);
    print_logo();

    enum {SRC_NONE=0, SRC_UDP, SRC_FILEBITS, SRC_IQ, SRC_HACKRF} src = SRC_NONE;
    if (opt_hackrf)        src = SRC_HACKRF;
    else if (opt_iqfile)   src = SRC_IQ;
    else if (opt_infile)   src = SRC_FILEBITS;
    else                   src = SRC_UDP;

    if (src == SRC_HACKRF) {
        printf("HackRF input %.3f MHz fs=%u sym=%u (demod built-in, tracking) | descramble=%s | BCH=%s | auto-deint=%s\n",
               (double)opt_rx.freq_hz / 1e6, opt_rx.sample_rate, DSR_SYMBOL_RATE_HZ,
               opt_descramble?"on":"off",
               opt_bch_check?"on":"off",
               opt_no_auto_deint?"off":"on");
        if(sel_runtime_stereo) print_sel();
    } else if (src == SRC_IQ) {
        printf("IQ input file=%s fmt=%s fs=%u sym=%u (demod built-in%s) | descramble=%s | BCH=%s | auto-deint=%s\n",
               opt_iqfile, opt_iqfmt, opt_iqrate, opt_track ? DSR_SYMBOL_RATE_HZ : opt_iqrate/2,
               opt_track ? ", tracking" : "",
               opt_descramble?"on":"off",
               opt_bch_check?"on":"off",
               opt_no_auto_deint?"off":"on");
        if(sel_runtime_stereo) print_sel();
    } else if (src == SRC_FILEBITS) {
        printf("FILE input | bit-order=%s | descramble=%s | BCH=%s | auto-deint=%s\n",
               opt_msb_first?"MSB->LSB":"LSB->MSB",
               opt_descramble?"on":"off",
               opt_bch_check?"on":"off",
               opt_no_auto_deint?"off":"on");
        if(sel_runtime_stereo) print_sel();
    } else {
        printf("UDP input (2 threads: UDP->ring, consumer->decode) | ring=%u KiB min-prefill=%d B | bit-order=%s | descramble=%s | BCH=%s | auto-deint=%s\n",
               (unsigned)(SPSC_CAP_BYTES / 1024u), SPSC_MIN_PREFILL_BYTES,
               opt_msb_first?"MSB->LSB":"LSB->MSB",
               opt_descramble?"on":"off",
               opt_bch_check?"on":"off",
               opt_no_auto_deint?"off":"on");
        if(sel_runtime_stereo) print_sel();
        printf("Listen %s:%d\n", opt_host,opt_port);
    }

    int fd = -1;
    FILE *fp = NULL;
    demod_t *dm = NULL;
    hackrf_rx_t *rx = NULL;
    if (src == SRC_IQ || src == SRC_HACKRF) {
        if (src == SRC_IQ) {
            fp = fopen(opt_iqfile, "rb");
            if(!fp){ perror("fopen iqfile"); return 2; }
        }
        demod_cfg_t dmcfg = {
            .fs_hz            = opt_iqrate,
            .sym_hz           = opt_track ? DSR_SYMBOL_RATE_HZ : opt_iqrate / 2,
            .rrc_beta         = 0.50f,
            .rrc_span         = 8,
            .fmt              = (!strcmp(opt_iqfmt,"f32le") ? DEMOD_FMT_F32LE :
                                 !strcmp(opt_iqfmt,"s8")    ? DEMOD_FMT_S8 : DEMOD_FMT_S16LE),
            .agc_ref          = 0.7f,
            .agc_tau          = 0.05f,
            .track            = opt_track ? 1 : 0,
        };
        dm = demod_create(&dmcfg);
        if(!dm){ fprintf(stderr,"demod_create failed\n"); if(fp) fclose(fp); return 2; }
        if (opt_track) g_track_dm = dm;
        if (src == SRC_HACKRF) {
            char err[256];
            rx = hackrf_rx_open(&opt_rx, err, sizeof(err));
            if (!rx) { fprintf(stderr, "[hackrf] %s\n", err); demod_free(dm); return 2; }
            g_rx = rx;
        }
    } else if (src == SRC_FILEBITS) {
        if(strcmp(opt_infile,"-")==0) fp = stdin;
        else {
            fp = fopen(opt_infile, "rb");
            if(!fp){ perror("fopen infile"); return 2; }
        }
    } else if (src == SRC_UDP) {
        fd = udp_bind_socket(opt_host,opt_port);
        if(fd<0) return 2;
    }

    decoder_opts_t dopts = {
        .descramble = opt_descramble,
        .bch_check  = opt_bch_check,
        .log_level  = opt_log_level,
        .auto_deint = !opt_no_auto_deint
    };
    decoder_t *dec = decoder_new(&dopts);
    if(!dec) { fprintf(stderr,"decoder_new failed\n"); return 3; }
    update_cached_channels(dec);

    pa_t *pa = pa_new();
    if(pa) decoder_attach_pa(dec, pa);

    wav_t wout={0};
    /* Always stereo: mono output is sent as L=R, and m/s/auto-detect can switch the
     * mode at runtime, so the channel count must not depend on the start mode. */
    int wav_channels = 2;
    int alsa_channels = 2;
    if(opt_wav) {
        if (wav_open(&wout,opt_wav,wav_channels,opt_rate)<0) {
            fprintf(stderr,"[wav] open failed\n");
        } else {
            printf("[wav] writing %s at %d Hz, channels=%d\n", opt_wav,opt_rate,wav_channels);
        }
    }
    if(opt_audioout) alsa_client_open((unsigned)opt_rate,alsa_channels);

    /* Open /dev/tty for keyboard input if STDIN is a pipe (not a terminal) */
    if (!isatty(STDIN_FILENO)) {
        tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK);
        if (tty_fd < 0) {
            /* If /dev/tty is not available, disable keyboard input */
            tty_fd = -1;
        } else {
            /* Set terminal to raw mode for /dev/tty */
            struct termios tty_attr;
            tcgetattr(tty_fd, &oldt);
            tty_attr = oldt;
            tty_attr.c_lflag &= ~(ICANON|ECHO);
            tcsetattr(tty_fd, TCSANOW, &tty_attr);
        }
    } else {
        /* STDIN is a terminal, use it for keyboard input */
        tty_fd = STDIN_FILENO;
        tcgetattr(STDIN_FILENO, &oldt);
        tty_raw();
    }
    set_nonblock(STDIN_FILENO);
    if (src == SRC_UDP) set_nonblock(fd);

    if (src == SRC_IQ || src == SRC_HACKRF) {
        const size_t CHUNK = 65536u; /* IQ pairs per fread, matches the old dsrdemod chunk size */
        size_t iq_item_size = (!strcmp(opt_iqfmt,"f32le") ? sizeof(float) :
                               !strcmp(opt_iqfmt,"s8")    ? sizeof(int8_t) : sizeof(int16_t));
        /* track: fs may be below 2*sym (HackRF at 20 MS/s) -> slightly more bits than samples */
        const size_t BITCAP = 2 * CHUNK;
        void *iqbuf = malloc(CHUNK * 2 * iq_item_size);
        uint8_t *bitbuf = (uint8_t*)malloc(BITCAP);
        if(!iqbuf || !bitbuf){
            fprintf(stderr, "[dsrdec] oom (iq buffers)\n");
            free(iqbuf); free(bitbuf);
            if(rx) hackrf_rx_close(rx);
            demod_free(dm);
            if(fp) fclose(fp);
            if(pa) pa_free(pa);
            decoder_free(dec);
            if(opt_wav) wav_close(&wout);
            if(opt_audioout) alsa_client_close();
            return 5;
        }

        int parity = 0;
        uint64_t total_bits = 0;   /* demodulated bitstream bits: drives stat cadence */
        uint64_t total_bytes = 0;  /* raw IQ bytes consumed: shown as "MB" */
        unsigned pace_pending = 0;
        file_pace_reset();
        uint64_t acq_samples = 0;   /* track: IQ samples since last lock (or acquisition start) */
        uint64_t rx_drops_seen = 0;

        while (!g_stop) {
            audio_warmup_arm_once();
            size_t got;
            if (opt_track) {
                demod_track_info_t ti;
                demod_get_track_info(dm, &ti);
                if (rx && hackrf_rx_auto_gain(rx, ti.level_dbfs)) {
                    unsigned lna, vga; bool amp;
                    hackrf_rx_get_gains(rx, &lna, &vga, &amp);
                    if (opt_log_level >= DSR_LOG_SYNC)
                        fprintf(stderr, "[hackrf] auto gain: level %.1f dBFS -> LNA %u dB, VGA %u dB\n",
                                ti.level_dbfs, lna, vga);
                }
                /* Williard alone is no proof: on garbage bits it locks on a single
                 * chance 16-bit match and drops out again 24 checks later. */
                if (decoder_is_williard_locked(dec) && pa && pa_sa_cycle_locked(pa)) {
                    acq_samples = 0;
                } else if (acq_samples > (uint64_t)ACQ_RETRY_SEC * opt_iqrate) {
                    /* Nothing locked: the auto-map may have been decided on noise or on a
                     * level transient. Start acquisition over (strobe, loops, auto-map). */
                    if (rx) printf("[hackrf] no DSR mux locked at %.3f MHz", (double)opt_rx.freq_hz / 1e6);
                    else    printf("[track] no DSR mux locked");
                    printf(" (level %.1f dBFS, EVM %.0f %%) - retrying\n", ti.level_dbfs, ti.evm * 100.0f);
                    fflush(stdout);
                    if (rx) hackrf_rx_flush(rx);
                    demod_reset(dm);
                    decoder_reset_sync(dec);
                    if (pa) pa_reset(pa);
                    parity = 0;
                    audio_warmup_reset();
                    acq_samples = 0;
                }
            }
            if (rx) {
                got = hackrf_rx_read(rx, (int8_t *)iqbuf, CHUNK, 200);
                if (got == (size_t)-1) { fprintf(stderr, "[hackrf] device stopped streaming\n"); break; }
                /* A USB overflow cuts a hole into the bitstream; PA would keep a shifted
                 * SA assembly (garbled labels), so rebuild everything like key 'r'. */
                uint64_t drops = hackrf_rx_dropped_bytes(rx);
                if (drops != rx_drops_seen) {
                    printf("[hackrf] %.1f MB dropped (USB/CPU too slow) - resync\n",
                           (double)(drops - rx_drops_seen) / (1024.0 * 1024.0));
                    rx_drops_seen = drops;
                    g_last_resync_mono.tv_sec = 0; g_last_resync_mono.tv_nsec = 0;  /* skip cooldown */
                    hard_resync_all(dec, pa, &parity, -1);
                    acq_samples = 0;
                    continue;
                }
                if (got == 0) {
                    char c;
                    if (tty_fd >= 0 && read(tty_fd, &c, 1) == 1) handle_keypress(c, pa, dec, &parity, -1);
                    continue;
                }
            } else {
                got = fread(iqbuf, 2*iq_item_size, CHUNK, fp);
            }
            if (got == 0) {
                if (feof(fp)) {
                    if (opt_repeat) {
                        if (opt_log_level >= DSR_LOG_SYNC)
                            fprintf(stderr, "[repeat] EOF -> rewind IQ file; counters reset\n");
                        clearerr(fp);
                        rewind(fp);
                        demod_reset(dm);
                        total_bytes = 0;
                        total_bits = 0;
                        pace_pending = 0;
                        file_pace_reset();
                        audio_warmup_reset();
                        if (pa) pa_reset(pa);
                        continue;
                    }
                    break;
                } else if (ferror(fp)) {
                    perror("fread iqfile");
                    break;
                }
                continue;
            }
            total_bytes += got * 2 * iq_item_size;
            acq_samples += got;

            size_t nbits = demod_process(dm, iqbuf, got, bitbuf, BITCAP);

            if (ingest_allowed()) {
                /* Block-wise instead of bit by bit: decoder_push_ab_bits() runs until the
                 * next complete A/B pair (~640 bits on average), saving the
                 * per-bit function call. */
                int16_t pcmA[16], pcmB[16];
                size_t k = 0;
                while (k < nbits) {
                    bool emitted = false;
                    k += decoder_push_ab_bits(dec, bitbuf + k, nbits - k, &parity,
                                              pcmA, pcmB, &emitted);
                    if (emitted) {
                        output_audio_fast(dec, pa, &wout, g_ao, pcmA, pcmB, opt_upsample);
                        if (opt_audioout && !rx) {   /* live RF paces itself */
                            pace_pending += (unsigned)opt_upsample;
                            if (pace_pending >= 512) {
                                file_pace_tick(pace_pending, (unsigned)opt_rate);
                                pace_pending = 0;
                            }
                        }
                    }
                }
                total_bits += nbits;
            }

            maybe_print_stats(rx ? "hackrf" : "iq", total_bytes, total_bits, dec, pa);

            char c;
            if (tty_fd >= 0 && read(tty_fd, &c, 1) == 1) {
                handle_keypress(c, pa, dec, &parity, -1);
            }
        }
        free(iqbuf);
        free(bitbuf);
        g_rx = NULL;
        g_track_dm = NULL;
        if (rx) hackrf_rx_close(rx);
        demod_free(dm);
    } else if (src == SRC_FILEBITS) {
        uint8_t rxbuf[256*1024];
        int parity = 0;
        uint64_t total_bits = 0;
        uint64_t total_bytes = 0;
        unsigned pace_pending = 0;
        file_pace_reset();

        while (!g_stop) {
            audio_warmup_arm_once();
            size_t nread = fread(rxbuf, 1, sizeof(rxbuf), fp);
            if (nread == 0) {
                if (feof(fp)) {
                    if (opt_repeat && fp && fp != stdin) {
                        if (opt_log_level >= DSR_LOG_SYNC)
                            fprintf(stderr, "[repeat] EOF -> rewind file; counters reset\n");
                        clearerr(fp);
                        rewind(fp);
                        total_bytes = 0;
                        total_bits = 0;
                        pace_pending = 0;
                        file_pace_reset();
                        audio_warmup_reset();
                        if (pa) pa_reset(pa);
                        continue;
                    }
                    break;
                } else if (ferror(fp)) {
                    if (fp == stdin && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        clearerr(fp);
                        struct timespec w = { .tv_sec = 0, .tv_nsec = 1000000L };
                        (void)nanosleep(&w, NULL);
                        continue;
                    }
                    perror("fread");
                    break;
                }
                continue;
            }
            total_bytes += nread;

            if (ingest_allowed()) {
                for (size_t i = 0; i < nread; i++) {
                    int16_t pcmA[16], pcmB[16];
                    if (decoder_push_ab_u8(dec, rxbuf[i], opt_msb_first, &parity, pcmA, pcmB)) {
                        output_audio_fast(dec, pa, &wout, g_ao, pcmA, pcmB, opt_upsample);
                        if (opt_audioout) {
                            pace_pending += (unsigned)opt_upsample;
                            if (pace_pending >= 512) {
                                file_pace_tick(pace_pending, (unsigned)opt_rate);
                                pace_pending = 0;
                            }
                        }
                    }
                    total_bits += 8;
                }
            }

            maybe_print_stats("file", total_bytes, total_bits, dec, pa);

            char c;
            if (tty_fd >= 0 && read(tty_fd, &c, 1) == 1) {
                handle_keypress(c, pa, dec, &parity, -1);
            }
        }
    } else { /* SRC_UDP: ingress thread (recv -> SPSC ring) + consumer (ring -> decoder -> WAV/ALSA) */
        spsc_bytes_t ring;
        if (spsc_init(&ring, SPSC_CAP_BYTES) != 0) {
            fprintf(stderr, "SPSC ring alloc failed\n");
            if (fd >= 0) close(fd);
            if (pa) pa_free(pa);
            decoder_free(dec);
            if (opt_wav) wav_close(&wout);
            if (opt_audioout) alsa_client_close();
            if (tty_fd >= 0) {
                if (tty_fd != STDIN_FILENO) {
                    tcsetattr(tty_fd, TCSANOW, &oldt);
                    close(tty_fd);
                } else {
                    tty_restore();
                }
            }
            return 3;
        }

        atomic_store(&g_ingress_bytes, 0);
        g_ingress_spsc = &ring;
        io_threads_ctx_t uctx = {
            .ring = &ring,
            .udp_fd = fd,
            .fp = NULL,
            .dec = dec,
            .pa = pa,
            .wout = &wout,
            .stat_src = "udp",
            .file_repeat_seq = 0,
        };

        pthread_t th_ing, th_con;
        if (pthread_create(&th_ing, NULL, udp_ingress_thread, &uctx) != 0) {
            perror("pthread_create ingress");
            g_ingress_spsc = NULL;
            spsc_destroy(&ring);
            if (fd >= 0) close(fd);
            if (pa) pa_free(pa);
            decoder_free(dec);
            if (opt_wav) wav_close(&wout);
            if (opt_audioout) alsa_client_close();
            if (tty_fd >= 0) {
                if (tty_fd != STDIN_FILENO) {
                    tcsetattr(tty_fd, TCSANOW, &oldt);
                    close(tty_fd);
                } else {
                    tty_restore();
                }
            }
            return 3;
        }

        if (pthread_create(&th_con, NULL, decode_consumer_thread, &uctx) != 0) {
            perror("pthread_create consumer");
            g_stop = 1;
            pthread_cond_broadcast(&ring.cv);
            (void)pthread_join(th_ing, NULL);
            g_ingress_spsc = NULL;
            spsc_destroy(&ring);
            if (fd >= 0) close(fd);
            if (pa) pa_free(pa);
            decoder_free(dec);
            if (opt_wav) wav_close(&wout);
            if (opt_audioout) alsa_client_close();
            if (tty_fd >= 0) {
                if (tty_fd != STDIN_FILENO) {
                    tcsetattr(tty_fd, TCSANOW, &oldt);
                    close(tty_fd);
                } else {
                    tty_restore();
                }
            }
            return 3;
        }

        (void)pthread_join(th_con, NULL);
        (void)pthread_join(th_ing, NULL);
        g_ingress_spsc = NULL;
        spsc_destroy(&ring);
    }


    if(opt_wav) wav_close(&wout);
    if(opt_audioout) alsa_client_close();
    if(fp && fp!=stdin) fclose(fp);
    if(fd>=0) close(fd);
    if(pa) pa_free(pa);
    decoder_free(dec);
    if (tty_fd >= 0) {
        if (tty_fd != STDIN_FILENO) {
            /* Restore /dev/tty terminal settings */
            tcsetattr(tty_fd, TCSANOW, &oldt);
            close(tty_fd);
        } else {
            /* Restore STDIN terminal settings */
            tty_restore();
        }
    }
    return 0;
}
