// demod.c — IQ → Bits (Diff‑QPSK, SPS=2, RRC matched, AGC, Auto‑Map)
// track=1 adds a receiver path for live RF / SDR recordings: DC blocker, Gardner symbol
// timing recovery with cubic interpolation (any fs ~2*sym) and a QPSK carrier loop.
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79

#include "demod.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* AGC gain is recomputed every N samples to avoid per-sample division */
#define AGC_UPDATE_INTERVAL 64

typedef struct { float i, q; } cpx;

typedef struct {
    float *taps; int ntaps;
    /* zi_i/zi_q are doubled-length ring buffers (2*ntaps): each incoming sample is
     * written at both zi_pos and zi_pos+ntaps, so the ntaps most recent samples are
     * always readable as one contiguous, branch-free window (see rrc_store). */
    float *zi_i, *zi_q; int zi_len, zi_pos;
    /* Polyphase split of taps for the decimate-by-2 block path: te = taps[0,2,4,...],
     * to = taps[1,3,5,...]. ntaps is odd, so nte = ntaps/2+1 and nto = ntaps/2. */
    float *te, *to; int nte, nto;
} rrc_filt_t;

struct demod_s {
    demod_cfg_t C;

    /* Fixed SPS=2 path */
    double  sps;                 /* always 2.0 */
    int64_t sample_idx;          /* absolute sample index */
    int     strobe_ofs;          /* 0 or 1 */
    int     strobe_locked;       /* 0=still searching, 1=found */
    /* Strobe-offset search accumulators. Must be per instance:
     * as function-level statics they were never cleared by
     * demod_reset() (--repeat), so the second pass "locked" immediately
     * using the statistics of the first instead of searching again. */
    double  strobe_acc[2];
    int64_t strobe_cnt[2];

    /* RRC */
    rrc_filt_t rrc;

    /* Diff‑QPSK state */
    int  prev_q;
    int  have_prev_q;

    /* Debug/Format */
    demod_iqfmt_t fmt;

    /* --- AGC (leaky envelope) --- */
    float agc_env;    /* envelope */
    float agc_alpha;  /* 0..1 (filter coeff) */
    float agc_ref;    /* target level (e.g. 0.7) */
    float agc_gain;   /* Cached gain (updated every AGC_UPDATE_INTERVAL samples) */
    int   agc_counter;/* Samples since last gain update */
    /* Closed form of the envelope over an entire run, see demod_fast_block:
     * agc_pow[m] = alpha^m,  agc_wrev[j] = alpha^(AGC_UPDATE_INTERVAL-2-j). */
    float agc_pow[AGC_UPDATE_INTERVAL+1];
    float agc_wrev[AGC_UPDATE_INTERVAL-1];

    /* --- Auto‑mapping of DPSK phase rotation & bit order --- */
    int   auto_done;          /* 0=probing, 1=fixed */
    int   sel_rot;            /* 0..3: phase rotation */
    int   sel_lsb_first;      /* 0=MSB->LSB, 1=LSB->MSB */
    int   sel_swap_hint;      /* 0=normal A/B, 1=swapped (info only) */

    /* We buffer the first few‑k increments (inc = (q - prev_q) & 3) */
    uint8_t *inc_buf;
    size_t   inc_cap;
    size_t   inc_len;

    /* --- Scratch for the block path (demod_fast_block), allocated once --- */
    float *sc_bi, *sc_bq;      /* (ntaps-1 + TILE) post-AGC samples, contiguous      */
    float *sc_ei, *sc_eq;      /* even parity of sc_b* (Polyphase E)                 */
    float *sc_oi, *sc_oq;      /* odd parity of sc_b* (Polyphase O)                  */
    float *sc_yi, *sc_yq;      /* filter output per symbol                           */
    int    sc_ready;

    /* --- Tracking path (track=1, see demod_track_process) --- */
    float  *tr_xi, *tr_xq;     /* ntaps-1 history + DEMOD_TILE post-AGC samples      */
    float  *tr_fi, *tr_fq;     /* TRK_HIST history + DEMOD_TILE matched-filter output */
    float   dc_i, dc_q;        /* DC blocker state (HackRF LO leakage spike)          */
    float   tr_dc_pow;         /* (1-TRK_DC_ALPHA)^AGC_UPDATE_INTERVAL                */
    double  tr_sps;            /* nominal samples per symbol, fs/sym                  */
    double  tr_pos;            /* last on-time strobe, index into tr_f*               */
    double  tr_rate;           /* timing loop integrator, samples/symbol              */
    float   tr_kp, tr_ki;      /* timing loop gains                                   */
    float   tr_yi, tr_yq;      /* previous on-time sample                             */
    int     tr_have_prev;
    float   tr_pow;            /* running symbol power (Gardner normalization)        */
    int64_t tr_settle;         /* symbols still discarded while the loops pull in     */
    float   cl_c, cl_s;        /* carrier phasor exp(j*theta)                         */
    float   cl_freq;           /* carrier frequency estimate, rad/symbol              */
    float   cl_kp, cl_ki;      /* carrier loop gains                                  */
    float   evm_amp, evm_err;  /* running mean amplitude / squared error per axis     */
};

/* Block-path tile size: keeps all scratch arrays together in L2. */
#define DEMOD_TILE 16384

/* Vector type for the FIR kernel. GCC/clang map 64 bytes onto AVX-512 and otherwise
 * automatically split into several narrower registers -- correct on any target
 * architecture, and with -march=native exactly as wide as the CPU supports. dvecu_t is
 * the same type with alignment 4, i.e. for unaligned accesses. */
#if defined(__GNUC__)
  #define DEMOD_HAVE_VEC 1
  #define DV 16
  typedef float dvec_t  __attribute__((vector_size(DV*sizeof(float))));
  typedef float dvecu_t __attribute__((vector_size(DV*sizeof(float)), aligned(sizeof(float))));
#else
  #define DEMOD_HAVE_VEC 0
#endif

/* ----------------- Helper functions ----------------- */

static inline float clipf(float x, float lim){ return x>lim?lim:(x<-lim?-lim:x); }

/* Quadrant 0..3, compatible with the modulator (rf.c):
   0=(-,-), 1=(-,+), 2=(+,+), 3=(+,-)
   Branch-free via a 4-entry table: with noise or payload data both signs are
   practically random; two unpredictable branches per symbol cost more in this
   path (one symbol per two input samples) than the actual computation.
   Rechnung. Index = (i<0)<<1 | (q<0). */
static inline int decide_quadrant(cpx z)
{
    static const uint8_t qlut[4] = { 2, 3, 1, 0 };
    return qlut[ (unsigned)((z.i < 0.0f) ? 2 : 0) | (unsigned)((z.q < 0.0f) ? 1 : 0) ];
}

/* Root‑Raised‑Cosine: β, span in symbols, SPS=2
   Taps with Hamming window, normalized to unit sum. */
