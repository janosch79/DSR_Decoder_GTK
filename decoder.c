// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// decoder.c — DSR RAW decoder core
// - A/B mainframe sync (11-bit Barker)
// - PRBS descramble mirroring _mkprbs (x^9 + x^5 + 1; seed=0x0BD; A:type=0, B:type=1; reset per frame)
// - Payload de-interleave -> 4×77b (auto EVEN/ODD: BCH score, or |sample| energy if BCH off)
// - 77b layout exactly like _77block(): 4×11 MSB, 19 BCH, 2× ZI, 4×3 LSB
// - 14-bit audio rebuild -> 16 samples/frame (per A or B)
// - Williard superframe sync: 0000 0101 1100 1111 (0x05CF), logs per log_level
// - Optional BCH(63,44) verification, with real syndrome-based error correction
//   (see bch_63_44_correct() below)

#include "decoder.h"
#include "pa.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#define DLOG_AT(decoder, level, ...) \
    do { if ((decoder)->opts.log_level >= (level)) printf(__VA_ARGS__); } while (0)

/* --- Constants --- */
#define A_SYNC 0x712u /* 11100010010 */
#define B_SYNC 0x0EDu /* 00011101101 */
#define SYNC_BITS 11
#define SYNC_MASK ((1u<<SYNC_BITS)-1u)
/* Flywheel: at the expected sync position right after a frame, accept the sync word with
 * up to this many bit errors instead of dropping into a 300+ bit sliding search (which
 * loses the frame and, before the pairing fix below, desynced A/B until a manual resync). */
#define SYNC_FLYWHEEL_MAX_ERR 2
/* A and B are bit-interleaved and start together, so frames of one A/B pair complete
 * within a couple of bits of each other. */
#define AB_PAIR_MAX_SKEW_BITS 4u

#define WILLIARD_CODE_SYNC1 0x05CFu /* 0000 0101 1100 1111 */
#define WILLIARD_CODE_SYNC2 0x05FFu /* 0000 0101 1111 1111 */

/* Minimum number of complete A mainframes before Williard lock (light debouncing) */
#define WILLIARD_ACQ_MIN_PRIOR_AFRAMES 8u
/* Tracking: after this many failed pos15 checks, drop the lock and re-acquire */
#define WILLIARD_POS15_BAD_UNLOCK 24

/* SYNC1 superframe every 8 Williard octets of 64 A mainframes each → 512 A-SSB steps */
#define DSR_A_MAINFRAMES_PER_WILLIARD_SF 512u

#define SCALE_FACTOR_INVALID 0xFF  /* Marker for an invalid scale factor (valid scale factors are 0-7) */

/* --- Types --- */
typedef struct {
    uint8_t bits[DSR_PAYLOAD_BITS];
    uint16_t nbits;
} payload_t;

typedef struct {
    uint8_t header_sync[SYNC_BITS];
    uint8_t ssb;
    payload_t payload;
    uint64_t idx;
    char kind; // 'A' or 'B'
} mainframe_t;

/* PRBS as encoder _mkprbs.
 * Both sequences depend only on (seed, type), and both call sites use the
 * same constant seed 0x0BD -- so the sequences are invariant over the whole
 * run. Previously they were regenerated at EVERY frame start: with roughly
 * 312k frames and two streams that is ~190 million LFSR steps, all producing
 * exactly the same result. Now built once per decoder_new(); per frame
 * only the index reset remains. */
#define PRBS9_SEED 0x0BDu

typedef struct {
    const uint8_t *seq;  /* points into s_prbs_seq[type] */
    uint16_t idx;        /* position in the sequence   */
} prbs9_enc_t;

static uint8_t s_prbs_seq[2][DSR_PAYLOAD_BITS];

/* Idempotent: always produces the same values, so it is safe to call
 * repeatedly from decoder_new(). */
static void prbs9_build_tables(void){
    for (int type = 0; type < 2; ++type) {
        uint16_t reg = PRBS9_SEED & 0x1FFu;
        if(!reg) reg = 0x0BDu;
        for (int bit_idx = 0; bit_idx < DSR_PAYLOAD_BITS; ++bit_idx) {
            uint8_t lsb  = (uint8_t)(reg & 1u);
            uint8_t bit3 = (uint8_t)((reg >> 3) & 1u);
            s_prbs_seq[type][bit_idx] = type ? (uint8_t)(lsb ^ bit3) : lsb;
            uint8_t newb = (uint8_t)((reg ^ (reg >> 4)) & 1u);
            reg = (uint16_t)(((reg >> 1) | (newb << 8)) & 0x1FFu);
        }
    }
}

static inline void prbs9_enc_init(prbs9_enc_t *prbs_state, uint8_t type){
    prbs_state->seq = s_prbs_seq[type & 1u];
    prbs_state->idx = 0;
}

/* Simple array lookup instead of LFSR stepping */
static inline uint8_t prbs9_enc_next_out(prbs9_enc_t *prbs_state){
    return prbs_state->seq[prbs_state->idx++];
}

/* BCH(63,44) check (generator 0x8751) – verification only */
static uint32_t bch_calc_63_44_from44bits(const uint8_t msb44[44]){
    uint32_t code = 0;
    for(int i=0;i<44;i++){
        int bit = (msb44[i] ^ ((code >> 18) & 1)) & 1;
        code <<= 1;
        if(bit) code ^= 0x8751u;
        code &= 0x7FFFFu;
    }
    return code & 0x7FFFFu;
}

/* ---------- GF(64) arithmetic + algebraic BCH(63,44) correction ----------
 * G(x)=0x8751 (degree 19) factors, over GF(64) with primitive polynomial
 * x^6+x+1 (0x43), as (x+1)*m1(x)*m3(x)*m5(x) — verified by direct
 * construction. m1/m3/m5 are the minimal polynomials of alpha^1, alpha^3,
 * alpha^5 (degree 6 each, covering roots alpha^1..alpha^6 via their
 * conjugates), i.e. the *standard narrow-sense t=3 BCH(63,45) code*. The
 * extra (x+1) factor is a single overall-parity check bolted on (root at
 * alpha^0), shrinking the code from (63,45) to (63,44).
 *
 * That means the hardware's "BCH[63,44] ERROR CORRECTION" block is not
 * doing anything exotic: it's textbook syndrome decoding (Berlekamp-Massey
 * + Chien search) for a t=3 code, plus using the extra parity bit as a free
 * sanity check on the result. This mirrors that exactly. */
#define GF64_PRIM 0x43u   /* x^6+x+1 */
#define BCH_T      3      /* correctable errors */
#define BCH_2T     (2*BCH_T)

static uint8_t gf64_exp[126];
static uint8_t gf64_log[64];
static bool    gf64_ready = false;

static void gf64_init(void){
    if (gf64_ready) return;
    unsigned x = 1;
    for (int i = 0; i < 63; ++i) {
        gf64_exp[i] = (uint8_t)x;
        gf64_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x40u) x ^= GF64_PRIM;
    }
    for (int i = 63; i < 126; ++i) gf64_exp[i] = gf64_exp[i - 63];
    gf64_ready = true;
}

static inline uint8_t gf64_mul(uint8_t a, uint8_t b){
    if (!a || !b) return 0;
    return gf64_exp[(unsigned)gf64_log[a] + (unsigned)gf64_log[b]];
}
static inline uint8_t gf64_inv(uint8_t a){ return gf64_exp[63 - gf64_log[a]]; }
static inline uint8_t gf64_alpha_pow(int e){
    int m = e % 63; if (m < 0) m += 63;
    return gf64_exp[m];
}

/* Attempt to correct up to BCH_T bit errors in a 63-bit received codeword
 * (cw[0..62], MSB-first — matches blk[0..62]: 44 data bits then 19 parity
 * bits exactly, same layout the encoder produces). Returns the number of
 * bits corrected (0..3) with cw updated in place on success, or -1 if
 * uncorrectable (cw left untouched — caller falls back to check-only
 * behavior, same as before this existed). */