static int rrc_make(rrc_filt_t *f, double beta, int span_sym, double sps)
{
    if (beta <= 0.0) beta = 0.25;        /* safe defaults */
    if (span_sym <= 0) span_sym = 8;

    const int ntaps = ((int)floor(span_sym * sps + 0.5)) | 1; /* odd */
    f->ntaps = ntaps;
    f->taps  = (float*)calloc((size_t)ntaps, sizeof(float));
    f->zi_i  = (float*)calloc((size_t)ntaps * 2u, sizeof(float));
    f->zi_q  = (float*)calloc((size_t)ntaps * 2u, sizeof(float));
    f->zi_len= ntaps; f->zi_pos=0;
    if(!f->taps||!f->zi_i||!f->zi_q) return -1;

    const double B = beta;
    const double T = 1.0;       /* symbol period */
    const double Ts= 1.0 / sps; /* sample period */

    int m = ntaps/2;
    double sum = 0.0;
    for (int n=-m; n<=m; ++n) {
        double t = n * Ts;
        double x;
        if (fabs(t) < 1e-12) {
            x = (1.0/T) * (1.0 + B*(4.0/M_PI - 1.0));
        } else if (B>0.0 && fabs(fabs(t) - T/(4.0*B)) < 1e-9) {
            x = B/(T*sqrt(2.0)) *
                ((1.0 + 2.0/M_PI)*sin(M_PI/(4.0*B)) + (1.0 - 2.0/M_PI)*cos(M_PI/(4.0*B)));
        } else {
            double num = sin(M_PI*(1.0 - B)*t/T) + 4.0*B*t/T * cos(M_PI*(1.0 + B)*t/T);
            double den = M_PI*t/T * (1.0 - 16.0*B*B*t*t/(T*T));
            x = (1.0/T) * (num/den);
        }
        /* Hamming window (0..1 over the tap length) */
        double xn = (double)(n+m) / (double)(2*m);
        double w  = 0.54 - 0.46 * cos(2.0*M_PI*xn);
        double xh = x * w;

        f->taps[n+m] = (float)xh;
        sum += xh;
    }
    /* Normalize to unit sum (DC gain 1) */
    if (sum != 0.0) {
        for (int i=0;i<ntaps;i++) f->taps[i] = (float)(f->taps[i] / sum);
    }

    /* Polyphase split for the block path (see demod_fast_block). */
    f->nte = ntaps/2 + 1;
    f->nto = ntaps/2;
    f->te  = (float*)calloc((size_t)f->nte, sizeof(float));
    f->to  = (float*)calloc((size_t)f->nto, sizeof(float));
    if(!f->te || !f->to) return -1;
    for (int u=0; u<f->nte; ++u) f->te[u] = f->taps[2*u];
    for (int v=0; v<f->nto; ++v) f->to[v] = f->taps[2*v+1];
    return 0;
}

/* RRC delay line: doubled ring buffer. Each sample is written at both zi_pos and
 * zi_pos+ntaps, so the ntaps most recent samples are always a contiguous slice
 * starting at zi_pos+1 — no per-tap wraparound branch, no reverse stride, letting
 * -O3 auto-vectorize the dot product.
 *
 * Store and evaluate are deliberately SEPARATE. Every input sample must enter the
 * delay line, but at SPS=2 only every second filter OUTPUT is ever read (the symbol
 * strobe). Computing the dot product on the discarded half was pure waste: see
 * demod_process, which calls rrc_store() unconditionally and rrc_dot() only on the
 * samples it actually consumes. Output is bit-identical — this is just
 * filter-then-decimate done in the correct order. */
static inline void rrc_store(rrc_filt_t *f, cpx x){
    const int ntaps = f->ntaps;
    if (++f->zi_pos >= ntaps) f->zi_pos = 0;
    const int p = f->zi_pos;
    f->zi_i[p] = x.i; f->zi_i[p + ntaps] = x.i;
    f->zi_q[p] = x.q; f->zi_q[p + ntaps] = x.q;
}

/* Dot product over the ntaps most recent samples (must follow an rrc_store). */
static inline cpx rrc_dot(const rrc_filt_t *f){
    const int ntaps = f->ntaps;
    const float *taps = f->taps;
    const float *wi = f->zi_i + f->zi_pos + 1;
    const float *wq = f->zi_q + f->zi_pos + 1;
    cpx y={0,0};
    for(int k=0; k<ntaps; k++){
        y.i += taps[k] * wi[k];
        y.q += taps[k] * wq[k];
    }
    return y;
}

static void rrc_free(rrc_filt_t *f){
    if(!f) return;
    free(f->taps); free(f->zi_i); free(f->zi_q);
    free(f->te); free(f->to);
    memset(f,0,sizeof(*f));
}

/* --------- Auto‑selection of DPSK phase rotation & bit order ---------- */

#define SYNC_A_11B  1810u /* 0b11100010010 = 0x712 */
#define SYNC_B_11B  237u  /* 0b00011101101 = 0x0ED */

static void autosel_init(struct demod_s *d)
{
    d->auto_done    = 0;
    d->sel_rot      = 0;
    d->sel_lsb_first= 0;
    d->sel_swap_hint= 0;

    d->inc_cap = 8192; /* ~8k symbols -> 16k bits (reduced from 16k to save memory/CPU) */
    /* Live RF: the wrong 90 degree rotation still scores 50-80 % of the right one (with
     * Gray mapping one of its bits equals the other bit of the right rotation), which
     * 4k increments cannot separate reliably under noise. 64k (~6 ms) can. */
    if (d->C.track) d->inc_cap = 131072;
    d->inc_len = 0;
    d->inc_buf = (uint8_t*)malloc(d->inc_cap);
}

static void autosel_free(struct demod_s *d)
{
    free(d->inc_buf);
    d->inc_buf=NULL;
    d->inc_cap= d->inc_len = 0;
}

static void autosel_feed_inc(struct demod_s *d, uint8_t inc)
{
    if (d->auto_done || !d->inc_buf) return;
    if (d->inc_len < d->inc_cap) d->inc_buf[d->inc_len++] = (uint8_t)(inc & 3);
}

/* Compute score:
   We test 4 rotations (rot=0..3) × 2 bit orders (msbfirst/lsbfirst).
   For each variant we count A/B sync hits (11-bit) in two streams,
   created by alternating bit assignment (parity 0/1).
   Additionally we evaluate "normal" (A on 0, B on 1) and "swap" (B on 0, A on 1)
   to detect a swap tendency (hint only, since demod only outputs bits).
*/
static void autosel_run(struct demod_s *d)
{
    if (d->auto_done || d->inc_len < (size_t) (d->inc_cap/2)) return; /* not before ~4k incs */

    static const uint8_t invmap[4] = {0,2,3,1};

    int best_rot = 0, best_order = 0, best_swap = 0;
    uint64_t best_score = 0;

    for (int rot=0; rot<4; ++rot){
        for (int order=0; order<2; ++order){
            uint32_t sr[2] = {0,0};
            uint64_t score_norm = 0, score_swap = 0;
            int parity = 0;

            for (size_t i=0; i<d->inc_len; ++i){
                uint8_t dib = invmap[(d->inc_buf[i] + rot) & 3];

                uint8_t b_first = (order==0) ? ((dib>>1)&1) : (dib&1); /* MSB or LSB first */
                uint8_t b_second= (order==0) ? (dib&1)     : ((dib>>1)&1);

                /* bit #1 */
                sr[parity] = ((sr[parity]<<1) | b_first) & 0x7FFu;
                /* score */
                if (sr[0]==SYNC_A_11B) score_norm++;
                if (sr[1]==SYNC_B_11B) score_norm++;
                if (sr[0]==SYNC_B_11B) score_swap++;
                if (sr[1]==SYNC_A_11B) score_swap++;
                parity ^= 1;

                /* bit #2 */
                sr[parity] = ((sr[parity]<<1) | b_second) & 0x7FFu;
                if (sr[0]==SYNC_A_11B) score_norm++;
                if (sr[1]==SYNC_B_11B) score_norm++;
                if (sr[0]==SYNC_B_11B) score_swap++;
                if (sr[1]==SYNC_A_11B) score_swap++;
                parity ^= 1;
            }

            uint64_t sc = (score_norm > score_swap) ? score_norm : score_swap;
            int swap = (score_norm > score_swap) ? 0 : 1;

            if (sc > best_score){
                best_score = sc;
                best_rot   = rot;
                best_order = order;
                best_swap  = swap;
            }
        }
    }

    d->sel_rot       = best_rot;
    d->sel_lsb_first = best_order;  /* 0=MSB first, 1=LSB first */
    d->sel_swap_hint = best_swap;
    d->auto_done     = 1;

    /* Info for user */
    fprintf(stderr, "[demod] auto-map: rot=%d, order=%s, swap_hint=%s, score=%llu (on %zu inc)\n",
            d->sel_rot,
            d->sel_lsb_first ? "LSB->MSB" : "MSB->LSB",
            d->sel_swap_hint ? "B<->A" : "A,B",
            (unsigned long long)best_score, d->inc_len);

    /* We can free the buffer now */
    autosel_free(d);
}

/* Block path (defined further below) */
static int    demod_scratch_ensure(demod_t *d);
static void   demod_scratch_free(demod_t *d);
static size_t demod_fast_block(demod_t *d, const void *iq, size_t first, size_t count,
                               int64_t n_first, uint8_t *out, size_t out_cap);
/* Tracking path (defined further below) */
static int    demod_track_init(demod_t *d);
static void   demod_track_reset(demod_t *d);
static void   demod_track_free(demod_t *d);
static size_t demod_track_process(demod_t *d, const void *iq, size_t iq_samples,
                                  uint8_t *out, size_t out_cap);

/* ----------------- Public API ----------------- */

demod_t* demod_create(const demod_cfg_t *cfg)
{
    if(!cfg) return NULL;
    demod_t *d = (demod_t*)calloc(1,sizeof(*d));
    if(!d) return NULL;

    /* fixed SPS=2, symbol rate from fs/2 if not specified */
    unsigned fs  = cfg->fs_hz  ? cfg->fs_hz  : 20480000u;
    unsigned sym = cfg->sym_hz ? cfg->sym_hz : fs/2u;
    double sps   = (double)fs / (double)sym;
    if (sps < 1.8 || sps > 2.2) { sym = fs/2u; sps = 2.0; }
    d->tr_sps    = sps;         /* the tracking path resamples to the true symbol rate */

    d->C   = *cfg;
    d->C.fs_hz  = fs;
    d->C.sym_hz = sym;
    d->fmt = cfg->fmt;

    d->sps           = 2.0;
    d->sample_idx    = -1;      /* set to 0 on the first sample */
    d->strobe_ofs    = 0;
    d->strobe_locked = 0;

    d->prev_q = 0;
    d->have_prev_q = 0;

    /* RRC per config */
    double beta = (cfg->rrc_beta > 0.0f) ? cfg->rrc_beta : 0.25;
    int    span = (cfg->rrc_span > 0)    ? cfg->rrc_span : 8;
    if (rrc_make(&d->rrc, beta, span, d->sps) != 0) { demod_free(d); return NULL; }

    /* Set up AGC */
    d->agc_ref = (cfg->agc_ref > 0.0f) ? cfg->agc_ref : 0.7f;
    double tau = (cfg->agc_tau > 0.0f) ? cfg->agc_tau : 0.05f; /* seconds */
    double fsd = (double)d->C.fs_hz;
    double a   = exp(-1.0 / (tau * fsd));
    if (a < 0.0) a = 0.0;
    if (a > 0.999999) a = 0.999999;
    d->agc_alpha = (float)a;
    d->agc_env   = 0.0f;
    d->agc_gain  = 1.0f;    /* Start with unity gain */
    d->agc_counter = 0;
    {
        double acc_p = 1.0;
        for (int m = 0; m <= AGC_UPDATE_INTERVAL; ++m) { d->agc_pow[m] = (float)acc_p; acc_p *= a; }
        for (int j = 0; j < AGC_UPDATE_INTERVAL-1; ++j)
            d->agc_wrev[j] = d->agc_pow[AGC_UPDATE_INTERVAL-2-j];
    }

    autosel_init(d);
    if (cfg->track && demod_track_init(d) != 0) { demod_free(d); return NULL; }
    return d;
}

void demod_free(demod_t *d){
    if(!d) return;
    autosel_free(d);
    demod_scratch_free(d);
    demod_track_free(d);
    rrc_free(&d->rrc);
    free(d);
}

void demod_reset(demod_t *d){
    if(!d) return;
    d->sample_idx    = -1;
    d->strobe_ofs    = 0;
    d->strobe_locked = 0;
    d->strobe_acc[0] = d->strobe_acc[1] = 0.0;
    d->strobe_cnt[0] = d->strobe_cnt[1] = 0;
    d->prev_q        = 0;
    d->have_prev_q   = 0;
    d->agc_env       = 0.0f;
    d->agc_gain      = 1.0f;
    d->agc_counter   = 0;

    autosel_free(d);
    autosel_init(d);

    if (d->rrc.zi_i) memset(d->rrc.zi_i,0,sizeof(float)*(size_t)d->rrc.zi_len*2u);
    if (d->rrc.zi_q) memset(d->rrc.zi_q,0,sizeof(float)*(size_t)d->rrc.zi_len*2u);
    if (d->C.track) demod_track_reset(d);
}

void demod_get_track_info(const demod_t *d, demod_track_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!d || !d->C.track) return;
    const double sym = (double)d->C.fs_hz / d->tr_sps;
    out->cfo_hz     = (float)((double)d->cl_freq / (2.0*M_PI) * sym);
    out->clock_ppm  = (float)(d->tr_rate / d->tr_sps * 1e6);
    out->level_dbfs = (d->agc_env > 1e-7f) ? 20.0f*log10f(d->agc_env) : -140.0f;
    out->evm        = (d->evm_amp > 1e-6f) ? sqrtf(d->evm_err) / d->evm_amp : 1.0f;
}

/* ================== Fast block path (steady state) ==================
 *
 * Once strobe offset and auto-map are fixed, the sample loop further below does
 * exactly the same thing for every sample -- there is nothing left to decide. This
 * state is >99.9% of a run and therefore gets a block implementation that the
 * compiler can vectorize. Three stages:
 *
 *   A) int16/float -> float, AGC, clip, into ONE contiguous buffer
 *   B) Polyphase RRC with decimation by 2   (core of the speed-up)
 *   C) Quadrant -> difference -> bits
 *
 * Re B: the filter output is y(t) = sum_k taps[k]*buf[t+k], but it is only needed
 * for every second t. Vectorized directly over t, the accesses would be stride-2, and
 * each output value would incur a horizontal reduction -- exactly what made the old
 * variant expensive. Splitting taps by index parity (te/to) and the input by
 * sample parity (E/O) turns this into
 *
 *     y(o) = sum_u te[u]*E[o+u] + sum_v to[v]*O[o+v]
 *
 * i.e. stride 1 in o. This keeps 16 output accumulators simultaneously in one
 * AVX-512 register, and there is no per-symbol reduction anymore.
 *
 * Arithmetically this is the same sum as in the sample path, just grouped differently.
 */