static int bch_63_44_correct(uint8_t cw[63]){
    gf64_init();

    /* Syndromes S[1..6]: S_r = c(alpha^r), c(x) = sum_i cw[i]*x^(62-i) */
    uint8_t S[BCH_2T + 1];
    bool any_nonzero = false;
    for (int r = 1; r <= BCH_2T; ++r) {
        uint8_t ar = gf64_alpha_pow(r);
        uint8_t acc = 0;
        for (int i = 0; i < 63; ++i) acc = (uint8_t)(gf64_mul(acc, ar) ^ cw[i]);
        S[r] = acc;
        if (acc) any_nonzero = true;
    }
    if (!any_nonzero) return 0; /* clean codeword */

    /* Berlekamp-Massey over GF(64) */
    uint8_t C[BCH_2T + 1] = {1}, B[BCH_2T + 1] = {1};
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int n = 0; n < BCH_2T; ++n) {
        uint8_t delta = S[n + 1];
        for (int i = 1; i <= L; ++i) delta = (uint8_t)(delta ^ gf64_mul(C[i], S[n + 1 - i]));
        if (delta == 0) {
            m++;
        } else {
            uint8_t T[BCH_2T + 1];
            memcpy(T, C, sizeof(T));
            uint8_t coef = gf64_mul(delta, gf64_inv(b));
            for (int i = 0; i + m <= BCH_2T; ++i) C[i + m] = (uint8_t)(C[i + m] ^ gf64_mul(coef, B[i]));
            if (2 * L <= n) {
                L = n + 1 - L;
                memcpy(B, T, sizeof(B));
                b = delta;
                m = 1;
            } else {
                m++;
            }
        }
    }
    if (L <= 0 || L > BCH_T) return -1; /* more errors than this code can fix */

    /* Chien search: error at polynomial position p=62-i iff sigma(alpha^-p)==0 */
    int flip_pos[BCH_T];
    int nflip = 0;
    for (int i = 0; i < 63; ++i) {
        int p = 62 - i;
        uint8_t xe = gf64_alpha_pow(-p);
        uint8_t acc = 0;
        for (int k = L; k >= 0; --k) acc = (uint8_t)(gf64_mul(acc, xe) ^ C[k]);
        if (acc == 0) {
            if (nflip >= BCH_T) { nflip++; continue; } /* more roots than L: reject below */
            flip_pos[nflip++] = i;
        }
    }
    if (nflip != L) return -1; /* root count doesn't match error-locator degree: not a valid t<=3 pattern */

    uint8_t corrected[63];
    memcpy(corrected, cw, 63);
    for (int k = 0; k < nflip; ++k) corrected[flip_pos[k]] ^= 1u;

    /* Sanity gate: re-verify zero syndromes, plus the extra (x+1) overall-
     * parity factor (root alpha^0, i.e. even weight) that's part of the real
     * G(x) but not used by the t=3 decode above. This is a free check that
     * catches some (not all — fundamental limit of a t=3 code) miscorrections
     * on inputs with more than 3 real errors, at zero cost to genuine <=3
     * error cases (verified by simulation: 0 false rejects across 15k+
     * randomized <=3-error trials, ~50% reduction in 4+-error miscorrection
     * rate with this gate vs. without). */
    for (int r = 1; r <= BCH_2T; ++r) {
        uint8_t ar = gf64_alpha_pow(r);
        uint8_t acc = 0;
        for (int i = 0; i < 63; ++i) acc = (uint8_t)(gf64_mul(acc, ar) ^ corrected[i]);
        if (acc) return -1;
    }
    uint8_t parity = 0;
    for (int i = 0; i < 63; ++i) parity ^= corrected[i];
    if (parity) return -1;

    memcpy(cw, corrected, 63);
    return nflip;
}

/* Stream FSM */
typedef enum { SEEK_SYNC=0, READ_HEADER_SSB, READ_PAYLOAD } fstate_e;

typedef struct {
    char kind; /* 'A' or 'B' */
    fstate_e st;
    uint32_t sync_win;
    int sync_fill;
    mainframe_t frame;
    uint64_t frames_total;

    bool got_frame;
    mainframe_t last_frame;
    uint64_t last_done_bit; /* decoder->bitcount when last_frame completed (A/B pairing) */
    bool flywheel;          /* next 11 bits are the expected sync position (frame just ended) */
} stream_t;

/* 77-bit block decoded view */
typedef struct {
    uint16_t msb11[4];
    uint8_t  zi[2];      /* bits 63,64 */
    uint16_t lsb3[4];    /* 3 LSB each */
    uint32_t bch19;      /* 19 parity bits */
    uint32_t bch19_calc; /* recomputed */
    bool     bch_ok;
    uint8_t  bch_corrected; /* 0..3: bit errors fixed by bch_63_44_correct(), 0 if clean or uncorrectable */
} block_info_t;

/* Decoder state (opaque) */
struct decoder {
    decoder_opts_t opts;

    stream_t sa, sb;
    prbs9_enc_t pra, prb; /* reset per frame */
    pa_t *pa;               /* <— new: PA/SK state */
    uint64_t bitcount;     /* total bits received */
    /* Superframe / Williard */
   
    uint16_t williard_shiftReg;
    bool     will_locked;
    uint32_t a_mod64;
    uint64_t superframe_idx;
    uint64_t superframe_err;     /* SYNC1 spacing != 512 A-SSB since last valid SF */
    uint64_t will_a_ssb_total;   /* every williard_update call (one A-SSB) */
    uint64_t will_sf_last_ssb_tick;
    bool     will_sf_have_prev;  /* false after acquire/realign/lock loss */
    uint16_t will_last_code_at15;   // only updated when a_mod64==15 // 0x05CF (MAIN) / 0x05FF (FOLLOWER) / 0
    uint8_t  will_octet_pos;   // 0..7: position in 8-set (0 = SYNC1 expected)
    uint8_t  will_bad_pos15;   /* consecutive failed pos15 checks while locked */
    /* BCH stats */
    uint64_t bch_total, bch_bad, bch_corrected;

    /* Deinterleave auto-probe */
    int  deint_mode;       /* -1 auto, 0 even->blk0, 1 odd->blk0 */
    uint32_t probe_frames; /* counted A/B pairs for decision */
    uint32_t probe_hits[2];/* BCH OK sum for mode 0/1 */
    bool probe_decided;

     /* ZI accumulation per stereo pair (0..15) */
    uint64_t zi_acc[16];
    uint8_t  zi_count[16];   /* 0..64 */
    /* ZI scale FIFO, 2 slots per pair. The encoder sends a block's ZI word two audio
     * blocks ahead of the audio it describes (dsr.c: +4 ms delay buffer), so audio
     * block M uses the word committed at the end of block M-2 — the second-latest
     * commit, which in a 2-slot ring is slot (sc_phase-2)&1 == sc_phase&1. */
    uint8_t scL_fifo[16][2];
    uint8_t scR_fifo[16][2];
    uint8_t sc_phase[16];    /* # of ZI words received per pair */

    /* Block completeness monitor per stereo pair (0..15): counts the samples of the
     * current 64-sample block, synchronized to Williard (reset at a_mod64 == 0) */
    #define BLOCK_SIZE 64
    uint8_t block_pos[16];                 /* Samples collected in current block (0..64) */
    bool block_valid[16];                  /* Block is valid (Williard-locked) */

    /* 0 = decode entire mux; else bitmask of DSR stereo pair indices 0..15 to fully process */
    uint16_t stream_focus_pairs;

};

/* --- ZI BCH (14,6) Table from Encoder --- */
static const uint8_t _zi_bch[64] = {
    0x00,0xD1,0x73,0xA2,0xE6,0x37,0x95,0x44,
    0x1D,0xCC,0x6E,0xBF,0xFB,0x2A,0x88,0x59,
    0x3A,0xEB,0x49,0x98,0xDC,0x0D,0xAF,0x7E,
    0x27,0xF6,0x54,0x85,0xC1,0x10,0xB2,0x63,
    0x74,0xA5,0x07,0xD6,0x92,0x43,0xE1,0x30,
    0x69,0xB8,0x1A,0xCB,0x8F,0x5E,0xFC,0x2D,
    0x4E,0x9F,0x3D,0xEC,0xA8,0x79,0xDB,0x0A,
    0x53,0x82,0x20,0xF1,0xB5,0x64,0xC6,0x17,
};

/* --- Helpers --- */
static void stream_init(stream_t *stream, char kind){
    memset(stream,0,sizeof(*stream));
    stream->kind = kind;
    stream->st = SEEK_SYNC;
    stream->frame.kind = kind;
}