static int demod_scratch_ensure(demod_t *d)
{
    if (d->sc_ready) return 0;
    const int ntaps = d->rrc.ntaps;
    const size_t nb  = (size_t)(ntaps - 1) + DEMOD_TILE;   /* history + tile       */
    const size_t nh  = nb/2 + 4;                           /* per parity half      */
    const size_t ny  = DEMOD_TILE/2 + 4;                   /* symbols per tile     */
    d->sc_bi = (float*)malloc(nb*sizeof(float));
    d->sc_bq = (float*)malloc(nb*sizeof(float));
    d->sc_ei = (float*)malloc(nh*sizeof(float));
    d->sc_eq = (float*)malloc(nh*sizeof(float));
    d->sc_oi = (float*)malloc(nh*sizeof(float));
    d->sc_oq = (float*)malloc(nh*sizeof(float));
    d->sc_yi = (float*)malloc(ny*sizeof(float));
    d->sc_yq = (float*)malloc(ny*sizeof(float));
    if(!d->sc_bi||!d->sc_bq||!d->sc_ei||!d->sc_eq||!d->sc_oi||!d->sc_oq||!d->sc_yi||!d->sc_yq){
        /* Partially allocated: free everything again, otherwise the next
         * attempt (the caller simply stays in the sample path and retries
         * on the next tile) would leak the already allocated buffers. */
        demod_scratch_free(d);
        return -1;
    }
    d->sc_ready = 1;
    return 0;
}

static void demod_scratch_free(demod_t *d)
{
    free(d->sc_bi); free(d->sc_bq);
    free(d->sc_ei); free(d->sc_eq);
    free(d->sc_oi); free(d->sc_oq);
    free(d->sc_yi); free(d->sc_yq);
    d->sc_bi=d->sc_bq=d->sc_ei=d->sc_eq=d->sc_oi=d->sc_oq=d->sc_yi=d->sc_yq=NULL;
    d->sc_ready = 0;
}

/* Processes iq[first .. first+count-1]. n_first is the absolute sample index of
 * iq[first]. Returns: number of bits written. */
static size_t demod_fast_block(demod_t *d, const void *iq, size_t first, size_t count,
                               int64_t n_first, uint8_t *out, size_t out_cap)
{
    const int ntaps = d->rrc.ntaps;
    const int nhist = ntaps - 1;          /* even, since ntaps is odd */
    const int nte = d->rrc.nte, nto = d->rrc.nto;
    const float *te = d->rrc.te, *to = d->rrc.to;
    const demod_iqfmt_t fmt = d->fmt;
    const float alpha = d->agc_alpha, one_m_alpha = 1.0f - d->agc_alpha;
    const float agc_ref = d->agc_ref;

    float *bi = d->sc_bi, *bq = d->sc_bq;
    float *ei = d->sc_ei, *eq = d->sc_eq;
    float *oi = d->sc_oi, *oq = d->sc_oq;
    float *yi = d->sc_yi, *yq = d->sc_yq;

    /* History: the last nhist post-AGC samples are in the ring. The window is
     * zi[zi_pos+1 .. zi_pos+ntaps] with the newest sample at the very end, so
     * the last nhist samples are zi[zi_pos+2 .. zi_pos+ntaps]. */
    {
        const int p = d->rrc.zi_pos;
        memcpy(bi, d->rrc.zi_i + p + 2, (size_t)nhist*sizeof(float));
        memcpy(bq, d->rrc.zi_q + p + 2, (size_t)nhist*sizeof(float));
    }

    size_t out_bits = 0;

    for (size_t done = 0; done < count; ) {
        const size_t tile = (count - done > (size_t)DEMOD_TILE) ? (size_t)DEMOD_TILE : (count - done);
        const int64_t n_start = n_first + (int64_t)done;

        /* ---- Stage A: load, AGC, clip ---------------------------------------
         * The envelope env = alpha*env + (1-alpha)*mag is a recursion and thus
         * blocks any vectorization of the load loop -- after the FIR rework it
         * was the largest single cost. However, env is only read every
         * AGC_UPDATE_INTERVAL samples, namely when a new gain is derived from it.
         * Within such a run gain is constant, and the result of the recursion
         * can be expressed in closed form:
         *
         *     env_nv = alpha^nv * env_0 + (1-alpha) * sum_j alpha^(nv-1-j) * mag[j]
         *
         * This is the same computation, just grouped differently: the sum is an
         * ordinary dot product and vectorizes; the recursion remains only once
         * per run instead of once per sample. */
        float env = d->agc_env, gain = d->agc_gain;
        int   cnt = d->agc_counter;
        const int16_t *ps = (const int16_t*)iq + 2*(first + done);
        const float   *pf = (const float  *)iq + 2*(first + done);
        const int8_t  *p8 = (const int8_t *)iq + 2*(first + done);

        for (size_t t = 0; t < tile; ) {
            /* There are AGC_UPDATE_INTERVAL-cnt samples until the next gain change;
             * the last of them already uses the new gain (as in the sample path). */
            size_t run = (size_t)(AGC_UPDATE_INTERVAL - cnt);
            int boundary = 1;
            if (run > tile - t) { run = tile - t; boundary = 0; }
            const size_t nv = boundary ? run - 1 : run;   /* samples with current gain */

            if (nv) {
                const float *w = d->agc_wrev + (AGC_UPDATE_INTERVAL - 1 - nv);
                float acc = 0.0f;
                if (fmt == DEMOD_FMT_S16LE) {
                    for (size_t j = 0; j < nv; ++j) {
                        float xi = (float)ps[2*(t+j)+0] * (1.0f/32768.0f);
                        float xq = (float)ps[2*(t+j)+1] * (1.0f/32768.0f);
                        float mi = (xi < 0.0f) ? -xi : xi;
                        float mq = (xq < 0.0f) ? -xq : xq;
                        acc += w[j] * ((mq > mi) ? mq : mi);
                        bi[nhist+t+j] = clipf(xi*gain, 2.0f);
                        bq[nhist+t+j] = clipf(xq*gain, 2.0f);
                    }
                } else if (fmt == DEMOD_FMT_S8) {
                    for (size_t j = 0; j < nv; ++j) {
                        float xi = (float)p8[2*(t+j)+0] * (1.0f/128.0f);
                        float xq = (float)p8[2*(t+j)+1] * (1.0f/128.0f);
                        float mi = (xi < 0.0f) ? -xi : xi;
                        float mq = (xq < 0.0f) ? -xq : xq;
                        acc += w[j] * ((mq > mi) ? mq : mi);
                        bi[nhist+t+j] = clipf(xi*gain, 2.0f);
                        bq[nhist+t+j] = clipf(xq*gain, 2.0f);
                    }
                } else {
                    for (size_t j = 0; j < nv; ++j) {
                        float xi = pf[2*(t+j)+0], xq = pf[2*(t+j)+1];
                        float mi = (xi < 0.0f) ? -xi : xi;
                        float mq = (xq < 0.0f) ? -xq : xq;
                        acc += w[j] * ((mq > mi) ? mq : mi);
                        bi[nhist+t+j] = clipf(xi*gain, 2.0f);
                        bq[nhist+t+j] = clipf(xq*gain, 2.0f);
                    }
                }
                env = d->agc_pow[nv]*env + one_m_alpha*acc;
                cnt += (int)nv;
                t   += nv;
            }

            if (boundary) {
                /* The sample at which the gain is recomputed -- it already gets the
                 * new gain itself, exactly as in the sample path. */
                float xi, xq;
                if (fmt == DEMOD_FMT_S16LE) {
                    xi = (float)ps[2*t+0] * (1.0f/32768.0f);
                    xq = (float)ps[2*t+1] * (1.0f/32768.0f);
                } else if (fmt == DEMOD_FMT_S8) {
                    xi = (float)p8[2*t+0] * (1.0f/128.0f);
                    xq = (float)p8[2*t+1] * (1.0f/128.0f);
                } else {
                    xi = pf[2*t+0]; xq = pf[2*t+1];
                }
                float mi = (xi < 0.0f) ? -xi : xi;
                float mq = (xq < 0.0f) ? -xq : xq;
                env = alpha*env + one_m_alpha*((mq > mi) ? mq : mi);
                cnt = 0;
                gain = (env > 1e-6f) ? (agc_ref/env) : 1.0f;
                bi[nhist+t] = clipf(xi*gain, 2.0f);
                bq[nhist+t] = clipf(xq*gain, 2.0f);
                t += 1;
            }
        }
        d->agc_env = env; d->agc_gain = gain; d->agc_counter = cnt;

        /* ---- Stage B: polyphase FIR with decimation by 2 --------------------- */
        /* Strobe hits absolute indices n with (n - strobe_ofs) even; relative to the
         * tile, that is t = t0, t0+2, ... with t0 = (strobe_ofs + n_start) & 1. */
        const int t0 = (int)((d->strobe_ofs + (int)(n_start & 1)) & 1);
        const size_t lastbuf = (size_t)nhist + tile - 1;      /* last valid buf index */
        size_t nout = 0;
        if ((size_t)t0 + (size_t)ntaps - 1 <= lastbuf)
            nout = (lastbuf - (size_t)t0 - (size_t)(ntaps-1)) / 2 + 1;

        if (nout) {
            /* Deinterleave by parity, starting at t0. */
            const size_t nE = (lastbuf - (size_t)t0)/2 + 1;
            const size_t nO = (lastbuf >= (size_t)t0+1) ? (lastbuf - (size_t)t0 - 1)/2 + 1 : 0;
            for (size_t j = 0; j < nE; ++j) { ei[j] = bi[t0 + 2*j];     eq[j] = bq[t0 + 2*j]; }
            for (size_t j = 0; j < nO; ++j) { oi[j] = bi[t0 + 1 + 2*j]; oq[j] = bq[t0 + 1 + 2*j]; }

            /* Register tiling over DV outputs: output block outside, taps inside,
             * so that the four accumulators stay in registers across all taps.
             * The other way round (tap outside), each tap would be a separate
             * read-modify-write pass over yi/yq, i.e. 2*ntaps passes of the whole
             * tile through L1/L2 -- making the FIR memory-bound instead of compute-bound.
             *
             * The accumulators are deliberately written as a vector type rather than
             * a float array: with an array, GCC spilled them to the stack and reloaded
             * them on every tap despite the constant block size. */
            size_t o0 = 0;
#if DEMOD_HAVE_VEC
            for (; o0 + DV <= nout; o0 += DV) {
                dvec_t ai = {0}, aq = {0};
                for (int u = 0; u < nte; ++u) {
                    const float c = te[u];
                    ai += c * (*(const dvecu_t*)(ei + o0 + u));
                    aq += c * (*(const dvecu_t*)(eq + o0 + u));
                }
                for (int v = 0; v < nto; ++v) {
                    const float c = to[v];
                    ai += c * (*(const dvecu_t*)(oi + o0 + v));
                    aq += c * (*(const dvecu_t*)(oq + o0 + v));
                }
                *(dvecu_t*)(yi + o0) = ai;
                *(dvecu_t*)(yq + o0) = aq;
            }
#endif
            for (; o0 < nout; ++o0) {
                float ai = 0.0f, aq = 0.0f;
                for (int u = 0; u < nte; ++u) { ai += te[u]*ei[o0+u]; aq += te[u]*eq[o0+u]; }
                for (int v = 0; v < nto; ++v) { ai += to[v]*oi[o0+v]; aq += to[v]*oq[o0+v]; }
                yi[o0] = ai; yq[o0] = aq;
            }

            /* ---- Stage C: quadrant -> difference -> bits -------------------- */
            static const uint8_t invmap[4] = {0,2,3,1};
            const int rot = d->sel_rot, lsb = d->sel_lsb_first;
            int prev_q = d->prev_q;
            for (size_t o = 0; o < nout; ++o) {
                cpx z = { yi[o], yq[o] };
                int q = decide_quadrant(z);
                int inc = (q - prev_q) & 3;
                prev_q = q;
                uint8_t dib = invmap[(inc + rot) & 3];
                uint8_t b_first  = lsb ? (dib & 1) : ((dib>>1)&1);
                uint8_t b_second = lsb ? ((dib>>1)&1) : (dib & 1);
                if (out_bits + 2 <= out_cap) {
                    out[out_bits++] = b_first;
                    out[out_bits++] = b_second;
                }
            }
            d->prev_q = prev_q;
        }

        /* Make the ring consistent again while the last ntaps samples are still
         * fully in bi/bq (after the memmove below only nhist remain). This lets
         * the sample path take over seamlessly at any time --
         * e.g. when the strobe is searched again or after demod_reset.
         * rrc_dot reads the window zi[p+1 .. p+ntaps] with the newest sample
         * at the very end; with p = ntaps-1 that is zi[ntaps .. 2*ntaps-1]. */
        {
            const size_t tail = (size_t)nhist + tile - (size_t)ntaps;  /* first of the last ntaps */
            for (int m = 0; m < ntaps; ++m) {
                d->rrc.zi_i[m] = d->rrc.zi_i[m+ntaps] = bi[tail+m];
                d->rrc.zi_q[m] = d->rrc.zi_q[m+ntaps] = bq[tail+m];
            }
            d->rrc.zi_pos = ntaps - 1;
        }

        /* Move the last nhist samples to the front as history for the next tile. */
        memmove(bi, bi + tile, (size_t)nhist*sizeof(float));
        memmove(bq, bq + tile, (size_t)nhist*sizeof(float));
        done += tile;
    }

    d->sample_idx = n_first + (int64_t)count - 1;
    return out_bits;
}