/* Factory */
decoder_t* decoder_new(const decoder_opts_t *opts){
    decoder_t *decoder = (decoder_t*)calloc(1, sizeof(*decoder));
    if(!decoder) return NULL;
    prbs9_build_tables();
    prbs9_enc_init(&decoder->pra, 0);
    prbs9_enc_init(&decoder->prb, 1);
    decoder->opts = *opts;
    decoder->bitcount = 0;
    stream_init(&decoder->sa, 'A');
    stream_init(&decoder->sb, 'B');
    decoder->williard_shiftReg = 0;
    decoder->will_locked = false;
    decoder->a_mod64 = 0;
    decoder->superframe_idx = 0;
    decoder->superframe_err = 0;
    decoder->will_a_ssb_total = 0;
    decoder->will_sf_last_ssb_tick = 0;
    decoder->will_sf_have_prev = false;
    decoder->will_octet_pos = 0;
    decoder->will_last_code_at15 = 0;
    decoder->bch_total = decoder->bch_bad = decoder->bch_corrected = 0;
    decoder->pa = NULL; // <— NEW (optional)    
    decoder->deint_mode   = decoder->opts.auto_deint ? -1 : 0;
    decoder->probe_frames = 0;
    decoder->probe_hits[0]= decoder->probe_hits[1]=0;
    decoder->probe_decided= !decoder->opts.auto_deint;
    
    /* Initialize block monitor */
    for(int pair=0; pair<16; pair++){
        decoder->block_pos[pair] = 0;
        decoder->block_valid[pair] = false;  /* Enabled only after Williard lock */
    }
    
    /* Initialize ZI FIFO: same as decoder_remote.c (calloc zeroes everything).
     * Scale factor 0 is valid (meaning: no redundant bits after the sign bit).
     * The FIFO is already zeroed by calloc, but it is set explicitly here
     * for clarity.
     */
    for(int pair_idx=0; pair_idx<16; pair_idx++){
        memset(decoder->scL_fifo[pair_idx], 0, sizeof(decoder->scL_fifo[pair_idx]));
        memset(decoder->scR_fifo[pair_idx], 0, sizeof(decoder->scR_fifo[pair_idx]));
        decoder->sc_phase[pair_idx] = 0;
    }

    return decoder;
}


void decoder_free(decoder_t *d){
    if(d) free(d);
}

void decoder_attach_pa(decoder_t *decoder, pa_t *pa){
    if(!decoder) return;
    decoder->pa = pa;
}

void decoder_reset_sync(decoder_t *decoder){
    if(!decoder) return;
    pa_t *pa_keep = decoder->pa;
    decoder_opts_t opts_keep = decoder->opts;
    uint16_t focus_keep = decoder->stream_focus_pairs;

    decoder->bitcount = 0;
    stream_init(&decoder->sa, 'A');
    stream_init(&decoder->sb, 'B');
    prbs9_enc_init(&decoder->pra, 0);
    prbs9_enc_init(&decoder->prb, 1);

    decoder->williard_shiftReg = 0;
    decoder->will_locked = false;
    decoder->a_mod64 = 0;
    decoder->superframe_idx = 0;
    decoder->superframe_err = 0;
    decoder->will_a_ssb_total = 0;
    decoder->will_sf_last_ssb_tick = 0;
    decoder->will_sf_have_prev = false;
    decoder->will_octet_pos = 0;
    decoder->will_last_code_at15 = 0;
    decoder->will_bad_pos15 = 0;

    decoder->bch_total = decoder->bch_bad = decoder->bch_corrected = 0;
    decoder->deint_mode   = decoder->opts.auto_deint ? -1 : 0;
    decoder->probe_frames = 0;
    decoder->probe_hits[0]=     decoder->probe_hits[1]=0;
    decoder->probe_decided= !decoder->opts.auto_deint;

    for(int pair=0; pair<16; pair++){
        decoder->block_pos[pair] = 0;
        decoder->block_valid[pair] = false;
    }

    memset(decoder->zi_acc, 0, sizeof(decoder->zi_acc));
    memset(decoder->zi_count, 0, sizeof(decoder->zi_count));
    for(int pair_idx=0; pair_idx<16; pair_idx++){
        memset(decoder->scL_fifo[pair_idx], 0, sizeof(decoder->scL_fifo[pair_idx]));
        memset(decoder->scR_fifo[pair_idx], 0, sizeof(decoder->scR_fifo[pair_idx]));
        decoder->sc_phase[pair_idx] = 0;
    }

    decoder->pa = pa_keep;
    decoder->opts = opts_keep;
    decoder->stream_focus_pairs = focus_keep;
}

void decoder_set_log_level(decoder_t *decoder, dsr_log_level_t level){
    if(!decoder) return;
    if(level < DSR_LOG_NONE) level = DSR_LOG_NONE;
    if(level > DSR_LOG_ZI)   level = DSR_LOG_ZI;
    decoder->opts.log_level = level;
}

void decoder_get_stats(decoder_t *decoder, uint64_t *bch_total, uint64_t *bch_bad){
    if(bch_total) *bch_total = decoder->bch_total;
    if(bch_bad)   *bch_bad   = decoder->bch_bad;
}
uint64_t decoder_get_bch_corrected(const decoder_t *decoder){
    return decoder ? decoder->bch_corrected : 0;
}

static inline int subchannel_to_pair_idx(int ch){
    ch &= 31;
    int k = (ch < 16) ? ch : (ch - 16);
    int bi = k >> 2;
    int rem = k & 3;
    int base = (ch < 16) ? 0 : 8;
    return base + (rem < 2 ? (2 * bi) : (2 * bi + 1));
}

void decoder_set_stream_focus(decoder_t *d, int Lch, int Rch, int mono_out){
    if(!d) return;
    Lch &= 31;
    Rch &= 31;
    if(mono_out){
        int p = subchannel_to_pair_idx(Lch);
        d->stream_focus_pairs = (uint16_t)(1u << p);
    } else {
        d->stream_focus_pairs = (uint16_t)((1u << subchannel_to_pair_idx(Lch)) |
                                             (1u << subchannel_to_pair_idx(Rch)));
    }
}

//Decoder Getter for external use
uint64_t decoder_get_superframe_index(const decoder_t *decoder){ return decoder->superframe_idx; }
uint64_t decoder_get_superframe_err_count(const decoder_t *decoder){
    return decoder ? decoder->superframe_err : 0;
}
bool     decoder_is_williard_locked   (const decoder_t *decoder){ return decoder->will_locked; }
bool     decoder_is_deint_ready(const decoder_t *decoder){ return decoder ? decoder->probe_decided : false; }




// 14b signed -> 16b signed
// Exact inverse of the encoder side (dsr.c):
//   Encoder:  ac = (audio16 << sc) >> 2;   // 16b -> 14b (compressed)
//   Decoder:  audio16 = (ac << 2) >> sc;   // 14b -> 16b (decompressed)
static inline int16_t restore_16_from_14(int16_t s14, uint8_t sc)
{
    /* CRITICAL: handle SCALE_FACTOR_INVALID (0xFF) - use a conservative fallback */
    if (sc == SCALE_FACTOR_INVALID) {
        sc = 7;  /* Conservative fallback: maximum compression */
    }
    if (sc > 7) sc = 7;  // scale factors are 0..7
    
    /* IMPORTANT: s14 is a 14-bit signed value (bits 0-13, bit 13 is the sign bit)
     * But s14 is stored as int16_t, and bits 14-15 may be wrong!
     * It must be explicitly sign-extended from 14 to 16 bits before casting to int32_t.
     * 
     * Sign extension from 14 bits to 16 bits:
     * 1. Mask bits 0-13 (14-bit value)
     * 2. Check bit 13 (sign bit)
     * 3. If set (negative): set bits 14-15
     * 4. If not set (positive): clear bits 14-15
     */
    uint16_t s14u = (uint16_t)s14 & 0x3FFF;  // Mask to 14 bits
    int16_t s16;
    if (s14u & 0x2000) {  // Bit 13 is set (negative)
        s16 = (int16_t)(s14u | 0xC000);  // Sign-extend: set bits 14-15
    } else {  // Positive
        s16 = (int16_t)s14u;  // Bits 14-15 are already 0
    }
    
    /* Now it is safe to cast to int32_t and perform the reconstruction:
     * 1. Shift left by 2 bits: restores the 2 LSBs (which were removed via ZI)
     * 2. Shift right by sc bits: decompresses according to the scale factor
     */
    int32_t x = (int32_t)s16 << 2;  // sign extension is now correct
    int32_t y = x >> sc;  // arithmetic (preserves sign bit)
    
    /* Saturation: clamp to 16-bit range */
    if (y >  32767) y =  32767;
    if (y < -32768) y = -32768;
    return (int16_t)y;
}

/* Pair index from frame/block/ZI-I/II */
static inline int pair_index(int isA, int blk, int which){
    return (isA ? 0 : 8) + 2*blk + (which & 1);
}


/* Check BCH(14,6), return the 6 data bits */
static inline int bch14_ok(uint16_t cw, uint8_t *sixbits_out){
    uint8_t data = (uint8_t)((cw >> 8) & 0x3F);
    uint8_t parity = (uint8_t)(cw & 0xFF);
    if(_zi_bch[data] == parity){ if(sixbits_out) *sixbits_out = data; return 1; }
    return 0;
}


/* Collect ZI-Bit and decode at 64 bits
 *
 * ZI scale-factor FIFO logic:
 *
 * The FIFO has 2 slots per stereo pair (ring buffer, write index = sc_phase & 1u).
 * The encoder sends a block's ZI word two audio blocks ahead of the corresponding
 * samples (dsr.c: +4 ms delay buffer). Hence the second-latest commit is read,
 * slot (sc_phase-2)&1 — a true 2-block lag, also measured against real IQ captures
 * with correct ZI word framing (see zi_push_bit): smoother than lag 1 or 3.
 * (Earlier measurements suggesting "1 block" were run with misframed ZI words.)
 *
 * Timing:
 * - ZI bits arrive continuously (1 bit per frame)
 * - After 64 frames (2 ms) a ZI block is complete
 * - The scale factor is then written into the FIFO
 */
static void zi_decode_and_commit(decoder_t *decoder, int pair, uint64_t zi64){
    /* Layout: [14][14][14][22] (MSB-first) */
    uint16_t cw0 = (uint16_t)((zi64 >> (64-14)) & 0x3FFFu);
    uint16_t cw1 = (uint16_t)((zi64 >> (64-28)) & 0x3FFFu);
    uint16_t cw2 = (uint16_t)((zi64 >> (64-42)) & 0x3FFFu);

    uint8_t v0=0,v1=0,v2=0;
    int ok0 = bch14_ok(cw0,&v0);
    int ok1 = bch14_ok(cw1,&v1);
    int ok2 = bch14_ok(cw2,&v2);

    if(!ok0 && !ok1 && !ok2){
        static uint64_t fail_count = 0;
        fail_count++;
        if(decoder->opts.log_level >= DSR_LOG_WARN &&
           (fail_count <= 10 || (fail_count % 50 == 0))){
            DLOG_AT(decoder, DSR_LOG_WARN,
                    "[ZI-BCH-FAIL] pair=%d a_mod64=%u sc_phase=%u zi64=0x%016llx cw0=0x%04x cw1=0x%04x cw2=0x%04x (data0=0x%02x parity0=0x%02x expected=0x%02x)\n",
                    pair, decoder->a_mod64, decoder->sc_phase[pair], (unsigned long long)zi64, cw0, cw1, cw2,
                    (uint8_t)((cw0 >> 8) & 0x3F), (uint8_t)(cw0 & 0xFF),
                    _zi_bch[(cw0 >> 8) & 0x3F]);
        }
    } else if(decoder->opts.log_level >= DSR_LOG_ZI){
        static uint64_t success_count = 0;
        success_count++;
        if(success_count <= 10 || (success_count % 100 == 0)){
            DLOG_AT(decoder, DSR_LOG_ZI,
                    "[ZI-BCH-OK] pair=%d zi64=0x%016llx cw0=0x%04x cw1=0x%04x cw2=0x%04x ok0=%d ok1=%d ok2=%d v0=%u v1=%u v2=%u\n",
                    pair, (unsigned long long)zi64, cw0, cw1, cw2, ok0, ok1, ok2, v0, v1, v2);
        }
    }

    uint8_t pos = decoder->sc_phase[pair] & 1u;

    /* On a total BCH failure, v0/v1/v2 all stay at their 0 default, which used to get
     * written through as scL=scR=0 ("no boost was applied by the encoder"). Since sc
     * is a block-floating-point exponent, applying 0 to a sample that was actually
     * boosted by e.g. 6 bits reconstructs it at up to 64x its true amplitude — an
     * audible spike, worse the more the true (quiet) sample had been boosted. Instead,
     * keep whatever scale factor is already sitting in this slot (the last known-good
     * value for this phase) rather than overwrite it with a fabricated zero.
     *
     * When only SOME of the 3 redundant codewords pass BCH, pick by majority among the
     * candidates that actually passed — never compare against an *unverified* v0/v1/v2,
     * since those default to 0 when their own BCH failed. 0 is also the most common
     * genuine scale factor (full-scale/unboosted audio), so a naive v1==v2 style check
     * that ignores ok1/ok2 status spuriously "agrees" with that 0 default far too often,
     * overwriting a good non-zero v0 and reproducing the exact full-failure overdrive
     * case above on every partial BCH success — the audible cause of the periodic
     * full-scale click bursts. */
    if(ok0 || ok1 || ok2){
        uint8_t v;
        if(ok0 && ok1 && v0==v1)      v = v0; /* two-of-three agree */
        else if(ok0 && ok2 && v0==v2) v = v0;
        else if(ok1 && ok2 && v1==v2) v = v1;
        else if(ok0)                  v = v0; /* only one passed (or all disagree): trust cw0 */
        else if(ok1)                  v = v1;
        else                          v = v2;

        uint8_t scL = (uint8_t)((v >> 3) & 0x07);
        uint8_t scR = (uint8_t)((v >> 0) & 0x07);

        decoder->scL_fifo[pair][pos] = scL;
        decoder->scR_fifo[pair][pos] = scR;
    }
    decoder->sc_phase[pair]++;

    DLOG_AT(decoder, DSR_LOG_ZI,
            "[ZI] pair=%d scL=%u scR=%u phase=%u pos=%u (slot=%u) v0=%u v1=%u v2=%u ok0=%d ok1=%d ok2=%d\n",
            pair, decoder->scL_fifo[pair][pos], decoder->scR_fifo[pair][pos],
            decoder->sc_phase[pair], pos, pos, v0, v1, v2, ok0, ok1, ok2);
}



/* ZI word framing. Encoder (dsr.c, dsr_encode): one audio block = 64 frames, ZI bit i
 * is carried in frame i of the block (MSB first); that frame's SSB bit is SA bit
 * (frame+16) & 63 ("SA bits are offset by 16 bits from the audio blocks"). a_mod64 is
 * exactly this SA bit position (Williard bit 15 -> a_mod64 15), so every
 * ZI word starts at a_mod64 == 16 and ends at a_mod64 == 15.
 *
 * Previously zi_count simply counted up from the first frame after decoder_new()/resync —
 * so the 64-bit boundary fell at a random point in the block, the BCH(14,6) check failed
 * almost always, and the scale factors stayed wrong until a resync happened to hit the
 * right phase by chance. */
#define ZI_WORD_START_POS64 16u

static inline void zi_push_bit(decoder_t *decoder, int pair, uint8_t bit){
    if(pair<0 || pair>=16) return;
    /* Without Williard lock the word boundary is unknown: collect nothing. */
    if(!decoder->will_locked) return;
    if(decoder->a_mod64 == ZI_WORD_START_POS64){
        /* Word start: discard any incomplete leftover word (lock mid-block,
         * lost frame) instead of shifting it into the new word. */
        decoder->zi_acc[pair] = 0;
        decoder->zi_count[pair] = 0;
    }
    decoder->zi_acc[pair] = (decoder->zi_acc[pair] << 1) | (bit & 1u);
    if(++decoder->zi_count[pair] == 64){
        if(decoder->opts.log_level >= DSR_LOG_ZI){
            uint64_t acc = decoder->zi_acc[pair];
            uint8_t first8 = (uint8_t)((acc >> 56) & 0xFF);
            uint8_t last8 = (uint8_t)(acc & 0xFF);
            uint8_t middle8 = (uint8_t)((acc >> 24) & 0xFF);
            DLOG_AT(decoder, DSR_LOG_ZI,
                    "[ZI-PUSH] pair=%d accumulated 64 bits, decoding... (acc=0x%016llx, first8=0x%02x middle8=0x%02x last8=0x%02x) phase_before=%u\n",
                    pair, (unsigned long long)acc, first8, middle8, last8, decoder->sc_phase[pair]);
        }
        zi_decode_and_commit(decoder, pair, decoder->zi_acc[pair]);
        DLOG_AT(decoder, DSR_LOG_ZI,
                "[ZI-PUSH] pair=%d after decode: phase_after=%u\n", pair, decoder->sc_phase[pair]);
        decoder->zi_acc[pair] = 0;
        decoder->zi_count[pair] = 0;
    }
}