/* ================== Tracking path (track=1: live RF / SDR) ==================
 *
 * The fixed path above relies on the IQ file coming straight from the encoder: exactly
 * 2 samples per symbol from the same clock, no carrier offset. A real receiver has
 * neither: the HackRF crystal is good to ~±20 ppm, i.e. a carrier offset of a few kHz
 * at 118 MHz and a sample clock that drifts a full sample against the symbol clock
 * within milliseconds. This path therefore adds:
 *
 *   A) int -> float, DC blocker (LO leakage spike of zero-IF SDRs), AGC, clip
 *   B) RRC matched filter at the full input rate (vectorized over outputs)
 *   C) Gardner timing error detector with a cubic Lagrange interpolator, PI loop;
 *      works for any fs near 2*sym (e.g. HackRF at 20 MS/s for 10.24 MBd)
 *   D) decision-directed QPSK Costas loop on the symbols (removes carrier offset and
 *      phase drift; the remaining 90 degree ambiguity cancels in the differential decode)
 *   E) quadrant -> difference -> auto-map -> bits, as in the fixed path
 *
 * Gardner and the Costas loop are both normalized (by symbol power / amplitude), so
 * their gains are independent of the signal level. */

#define TRK_HIST     8          /* filtered samples kept for the interpolator (k-1..k+2) */
#define TRK_DC_ALPHA (1.0f/8192.0f)
#define TRK_SETTLE   65536      /* symbols (~6.4 ms) discarded while the loops pull in */
#define TRK_AVG      (1.0f/4096.0f)
#define TRK_BATCH    8          /* symbols per loop update, see demod_track_process */

/* PI loop gains for a second-order loop with detector gain 1 (Gardner/Costas both
 * normalized), noise bandwidth bn (per update) and damping 1/sqrt(2). */
static void loop_gains(double bn, float *kp, float *ki)
{
    const double zeta = 0.7071, th = bn / (zeta + 0.25/zeta);
    const double den = 1.0 + 2.0*zeta*th + th*th;
    *kp = (float)(4.0*zeta*th/den);
    *ki = (float)(4.0*th*th/den);
}