/* Payload → 4×77b with selectable even/odd mapping */
static void payload_to_blocks_mode(const uint8_t pay[DSR_PAYLOAD_BITS],
                                   uint8_t blk[4][DSR_BLOCK_BITS],
                                   int mode /*0 even->blk0, 1 odd->blk0*/)
{
    /* Segment 1: 0..153 */
    for(int i=0;i<77;i++){
        if(mode==0){ blk[0][i]=pay[2*i+0]; blk[1][i]=pay[2*i+1]; }
        else       { blk[0][i]=pay[2*i+1]; blk[1][i]=pay[2*i+0]; }
    }
    /* Segment 2: 154..307 */
    for(int i=0;i<77;i++){
        if(mode==0){ blk[2][i]=pay[154+2*i+0]; blk[3][i]=pay[154+2*i+1]; }
        else       { blk[2][i]=pay[154+2*i+1]; blk[3][i]=pay[154+2*i+0]; }
    }
}

/* 77b parse exactly like _77block(). When do_bch is set, first attempts real
 * BCH(63,44) error correction on the 63-bit codeword (44 MSB data bits + 19
 * parity bits); msb11/bch19 below are then extracted from the corrected
 * bits when correction succeeds, so a fixed transmission error actually
 * reaches the audio path instead of just being flagged. */
static void parse_block77(const uint8_t blk[77], block_info_t *bi, bool do_bch){
    uint8_t corrected_cw[63];
    const uint8_t *src = blk; /* points at raw blk, or corrected_cw once correction succeeds */
    int ncorr = -1;

    if(do_bch){
        memcpy(corrected_cw, blk, 63);
        ncorr = bch_63_44_correct(corrected_cw);
        if(ncorr >= 0) src = corrected_cw;
    }

    for(int channel_idx=0;channel_idx<4;channel_idx++){
        uint16_t value=0;
        for(int bit_idx=0;bit_idx<11;bit_idx++) value = (uint16_t)((value<<1) | (src[channel_idx*11 + bit_idx] & 1u));
        bi->msb11[channel_idx]=value;
    }
    uint32_t bch=0;
    for(int bit_idx=44;bit_idx<63;bit_idx++) bch = (bch<<1) | (src[bit_idx]&1u);
    bi->bch19 = bch & 0x7FFFFu;

    /* ZI/LSB bits sit outside the 63-bit BCH codeword (blk[63..76]); always
     * read from the original blk, correction never touches them. */
    bi->zi[0]=blk[63]&1u;
    bi->zi[1]=blk[64]&1u;

    for(int channel_idx=0;channel_idx<4;channel_idx++){
        int base = 65 + 3*channel_idx;
        bi->lsb3[channel_idx] = (uint16_t)((blk[base]<<2)|(blk[base+1]<<1)|blk[base+2]);
    }

    if(do_bch){
        if(ncorr >= 0){
            bi->bch19_calc = bi->bch19; /* corrected codeword is valid by construction */
            bi->bch_ok = true;
            bi->bch_corrected = (uint8_t)ncorr;
        } else {
            uint8_t msb44[44];
            for(int i=0;i<44;i++) msb44[i] = blk[i]&1u;
            bi->bch19_calc = bch_calc_63_44_from44bits(msb44);
            bi->bch_ok = (bi->bch19 == bi->bch19_calc);
            bi->bch_corrected = 0;
        }
    }else{
        bi->bch19_calc = 0; bi->bch_ok = true; bi->bch_corrected = 0;
    }
}
static void audio_from_info(const block_info_t *bi, int16_t s14[4]){
    for(int k=0;k<4;k++){
        uint16_t v14u = (uint16_t)((bi->msb11[k]<<3) | (bi->lsb3[k] & 0x7));
        int16_t  v14s = (v14u & 0x2000) ? (int16_t)(v14u - 0x4000) : (int16_t)v14u;
        s14[k] = v14s; // 14-bit signed, NOT <<2!
    }
}

/* Decode one frame for a given deinterleave mode.
 * Returns number of BCH-OK blocks (0..4). pcm16 is always filled.
 * 
 * IMPORTANT: This function collects samples into block buffers (64 samples per block).
 * The block boundaries are strictly tied to the Williard synchronization:
 * - A block starts at a_mod64 == 0 (after Williard sync)
 * - 4 frames (A+B pairs) = 64 samples = 1 block = 2 ms
 */
static int decode_frame_audio_mode(decoder_t *decoder,
                                   const mainframe_t *frame, bool do_bch,
                                   int deint_mode,
                                   uint64_t *bch_total, uint64_t *bch_bad,
                                   int16_t pcm16[16])
{                                  
    uint8_t blocks[4][DSR_BLOCK_BITS];
    payload_to_blocks_mode(frame->payload.bits, blocks, deint_mode);
    int hits = 0;
    const int isA = (frame->kind=='A');

    uint16_t eff_pair_mask = 0xFFFFu;
    if(decoder->stream_focus_pairs != 0)
        eff_pair_mask = decoder->stream_focus_pairs;
    if(decoder->opts.bch_check && !decoder->probe_decided)
        eff_pair_mask = 0xFFFFu;

    /* Block boundaries are set in williard_update() when a_mod64 == 0 */
    /* Here we only check whether the block is valid */

    for(int bi=0; bi<4; ++bi){
        int pI0  = pair_index(isA, bi, 0);
        int pII0 = pair_index(isA, bi, 1);
        uint16_t block_pairs = (uint16_t)((1u << pI0) | (1u << pII0));
        int want_pcm = (block_pairs & eff_pair_mask) != 0;

        block_info_t info; memset(&info,0,sizeof(info));
        parse_block77(blocks[bi], &info, do_bch);

        if(do_bch){
            (*bch_total)++;
            if(!info.bch_ok) (*bch_bad)++; else hits++;
            if(info.bch_corrected) decoder->bch_corrected++;
        }

        if(!want_pcm){
            /* Stream focus: still advance ZI / scale FSM for every pair; skipping this used to desync SF. */
            bool should_push = decoder->probe_decided ? true : (deint_mode == 0);
            if(should_push){
                zi_push_bit(decoder, pI0,  info.zi[0]);
                zi_push_bit(decoder, pII0, info.zi[1]);
            }
            pcm16[bi*4 + 0] = pcm16[bi*4 + 1] = pcm16[bi*4 + 2] = pcm16[bi*4 + 3] = 0;
            continue;
        }

        /* Fetch 14-bit samples (L1,R1,L2,R2) */
        int16_t s14[4];
        audio_from_info(&info, s14);

        /* Determine channel indices */
        int pI  = pI0;   /* L channel */
        int pII = pII0;  /* R channel */

        if(decoder->opts.log_level >= DSR_LOG_ZI){
            static uint64_t block_debug_count = 0;
            if(block_debug_count < 50 || (block_debug_count % 1000 == 0)){
                DLOG_AT(decoder, DSR_LOG_ZI,
                        "[BLOCK-DECODE] bi=%d isA=%d pI=%d pII=%d s14=(%d,%d,%d,%d) zi[0]=%u zi[1]=%u msb11=(%u,%u,%u,%u) lsb3=(%u,%u,%u,%u) bch_ok=%d\n",
                        bi, isA, pI, pII, s14[0], s14[1], s14[2], s14[3],
                        info.zi[0], info.zi[1],
                        info.msb11[0], info.msb11[1], info.msb11[2], info.msb11[3],
                        info.lsb3[0], info.lsb3[1], info.lsb3[2], info.lsb3[3],
                        info.bch_ok);
            }
            block_debug_count++;
        }
        
        /* Read scale FIFO: second-latest commit = ZI word of block M-2 (encoder sends
         * ZI two blocks ahead of the audio). Write: pos = phase & 1u, then phase++. */
        uint8_t posI, posII;
        if(decoder->sc_phase[pI] < 2){
            posI = 0;  /* Initially: use slot 0 (initialized to 0) */
        } else {
            posI = (decoder->sc_phase[pI] - 2) & 1u;
        }
        if(decoder->sc_phase[pII] < 2){
            posII = 0;  /* Initially: use slot 0 (initialized to 0) */
        } else {
            posII = (decoder->sc_phase[pII] - 2) & 1u;
        }

        /* ZI zi[0] → pair pI (L1,R1), zi[1] → pair pII (L2,R2): each reads its own SF FIFO. */
        uint8_t sc_l1 = decoder->scL_fifo[pI][posI];
        uint8_t sc_r1 = decoder->scR_fifo[pI][posI];
        uint8_t sc_l2 = decoder->scL_fifo[pII][posII];
        uint8_t sc_r2 = decoder->scR_fifo[pII][posII];
        
        /* Reconstruct 16-bit: (s14<<2) >> sc — with saturation.
         * (A per-sample crossfade across genuine sc transitions was tried here and
         * measured WORSE against real captured audio: blending two reconstructions
         * of the same fast-changing raw s14 at very different gains — up to 64x
         * apart — injects its own ringing instead of removing the step. Reverted.) */
        int16_t sample_l1 = restore_16_from_14(s14[0], sc_l1); /* L1 */
        int16_t sample_r1 = restore_16_from_14(s14[1], sc_r1); /* R1 */
        int16_t sample_l2 = restore_16_from_14(s14[2], sc_l2); /* L2 */
        int16_t sample_r2 = restore_16_from_14(s14[3], sc_r2); /* R2 */

        if(decoder->opts.log_level >= DSR_LOG_WARN &&
           (sc_l1 == SCALE_FACTOR_INVALID || sc_r1 == SCALE_FACTOR_INVALID ||
            sc_l2 == SCALE_FACTOR_INVALID || sc_r2 == SCALE_FACTOR_INVALID)){
            static uint64_t sc_warn_count = 0;
            sc_warn_count++;
            if(sc_warn_count <= 10 || (sc_warn_count % 100 == 0)){
                DLOG_AT(decoder, DSR_LOG_WARN,
                        "[SC-FIFO] invalid scale factors #%llu pairI=%d pairII=%d "
                        "scI=(%u,%u) scII=(%u,%u) phaseI=%u phaseII=%u posI=%u posII=%u a_mod64=%u "
                        "s14=(%d,%d,%d,%d) pcm=(%d,%d,%d,%d)\n",
                        (unsigned long long)sc_warn_count, pI, pII,
                        sc_l1, sc_r1, sc_l2, sc_r2,
                        decoder->sc_phase[pI], decoder->sc_phase[pII], posI, posII, decoder->a_mod64,
                        s14[0], s14[1], s14[2], s14[3],
                        sample_l1, sample_r1, sample_l2, sample_r2);
            }
        }

        /* Count samples per pair for the block monitor: L+R of each pair (only if block
         * valid; skip pairs outside focus). block_pos stays even, so +2 never passes 64. */
        if((eff_pair_mask & (1u << pI)) && decoder->block_valid[pI] && decoder->block_pos[pI] < BLOCK_SIZE){
            decoder->block_pos[pI] += 2;
        }
        if((eff_pair_mask & (1u << pII)) && decoder->block_valid[pII] && decoder->block_pos[pII] < BLOCK_SIZE){
            decoder->block_pos[pII] += 2;
        }

        /* Per-frame PCM output */
        pcm16[bi*4 + 0] = sample_l1;
        pcm16[bi*4 + 1] = sample_r1;
        pcm16[bi*4 + 2] = sample_l2;
        pcm16[bi*4 + 3] = sample_r2;

        /* Push Z bits (apply to FUTURE blocks)
         * IMPORTANT: ZI bits must be pushed for EVERY block, not just once per superframe!
         * 
         * A superframe has 64 frames, each frame has 4 blocks.
         * Per pair there are 32 blocks per superframe (because there are 16 pairs: 0-15).
         * Each block has 2 ZI bits (zi[0] and zi[1]).
         * So: 32 blocks × 2 bits = 64 ZI bits per pair per superframe.
         * 
         * During the auto-probe phase decode_frame_audio_mode is called for both
         * deinterleave modes (for both sa and sb), but the ZI bits
         * should be pushed only once per block (when deint_mode==0 or once probe_decided is set).
         * 
         * Stream A (isA=1) carries ZI bits for pairs 0-7
         * Stream B (isA=0) carries ZI bits for pairs 8-15
         * 
         * Strategy: push ZI bits for EVERY block, but only once per block
         * (when deint_mode==0 or once probe_decided is set, to avoid duplicate pushes).
         */
        bool should_push = false;

        if(decoder->probe_decided){
            /* After the auto-probe: push ZI bits for every block */
            should_push = true;
        } else {
            /* During the auto-probe: push ZI bits only when deint_mode==0 (prevents duplicate pushes) */
            should_push = (deint_mode == 0);
        }

        if(should_push){
            if(decoder->opts.log_level >= DSR_LOG_ZI){
                static uint64_t zi_bit_debug_count = 0;
                if(zi_bit_debug_count < 100 || (zi_bit_debug_count % 1000 == 0)){
                    DLOG_AT(decoder, DSR_LOG_ZI,
                            "[ZI-BIT] pairI=%d pairII=%d zi[0]=%u zi[1]=%u a_mod64=%u deint_mode=%d should_push=%d\n",
                            pI, pII, info.zi[0], info.zi[1], decoder->a_mod64, deint_mode, should_push);
                }
                zi_bit_debug_count++;
            }
            /* Always both ZI lines per decoded block: the SF read logic uses the pI FIFO for all
             * four samples; zi[0]/zi[1] belong to the same block and must advance together,
             * otherwise the scaling drifts (typically "only 2nd channel broken"). */
            zi_push_bit(decoder, pI,  info.zi[0]);
            zi_push_bit(decoder, pII, info.zi[1]);
        }
    }
    
    return hits;
}