static int demod_track_init(demod_t *d)
{
    const int nhist = d->rrc.ntaps - 1;
    d->tr_xi = (float*)calloc((size_t)nhist + DEMOD_TILE + 16, sizeof(float));
    d->tr_xq = (float*)calloc((size_t)nhist + DEMOD_TILE + 16, sizeof(float));
    d->tr_fi = (float*)calloc((size_t)TRK_HIST + DEMOD_TILE + 16, sizeof(float));
    d->tr_fq = (float*)calloc((size_t)TRK_HIST + DEMOD_TILE + 16, sizeof(float));
    if (!d->tr_xi || !d->tr_xq || !d->tr_fi || !d->tr_fq) return -1;
    d->tr_dc_pow = powf(1.0f - TRK_DC_ALPHA, (float)AGC_UPDATE_INTERVAL);
    loop_gains(0.002, &d->tr_kp, &d->tr_ki);   /* timing: slow, clock drift is tiny */
    loop_gains(0.005, &d->cl_kp, &d->cl_ki);   /* carrier: ~50 kHz loop, locks within ~±20 kHz */
    demod_track_reset(d);
    return 0;
}

static void demod_track_reset(demod_t *d)
{
    const int nhist = d->rrc.ntaps - 1;
    if (d->tr_xi) memset(d->tr_xi, 0, (size_t)nhist*sizeof(float));
    if (d->tr_xq) memset(d->tr_xq, 0, (size_t)nhist*sizeof(float));
    if (d->tr_fi) memset(d->tr_fi, 0, (size_t)TRK_HIST*sizeof(float));
    if (d->tr_fq) memset(d->tr_fq, 0, (size_t)TRK_HIST*sizeof(float));
    d->dc_i = d->dc_q = 0.0f;
    d->tr_pos = 2.0;
    d->tr_rate = 0.0;
    d->tr_yi = d->tr_yq = 0.0f;
    d->tr_have_prev = 0;
    d->tr_pow = 0.5f;
    d->tr_settle = TRK_SETTLE;
    d->cl_c = 1.0f; d->cl_s = 0.0f;
    d->cl_freq = 0.0f;
    d->evm_amp = 0.5f; d->evm_err = 0.25f;
}

static void demod_track_free(demod_t *d)
{
    free(d->tr_xi); free(d->tr_xq);
    free(d->tr_fi); free(d->tr_fq);
    d->tr_xi = d->tr_xq = d->tr_fi = d->tr_fq = NULL;
}

/* Stage A for n samples starting at iq[first]: into xi/xq. DC estimate and AGC envelope
 * are updated once per AGC_UPDATE_INTERVAL samples from that run's mean (closed form of
 * the per-sample recursions with a constant input), so the per-sample loops carry no
 * dependency and vectorize. The gain was only ever recomputed per run anyway. */
static void track_load(demod_t *d, const void *iq, size_t first, size_t n, float *xi, float *xq)
{
    const float sc = (d->fmt == DEMOD_FMT_S16LE) ? 1.0f/32768.0f :
                     (d->fmt == DEMOD_FMT_S8)    ? 1.0f/128.0f : 1.0f;
    for (size_t r0 = 0; r0 < n; r0 += AGC_UPDATE_INTERVAL) {
        const size_t len = (n - r0 < AGC_UPDATE_INTERVAL) ? n - r0 : AGC_UPDATE_INTERVAL;
        const size_t k0 = 2*(first + r0);
        float *oi = xi + r0, *oq = xq + r0;
        switch (d->fmt) {
        case DEMOD_FMT_S16LE: {
            const int16_t *p = (const int16_t*)iq + k0;
            for (size_t j = 0; j < len; ++j) { oi[j] = (float)p[2*j]*sc; oq[j] = (float)p[2*j+1]*sc; }
            break; }
        case DEMOD_FMT_S8: {
            const int8_t *p = (const int8_t*)iq + k0;
            for (size_t j = 0; j < len; ++j) { oi[j] = (float)p[2*j]*sc; oq[j] = (float)p[2*j+1]*sc; }
            break; }
        default: {
            const float *p = (const float*)iq + k0;
            for (size_t j = 0; j < len; ++j) { oi[j] = p[2*j]; oq[j] = p[2*j+1]; }
            break; }
        }
        const float dci = d->dc_i, dcq = d->dc_q, gain = d->agc_gain;
        float si = 0.0f, sq = 0.0f, smag = 0.0f;
        for (size_t j = 0; j < len; ++j) {
            const float i = oi[j], q = oq[j];
            si += i; sq += q;
            const float ci = i - dci, cq = q - dcq;
            smag += fmaxf(fabsf(ci), fabsf(cq));
            oi[j] = clipf(ci*gain, 2.0f);
            oq[j] = clipf(cq*gain, 2.0f);
        }
        const float inv = 1.0f / (float)len;
        const float wdc = 1.0f - ((len == AGC_UPDATE_INTERVAL) ? d->tr_dc_pow : powf(1.0f - TRK_DC_ALPHA, (float)len));
        const float wenv = 1.0f - d->agc_pow[len];
        d->dc_i += (si*inv - d->dc_i) * wdc;
        d->dc_q += (sq*inv - d->dc_q) * wdc;
        if (d->agc_env > 0.0f) d->agc_env += (smag*inv - d->agc_env) * wenv;
        else                   d->agc_env = smag*inv;   /* first run: start at the input level */
        d->agc_gain = (d->agc_env > 1e-6f) ? (d->agc_ref / d->agc_env) : 1.0f;
    }
}

/* Stage B: y[o] = sum_k taps[k]*x[o+k], o = 0..nout-1 (x holds nout+ntaps-1 samples). */
static void track_fir(const rrc_filt_t *f, const float *x, float *y, size_t nout)
{
    const int ntaps = f->ntaps;
    const float *taps = f->taps;
    size_t o0 = 0;
#if DEMOD_HAVE_VEC
    for (; o0 + DV <= nout; o0 += DV) {
        dvec_t a = {0};
        for (int k = 0; k < ntaps; ++k) a += taps[k] * (*(const dvecu_t*)(x + o0 + k));
        *(dvecu_t*)(y + o0) = a;
    }
#endif
    for (; o0 < nout; ++o0) {
        float a = 0.0f;
        for (int k = 0; k < ntaps; ++k) a += taps[k] * x[o0 + k];
        y[o0] = a;
    }
}

/* Cubic Lagrange interpolation at fractional index pos >= 1 (reads pos-1 .. pos+2). */
static inline cpx track_interp(const float *fi, const float *fq, float pos)
{
    const int k = (int)pos;
    const float mu = pos - (float)k;
    const float mm1 = mu - 1.0f, mm2 = mu - 2.0f, mp1 = mu + 1.0f;
    const float cm1 = -mu*mm1*mm2*(1.0f/6.0f);
    const float c0  =  mp1*mm1*mm2*0.5f;
    const float c1  = -mp1*mu*mm2*0.5f;
    const float c2  =  mp1*mu*mm1*(1.0f/6.0f);
    cpx r;
    r.i = cm1*fi[k-1] + c0*fi[k] + c1*fi[k+1] + c2*fi[k+2];
    r.q = cm1*fq[k-1] + c0*fq[k] + c1*fq[k+1] + c2*fq[k+2];
    return r;
}

/* decide_quadrant from sign bits: no compare at all (random symbol signs make every
 * branch a coin flip). Differs from it only for -0.0f, which never matters here. */
static inline int track_quadrant(float zi, float zq)
{
    static const uint8_t qlut[4] = { 2, 3, 1, 0 };
    return qlut[((unsigned)signbit(zi) ? 2u : 0u) | ((unsigned)signbit(zq) ? 1u : 0u)];
}