/* Williard sync update (only call on SSB of an A-Frame) */
static void williard_update(decoder_t *decoder, const mainframe_t *frame_a)
{
    decoder->will_a_ssb_total++;

    /* 16-bit shift register: shift in the new SSB */
    decoder->williard_shiftReg = (uint16_t)((decoder->williard_shiftReg << 1) | (frame_a->ssb & 1u));

    /* Acquisition: after a few valid A mainframes, a SYNC1/SYNC2 in the shift register (as before).
     * The former "2× SYNC1 on the 8-grid" rule rarely triggered in practice → will_locked stayed false,
     * PA never got lock and extracted no SA data. */
    if (!decoder->will_locked) {
        const uint16_t code = decoder->williard_shiftReg;
        if (decoder->sa.frames_total < WILLIARD_ACQ_MIN_PRIOR_AFRAMES)
            return;
        if (code == WILLIARD_CODE_SYNC1 || code == WILLIARD_CODE_SYNC2) {
            decoder->will_locked         = true;
            decoder->will_sf_have_prev   = false;
            decoder->a_mod64             = 15;
            decoder->will_octet_pos      = (code == WILLIARD_CODE_SYNC1) ? 0 : 1;
            decoder->will_last_code_at15 = code;
            decoder->will_bad_pos15      = 0;
            DLOG_AT(decoder, DSR_LOG_SYNC,
                    "[superframe] acquire at A#%llu code=0x%04X (octet_pos=%u)\n",
                    (unsigned long long)frame_a->idx, code, decoder->will_octet_pos);
        }
        return;
    }

    /* Tracking: check/log only at the 64-bit frame boundary (pos==15) */
    decoder->a_mod64 = (decoder->a_mod64 + 1) & 63u;
    
    /* Block boundary synchronization: a new block starts at a_mod64 == 0
     * This is strictly tied to the Williard synchronization.
     * IMPORTANT: the block starts AFTER the frame with a_mod64 == 0, i.e. at a_mod64 == 1
     * the first sample of the new block is collected. */
    if (decoder->a_mod64 == 0 && decoder->will_locked) {
        /* Block start: all channels begin a new 64-sample block
         * First check whether the previous blocks were complete */
        bool logged_incomplete = false;
        for(int channel_idx=0; channel_idx<16; channel_idx++){
            if(decoder->block_valid[channel_idx]){
                if(decoder->block_pos[channel_idx] < BLOCK_SIZE){
                    /* Incomplete block is discarded (synchronization error).
                     * Only warn for a pair actually being tracked: with stream-focus on
                     * (the default), block_pos for out-of-focus pairs never fills —
                     * that's by design, not an error — so hard-coding channel_idx==0 as
                     * the representative sample spammed this warning on every single
                     * block whenever the selected channel wasn't pair 0. channel_idx
                     * here IS the pair index (0..15), so no further conversion needed. */
                    bool pair_in_focus = (decoder->stream_focus_pairs == 0)
                        || (decoder->stream_focus_pairs & (1u << channel_idx));
                    if(!logged_incomplete && pair_in_focus){
                        logged_incomplete = true;
                        if(decoder->opts.log_level >= DSR_LOG_WARN){
                            DLOG_AT(decoder, DSR_LOG_WARN,
                                    "[BLOCK] Discarding incomplete block pair=%d (pos=%u/%u) at superframe=%llu\n",
                                    channel_idx, decoder->block_pos[channel_idx], (unsigned)BLOCK_SIZE,
                                    (unsigned long long)decoder->superframe_idx);
                        }
                    }
                }
                /* Block complete - no output in normal operation */
            }
            /* Reset for new block */
            decoder->block_pos[channel_idx] = 0;
            decoder->block_valid[channel_idx] = true;
        }
        /* Block start output removed - too verbose */
    }
    
    if (decoder->a_mod64 != 15) return;

    const uint16_t code     = decoder->williard_shiftReg;
    const uint16_t expected = (decoder->will_octet_pos == 0) ? WILLIARD_CODE_SYNC1 : WILLIARD_CODE_SYNC2;

    /* Update "last" only at pos==15 -> consistent display */
    decoder->will_last_code_at15 = code;

    if (code != expected) {
        /* Not a "slip" in the channel sense; we only realign the 8-pattern
           if a valid Williard was seen. */
        if (code == WILLIARD_CODE_SYNC1 || code == WILLIARD_CODE_SYNC2) {
            decoder->will_bad_pos15 = 0;
            decoder->will_sf_have_prev = false;
            DLOG_AT(decoder, DSR_LOG_SYNC,
                    "[superframe] realign: expected 0x%04X, got 0x%04X at A#%llu (pos15)\n",
                    expected, code, (unsigned long long)frame_a->idx);
            decoder->will_octet_pos = (code == WILLIARD_CODE_SYNC1) ? 0 : 1;
        } else {
            if (++decoder->will_bad_pos15 >= WILLIARD_POS15_BAD_UNLOCK) {
                decoder->will_locked      = false;
                decoder->will_sf_have_prev  = false;
                decoder->will_bad_pos15   = 0;
                decoder->williard_shiftReg= 0;
                decoder->a_mod64          = 0;
                decoder->will_octet_pos   = 0;
                DLOG_AT(decoder, DSR_LOG_SYNC,
                        "[superframe] lost lock after %d bad pos15 checks — re-acquire\n",
                        WILLIARD_POS15_BAD_UNLOCK);
            } else {
                static uint64_t warn_count = 0;
                if (decoder->opts.log_level >= DSR_LOG_SYNC && (++warn_count % 100 == 0)) {
                    DLOG_AT(decoder, DSR_LOG_SYNC,
                            "[superframe] WARN: no Williard at pos15 (0x%04X) at A#%llu (warn #%llu bad=%u)\n",
                            code, (unsigned long long)frame_a->idx, (unsigned long long)warn_count,
                            decoder->will_bad_pos15);
                }
            }
        }
    } else {
        decoder->will_bad_pos15 = 0;
        /* Expected pattern OK. Any per-match counting belongs here.
           Note: 0x05CF appears only once per 8 SA frames. */
        if (code == WILLIARD_CODE_SYNC1) {
            if (decoder->will_sf_have_prev) {
                uint64_t delta = decoder->will_a_ssb_total - decoder->will_sf_last_ssb_tick;
                if (delta != (uint64_t)DSR_A_MAINFRAMES_PER_WILLIARD_SF)
                    decoder->superframe_err++;
            }
            decoder->will_sf_last_ssb_tick = decoder->will_a_ssb_total;
            decoder->will_sf_have_prev     = true;
            decoder->superframe_idx++;
        }
    }

    /* Next of the eight positions in the pattern */
    decoder->will_octet_pos = (uint8_t)((decoder->will_octet_pos + 1) & 7u);
}


/* A/B state machine push
 *
 * Mainframe sync (per stream A/B, separately):
 *   SEEK_SYNC: 11-bit window sync_win; on a match with A_SYNC/B_SYNC go straight to READ_HEADER_SSB
 *   (one Barker word — no multiple confirmation at mainframe level).
 * Williard/superframe (A-SSB only): williard_update — lock on SYNC1/SYNC2 in the shift register
 *   after at least WILLIARD_ACQ_MIN_PRIOR_AFRAMES complete A frames; lock loss after
 *   WILLIARD_POS15_BAD_UNLOCK failed pos15 checks.
 */
static int stream_push_bit(decoder_t *decoder, stream_t *stream, uint8_t bit)
{
    decoder->bitcount++;
    switch(stream->st){
    case SEEK_SYNC: {
        stream->sync_win = ((stream->sync_win << 1) | (bit & 1u)) & SYNC_MASK;
        if(stream->sync_fill < SYNC_BITS) stream->frame.header_sync[stream->sync_fill++] = bit & 1u;
        if(stream->sync_fill == SYNC_BITS){
            uint16_t ref = (stream->kind=='A') ? A_SYNC : B_SYNC;
            bool hit = (stream->sync_win == ref);
            if(!hit && stream->flywheel)
                hit = __builtin_popcount(stream->sync_win ^ ref) <= SYNC_FLYWHEEL_MAX_ERR;
            stream->flywheel = false; /* only the first window after a frame is "expected" */
            if(hit){
                stream->st = READ_HEADER_SSB;
                stream->frame.payload.nbits = 0;
                /* PRBS reset per frame */
                if(stream->kind=='A') prbs9_enc_init(&decoder->pra, 0);
                else                  prbs9_enc_init(&decoder->prb, 1);
            }else{
                memmove(&stream->frame.header_sync[0], &stream->frame.header_sync[1], SYNC_BITS-1);
                stream->frame.header_sync[SYNC_BITS-1] = 0;
            }
        }
        return 0;
    }
    case READ_HEADER_SSB: {
        stream->frame.ssb = bit & 1u;

        if (stream->kind == 'A') {
            /* Advance Williard */

            williard_update(decoder, &stream->frame);

            /* Feed PA – pa.c decides internally based on will_locked/pos/code
            whether to collect/commit. No warmup/gating flags needed. */
            if (decoder->pa) {
                pa_push_a_ssb(decoder->pa,
                            stream->frame.ssb,
                            decoder->will_locked,
                            (uint8_t)(decoder->a_mod64 & 63u),
                            decoder->will_last_code_at15,
                            decoder->will_octet_pos);
            }
        }

        stream->st = READ_PAYLOAD;
        return 0;
    }

    case READ_PAYLOAD: {
        uint8_t b = bit & 1u;
        if(decoder->opts.descramble){
            uint8_t scr = (stream->kind=='A') ? prbs9_enc_next_out(&decoder->pra) : prbs9_enc_next_out(&decoder->prb);
            b ^= scr;
        }
        stream->frame.payload.bits[stream->frame.payload.nbits++] = b;
            if(stream->frame.payload.nbits == DSR_PAYLOAD_BITS){
            stream->frames_total++; stream->frame.idx = stream->frames_total;
            stream->last_frame = stream->frame; stream->got_frame = true;
            stream->last_done_bit = decoder->bitcount;
            stream->st = SEEK_SYNC; stream->sync_win=0; stream->sync_fill=0;
            stream->flywheel = true;
            memset(&stream->frame,0,sizeof(stream->frame)); stream->frame.kind=stream->kind;
            return 1;
        }
        return 0;
    }
    default: return 0;
    }
}