/* Stage E for one quadrant decision. Returns the new out_bits. */
static inline size_t track_emit(demod_t *d, int q, uint8_t *out, size_t out_bits, size_t out_cap)
{
    if (d->tr_settle > 0) { d->tr_settle--; return out_bits; }
    if (!d->have_prev_q) { d->prev_q = q; d->have_prev_q = 1; return out_bits; }
    const int inc = (q - d->prev_q) & 3;
    d->prev_q = q;
    if (!d->auto_done) {
        autosel_feed_inc(d, (uint8_t)inc);
        autosel_run(d);
        return out_bits;
    }
    static const uint8_t invmap[4] = {0,2,3,1};
    const uint8_t dib = invmap[(inc + d->sel_rot) & 3];
    if (out_bits + 2 <= out_cap) {
        out[out_bits++] = d->sel_lsb_first ? (dib & 1) : ((dib>>1)&1);
        out[out_bits++] = d->sel_lsb_first ? ((dib>>1)&1) : (dib & 1);
    }
    return out_bits;
}

/* Stages C+D. Both loops are updated once per batch of TRK_BATCH symbols instead of per
 * symbol: within a batch, strobe positions and carrier phasor advance at the current
 * rate/frequency estimate and do not depend on each other's errors, so the CPU can
 * overlap them (a per-symbol update made interpolate -> error -> next position one long
 * dependency chain). The summed errors give the same integral action; the effective loop
 * bandwidth per update is TRK_BATCH times the per-symbol value, still well inside the
 * stable range. */
static size_t demod_track_process(demod_t *d, const void *iq, size_t iq_samples,
                                  uint8_t *out, size_t out_cap)
{
    const int nhist = d->rrc.ntaps - 1;
    const double sps = d->tr_sps;
    size_t out_bits = 0;

    for (size_t done = 0; done < iq_samples; ) {
        const size_t tile = (iq_samples - done > (size_t)DEMOD_TILE) ? (size_t)DEMOD_TILE
                                                                    : iq_samples - done;
        track_load(d, iq, done, tile, d->tr_xi + nhist, d->tr_xq + nhist);
        track_fir(&d->rrc, d->tr_xi, d->tr_fi + TRK_HIST, tile);
        track_fir(&d->rrc, d->tr_xq, d->tr_fq + TRK_HIST, tile);
        memmove(d->tr_xi, d->tr_xi + tile, (size_t)nhist*sizeof(float));
        memmove(d->tr_xq, d->tr_xq + tile, (size_t)nhist*sizeof(float));

        const float *fi = d->tr_fi, *fq = d->tr_fq;
        const size_t flen = (size_t)TRK_HIST + tile;
        const double lim = (double)flen - 3.0;    /* interpolator reads up to k+2 */
        double pos = d->tr_pos;
        for (;;) {
            const double step = sps + d->tr_rate;
            int nb = TRK_BATCH;
            while (nb > 0 && pos + nb*step >= lim) nb--;
            if (nb == 0) break;

            /* C) on-time and mid samples, Gardner error. Positions as float offsets
             * from an integer base keep the interpolator in single precision. */
            cpx y[TRK_BATCH], m[TRK_BATCH];
            const int kb = (int)pos - 2;
            const float fp = (float)(pos - (double)kb), fs = (float)step;
            for (int k = 0; k < nb; ++k) {
                const float on = fp + (float)(k + 1)*fs;
                m[k] = track_interp(fi + kb, fq + kb, on - 0.5f*fs);
                y[k] = track_interp(fi + kb, fq + kb, on);
            }
            float pi = d->tr_yi, pq = d->tr_yq, esum = 0.0f, pwsum = 0.0f;
            for (int k = 0; k < nb; ++k) {
                esum  += (y[k].i - pi)*m[k].i + (y[k].q - pq)*m[k].q;
                pwsum += y[k].i*y[k].i + y[k].q*y[k].q;
                pi = y[k].i; pq = y[k].q;
            }
            if (!d->tr_have_prev) { esum = 0.0f; d->tr_have_prev = 1; }
            d->tr_pow += (pwsum/(float)nb - d->tr_pow) * (TRK_AVG*(float)nb);
            float et = esum / (d->tr_pow + 1e-9f);
            if (et >  2.0f*nb) et =  2.0f*nb;
            if (et < -2.0f*nb) et = -2.0f*nb;
            /* et > 0: strobes late -> sample earlier */
            d->tr_rate -= d->tr_ki * et;
            if (d->tr_rate >  0.02*sps) d->tr_rate =  0.02*sps;
            if (d->tr_rate < -0.02*sps) d->tr_rate = -0.02*sps;
            pos += nb*step - d->tr_kp * et;
            d->tr_yi = y[nb-1].i; d->tr_yq = y[nb-1].q;

            /* D) Costas: derotate, decision-directed error, quadrant decisions */
            float c = d->cl_c, s = d->cl_s;
            const float f = d->cl_freq, rc = 1.0f - 0.5f*f*f, rs = f;
            const float amp0 = d->evm_amp;
            float ec = 0.0f, asum = 0.0f, errsum = 0.0f;
            int qd[TRK_BATCH];
            for (int k = 0; k < nb; ++k) {
                const float zi = y[k].i*c + y[k].q*s;
                const float zq = y[k].q*c - y[k].i*s;
                const float ai = fabsf(zi), aq = fabsf(zq);
                ec += zq*copysignf(1.0f, zi) - zi*copysignf(1.0f, zq);  /* sgn(I)*Q - sgn(Q)*I, branch-free */
                asum += 0.5f*(ai + aq);
                errsum += 0.5f*((ai - amp0)*(ai - amp0) + (aq - amp0)*(aq - amp0));
                qd[k] = track_quadrant(zi, zq);
                const float nc = c*rc - s*rs;
                s = s*rc + c*rs;
                c = nc;
            }
            /* normalized by |I|+|Q| = 2*amplitude: detector gain 1 like Gardner */
            ec /= 2.0f*amp0 + 1e-9f;
            d->cl_freq += d->cl_ki * ec;
            /* ±0.04 rad/symbol = ±65 kHz: far beyond any SDR crystal error, but keeps the
             * loop from wandering off on noise while no signal is there */
            if (d->cl_freq >  0.04f) d->cl_freq =  0.04f;
            if (d->cl_freq < -0.04f) d->cl_freq = -0.04f;
            const float ph = d->cl_kp * ec, pc = 1.0f - 0.5f*ph*ph;
            const float nc = c*pc - s*ph;
            s = s*pc + c*ph;
            c = nc;
            const float g = 1.5f - 0.5f*(c*c + s*s);
            d->cl_c = c*g; d->cl_s = s*g;
            d->evm_amp += (asum/(float)nb - d->evm_amp) * (TRK_AVG*(float)nb);
            d->evm_err += (errsum/(float)nb - d->evm_err) * (TRK_AVG*(float)nb);

            /* E) */
            for (int k = 0; k < nb; ++k) out_bits = track_emit(d, qd[k], out, out_bits, out_cap);
        }
        const size_t shift = flen - TRK_HIST;
        memmove(d->tr_fi, d->tr_fi + shift, TRK_HIST*sizeof(float));
        memmove(d->tr_fq, d->tr_fq + shift, TRK_HIST*sizeof(float));
        d->tr_pos = pos - (double)shift;
        done += tile;
    }
    d->sample_idx += (int64_t)iq_samples;
    return out_bits;
}

/* ---------- DEMOD: pure encoder‑mirror path (SPS=2) ---------- */
size_t demod_process(demod_t *d, const void *iq, size_t iq_samples, uint8_t *out, size_t out_cap)
{
#ifdef DEMOD_DEBUG
    /* One‑time hex dump of the first N bytes */
    enum { DUMP_BYTES = 512, DUMP_BITS = DUMP_BYTES * 8 };
    static int      s_dump_done = 0;
    static uint32_t s_bit_count = 0;
    static uint8_t  s_bits[DUMP_BITS];
#endif
    if(!d||!iq||!out||out_cap==0) return 0;
    if (d->C.track) return demod_track_process(d, iq, iq_samples, out, out_cap);

    const demod_iqfmt_t fmt = d->fmt;
    size_t out_bits = 0;

#ifdef DEMOD_DEBUG
    /* first I/Q dump, compatible with the encoder */
    static int s_iq_dump_done = 0;
    if (!s_iq_dump_done && fmt == DEMOD_FMT_S16LE && iq_samples >= 32) {
        const int16_t *p = (const int16_t*)iq;
        setvbuf(stderr, NULL, _IONBF, 0);
        fprintf(stderr, "[demod] first 32 I/Q int16 (hex):\n");
        for (int i = 0; i < 32; ++i) {
            uint16_t I = (uint16_t)p[2*i + 0];
            uint16_t Q = (uint16_t)p[2*i + 1];
            fprintf(stderr, "I:0x%04X Q:0x%04X   ", I, Q);
            if ((i % 4) == 3) fprintf(stderr, "\n");
        }
        if ((32 % 4) != 0) fprintf(stderr, "\n");
        s_iq_dump_done = 1;
    }
#endif

    /* Select the better strobe offset (0/1) once:
       M(off) = sum(|I|+|Q|) at sampling points with this offset. */
    int64_t local_idx_base = (d->sample_idx < 0) ? 0 : (d->sample_idx + 1);

    for (size_t n=0; n<iq_samples; ++n) {
#ifndef DEMOD_DEBUG
        /* Steady state? Then process the rest of the tile via the vectorized block path.
         * (With DEMOD_DEBUG the sample path stays active so the dumps are correct.) */
        if (d->strobe_locked && d->auto_done && d->have_prev_q) {
            if (demod_scratch_ensure(d) == 0) {
                out_bits += demod_fast_block(d, iq, n, iq_samples - n,
                                             local_idx_base + (int64_t)n,
                                             out + out_bits, out_cap - out_bits);
                break;
            }
            /* Allocation failed: silently continue in the sample path. */
        }
#endif
        /* 1) Load sample */
        cpx s;
        if (fmt == DEMOD_FMT_S16LE){
            const int16_t *p = (const int16_t*)iq;
            s.i = (float)p[2*n+0] / 32768.0f;
            s.q = (float)p[2*n+1] / 32768.0f;
        } else if (fmt == DEMOD_FMT_S8){
            const int8_t *p = (const int8_t*)iq;
            s.i = (float)p[2*n+0] / 128.0f;
            s.q = (float)p[2*n+1] / 128.0f;
        } else {
            const float *p = (const float*)iq;
            s.i = p[2*n+0]; s.q = p[2*n+1];
        }

        /* --- AGC: leaky‑envelope with cached gain (division every N samples) --- */
        /* Faster magnitude approximation: max(|I|, |Q|) is good enough for AGC */
        float mag_i = (s.i < 0.0f) ? -s.i : s.i;  /* fabsf alternative */
        float mag_q = (s.q < 0.0f) ? -s.q : s.q;
        float mag = (mag_q > mag_i) ? mag_q : mag_i;  /* max */
        d->agc_env = d->agc_alpha * d->agc_env + (1.0f - d->agc_alpha) * mag;
        
        /* Recompute gain only every AGC_UPDATE_INTERVAL samples */
        if (++d->agc_counter >= AGC_UPDATE_INTERVAL) {
            d->agc_counter = 0;
            d->agc_gain = (d->agc_env > 1e-6f) ? (d->agc_ref / d->agc_env) : 1.0f;
        }
        
        s.i = clipf(s.i * d->agc_gain, 2.0f);
        s.q = clipf(s.q * d->agc_gain, 2.0f);

        /* 2) RRC matched filter: ALWAYS push the sample into the delay line, but only
         *    convolve where the result is actually read (strobe or offset search). */
        rrc_store(&d->rrc, s);

        /* 3) Track sample index */
        d->sample_idx = local_idx_base + (int64_t)n;

        /* 4) Offset search over the first ~2*ntaps samples */
        if (!d->strobe_locked) {
            cpx mf = rrc_dot(&d->rrc);
            double  *acc = d->strobe_acc;
            int64_t *cnt = d->strobe_cnt;

            int off = (int)(d->sample_idx & 1);
            acc[off] += fabsf(mf.i) + fabsf(mf.q);
            cnt[off]++;

            if ( (cnt[0] + cnt[1]) >= (int64_t)(2 * d->rrc.ntaps) ) {
                double m0 = (cnt[0]>0) ? acc[0]/cnt[0] : 0.0;
                double m1 = (cnt[1]>0) ? acc[1]/cnt[1] : 0.0;
                d->strobe_ofs    = (m1 > m0) ? 1 : 0;
                d->strobe_locked = 1;
                d->have_prev_q   = 0;   /* reset diff state */
            }
            continue; /* no symbol output until the offset is fixed */
        }

        /* 5) Symbol strobe: every 2 samples at the found offset */
        if ( ((d->sample_idx - d->strobe_ofs) & 1) == 0 ) {
            cpx sym = rrc_dot(&d->rrc);

            /* 6) Hard‑Decision (Quadrant) → 0..3 */
            int q = decide_quadrant(sym);

            /* 7) Differential: inc = (q - prev_q) mod 4 */
            if (!d->have_prev_q) {
                d->prev_q = q;
                d->have_prev_q = 1;   /* first symbol only sets the reference */
                continue;
            }

            int inc = (q - d->prev_q) & 3;
            d->prev_q = q;

            /* During the auto phase, only buffer increments */
            if (!d->auto_done) {
                autosel_feed_inc(d, (uint8_t)inc);
                autosel_run(d);
                continue; /* no output yet during the probe phase */
            }

            /* 8) Apply selected map → dibit */
            static const uint8_t invmap[4] = {0,2,3,1};
            uint8_t dib = invmap[(inc + d->sel_rot) & 3];

            /* 9) Output bits in the selected order */
            uint8_t b_first = d->sel_lsb_first ? (dib & 1) : ((dib>>1)&1);
            uint8_t b_second= d->sel_lsb_first ? ((dib>>1)&1) : (dib & 1);

            if (out_bits + 2 <= out_cap) {
                out[out_bits++] = b_first;
                out[out_bits++] = b_second;

#ifdef DEMOD_DEBUG
                if (!s_dump_done) {
                    if (s_bit_count < DUMP_BITS) s_bits[s_bit_count++] = b_first;
                    if (s_bit_count < DUMP_BITS) s_bits[s_bit_count++] = b_second;

                    if (s_bit_count >= DUMP_BITS) {
                        uint8_t bytes[DUMP_BYTES];
                        for (int i = 0; i < DUMP_BYTES; ++i) {
                            uint8_t v = 0;
                            for (int k = 0; k < 8; ++k)
                                v = (uint8_t)((v << 1) | (s_bits[i*8 + k] & 1));
                            bytes[i] = v;
                        }
                        setvbuf(stderr, NULL, _IONBF, 0);
                        fprintf(stderr, "[demod] first %d bytes:", DUMP_BYTES);
                        for (int i = 0; i < DUMP_BYTES; ++i) {
                            if ((i % 16) == 0) fprintf(stderr, "\n  ");
                            fprintf(stderr, " %02X", bytes[i]);
                        }
                        fprintf(stderr, "\n");
                        s_dump_done = 1;
                    }
                }
#endif
            }
        }
    }

    return out_bits;
}