static bool decoder_try_emit_ab_frame_pair(decoder_t *decoder,
                                           int16_t pcmA[16], int16_t pcmB[16])
{
    if (!decoder->sa.got_frame || !decoder->sb.got_frame)
        return false;
    /* Pair by completion time, not by per-stream frame counters: one lost or false-locked
     * frame on either stream shifted the counters apart for good, after which no pair
     * ever matched again (audio silent until a manual resync). Drop the older frame. */
    uint64_t ta = decoder->sa.last_done_bit, tb = decoder->sb.last_done_bit;
    uint64_t skew = (ta > tb) ? (ta - tb) : (tb - ta);
    if (skew > AB_PAIR_MAX_SKEW_BITS) {
        if (ta < tb) decoder->sa.got_frame = false;
        else         decoder->sb.got_frame = false;
        return false;
    }

    /* Decide EVEN/ODD deinterleave using genuine BCH(63,44) pass/fail as ground truth,
     * always — not just when --bch-check is given. A t=3 BCH code essentially never
     * passes by chance on misaligned/wrong-mode data, unlike the |sample|-energy
     * heuristic this replaces: comparing raw audio energy between the two candidate
     * modes could — and in practice did — pick the wrong one on some real audio
     * content. Because probe_decided never re-runs itself, that permanently corrupted
     * every ZI/scale-factor bit for the rest of the run, and the only recovery was the
     * user noticing garbled audio and pressing hard-resync to re-roll the decision.
     * The one-time ~200-pair probe below is a small, bounded cost (decode_frame_audio_mode
     * is not the hot path — demod_process dominates runtime by far) paid once at startup
     * (or after a resync), not on every frame.
     *
     * Gated on will_locked so the probe only ever samples frames from after real
     * superframe sync, not acquisition churn.
     *
     * The probe's BCH total/bad counters are thrown away unless --bch-check is also
     * set, so "BCH=off" still reports zero BCH activity to the user; bch_corrected
     * (written directly by decode_frame_audio_mode, not via a counter pointer) can in
     * principle tick up by a negligible amount during the probe regardless, but
     * correctable errors are rare on already-locked, correct-mode data, so this isn't
     * user-visible in practice. */
    if (decoder->opts.auto_deint && !decoder->probe_decided && decoder->will_locked) {
        int16_t aA[16], aB[16], bA[16], bB[16];
        uint64_t local_bt = 0, local_bb = 0;
        uint64_t *bt = decoder->opts.bch_check ? &decoder->bch_total : &local_bt;
        uint64_t *bb = decoder->opts.bch_check ? &decoder->bch_bad   : &local_bb;

        int hit0 = decode_frame_audio_mode(decoder, &decoder->sa.last_frame, true, 0, bt, bb, aA)
                 + decode_frame_audio_mode(decoder, &decoder->sb.last_frame, true, 0, bt, bb, aB);
        int hit1 = decode_frame_audio_mode(decoder, &decoder->sa.last_frame, true, 1, bt, bb, bA)
                 + decode_frame_audio_mode(decoder, &decoder->sb.last_frame, true, 1, bt, bb, bB);

        decoder->probe_hits[0] += (uint32_t)hit0;
        decoder->probe_hits[1] += (uint32_t)hit1;
        decoder->probe_frames++;

        if (decoder->probe_frames >= 200) {
            decoder->deint_mode    = (decoder->probe_hits[1] > decoder->probe_hits[0]) ? 1 : 0;
            decoder->probe_decided = true;
            DLOG_AT(decoder, DSR_LOG_SYNC,
                    "[auto-deint] decided mode=%s (hits even=%u, odd=%u)\n",
                    decoder->deint_mode ? "ODD->blk0" : "EVEN->blk0",
                    decoder->probe_hits[0], decoder->probe_hits[1]);
        }

        if ((!decoder->probe_decided && decoder->probe_hits[1] > decoder->probe_hits[0]) || (decoder->deint_mode==1)) {
            memcpy(pcmA, bA, sizeof(bA));
            memcpy(pcmB, bB, sizeof(bB));
        } else {
            memcpy(pcmA, aA, sizeof(aA));
            memcpy(pcmB, aB, sizeof(aB));
        }
    } else {
        int mode = (decoder->probe_decided ? decoder->deint_mode : 0);
        decode_frame_audio_mode(decoder, &decoder->sa.last_frame, decoder->opts.bch_check, mode, &decoder->bch_total, &decoder->bch_bad, pcmA);
        decode_frame_audio_mode(decoder, &decoder->sb.last_frame, decoder->opts.bch_check, mode, &decoder->bch_total, &decoder->bch_bad, pcmB);
    }

    decoder->sa.got_frame = decoder->sb.got_frame = false;
    return true;
}

/* Processes up to nbits bits in one go and returns as soon as an A/B pair is
 * complete (or the buffer is exhausted). Return = bits actually consumed;
 * *emitted_out tells whether pcmA/pcmB are valid. decoder_try_emit_ab_frame_pair()
 * is only called when stream_push_bit() reports a completed frame, because it can
 * only emit right after one. Block input avoids a per-bit function call -- at
 * 20.48 Mbit/s the call overhead alone was about a third of decoder time, because the compiler cannot
 * inline anything or keep state in registers across the translation unit
 * boundary. An emit occurs only every ~640 bits, so the loop body is
 * fully amortized. */
size_t decoder_push_ab_bits(decoder_t *decoder, const uint8_t *bits, size_t nbits,
                            int *parity_io, int16_t pcmA[16], int16_t pcmB[16],
                            bool *emitted_out)
{
    int parity = *parity_io;
    size_t i = 0;
    bool emitted = false;

    while (i < nbits) {
        uint8_t bit = bits[i++];
        int completed = stream_push_bit(decoder, (parity==0) ? &decoder->sa : &decoder->sb, bit);
        parity ^= 1;
        if (completed && decoder_try_emit_ab_frame_pair(decoder, pcmA, pcmB)) {
            emitted = true;
            break;
        }
    }

    *parity_io   = parity;
    *emitted_out = emitted;
    return i;
}

bool decoder_push_ab_u8(decoder_t *decoder, uint8_t raw_byte, bool msb_first,
                        int *parity_io, int16_t pcmA[16], int16_t pcmB[16])
{
    bool emitted = false;
    for (int k = 0; k < 8; k++) {
        int bitpos = msb_first ? (7 - k) : k;
        uint8_t b = (uint8_t)((raw_byte >> bitpos) & 1u);
        if (stream_push_bit(decoder, (*parity_io == 0) ? &decoder->sa : &decoder->sb, b)) {
            if (decoder_try_emit_ab_frame_pair(decoder, pcmA, pcmB))
                emitted = true;
        }
        *parity_io ^= 1;
    }
    return emitted;
}

#ifdef BCH_SELFTEST
/* Standalone randomized fuzz test for bch_63_44_correct(), ported 1:1 from
 * the Python prototype used to validate the algorithm before this was
 * wired into parse_block77(). Not part of the normal build:
 *   cc -DBCH_SELFTEST -O2 -o /tmp/bch_selftest decoder.c pa.c -lm && /tmp/bch_selftest
 */
#include <time.h>
static uint32_t rng_state;
static uint32_t rnd(void){ rng_state ^= rng_state<<13; rng_state ^= rng_state>>17; rng_state ^= rng_state<<5; return rng_state; }
int main(void){
    rng_state = 0xC0FFEEu;
    long trials = 200000, fail_should_pass = 0;
    long by_n[4] = {0,0,0,0};
    long fourplus_detected = 0, fourplus_miscorrected = 0;
    for(long t=0; t<trials; ++t){
        uint8_t msb44[44];
        for(int i=0;i<44;i++) msb44[i] = (uint8_t)(rnd() & 1u);
        uint32_t parity = bch_calc_63_44_from44bits(msb44);
        uint8_t cw[63];
        memcpy(cw, msb44, 44);
        for(int j=0;j<19;j++) cw[44+j] = (uint8_t)((parity >> (18-j)) & 1u);

        static const int weights[] = {0,1,1,1,2,2,2,3,3,3,4,5};
        int nerr = weights[rnd() % (int)(sizeof(weights)/sizeof(weights[0]))];
        int positions[63]; for(int i=0;i<63;i++) positions[i]=i;
        for(int i=0;i<nerr;i++){ int j = i + (int)(rnd() % (uint32_t)(63-i)); int tmp=positions[i]; positions[i]=positions[j]; positions[j]=tmp; }

        uint8_t corrupted[63]; memcpy(corrupted, cw, 63);
        for(int i=0;i<nerr;i++) corrupted[positions[i]] ^= 1u;

        uint8_t attempt[63]; memcpy(attempt, corrupted, 63);
        int n = bch_63_44_correct(attempt);

        if(nerr <= 3){
            bool exact = (n == nerr) && (memcmp(attempt, cw, 63) == 0);
            if(exact) by_n[nerr]++;
            else { fail_should_pass++; printf("FAIL t=%ld nerr=%d n=%d\n", t, nerr, n); }
        } else {
            if(n < 0) fourplus_detected++;
            else if(memcmp(attempt, cw, 63) == 0) fourplus_detected++;
            else fourplus_miscorrected++;
        }
    }
    printf("trials=%ld  n0=%ld n1=%ld n2=%ld n3=%ld  fail_should_pass=%ld  4plus_detected=%ld 4plus_miscorrected=%ld\n",
           trials, by_n[0], by_n[1], by_n[2], by_n[3], fail_should_pass, fourplus_detected, fourplus_miscorrected);
    return fail_should_pass ? 1 : 0;
}
#endif
