// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
#ifndef DECODER_H
#define DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pa.h"


#define DSR_BLOCK_BITS    77
#define DSR_BLOCKS         4
#define DSR_PAYLOAD_BITS 308

/* Opaque decoder */
typedef struct decoder decoder_t;

typedef enum {
    DSR_LOG_NONE = 0,  /* no decoder diagnostics (default)      */
    DSR_LOG_SYNC = 1,  /* superframe, auto-deint                */
    DSR_LOG_WARN = 2,  /* + BCH/ZI fails, SC-FIFO, block errors */
    DSR_LOG_ZI   = 3,  /* + full ZI/block trace                 */
} dsr_log_level_t;

typedef struct {
    bool descramble;   /* PRBS descramble (_mkprbs)            */
    bool bch_check;    /* BCH(63,44) verification (no correct) */
    dsr_log_level_t log_level;
    bool auto_deint;   /* auto decide EVEN/ODD deinterleave     */
} decoder_opts_t;

/* Factory */
decoder_t* decoder_new(const decoder_opts_t *opts);
void       decoder_free(decoder_t *d);

/* Runtime toggles */
void decoder_set_log_level(decoder_t *d, dsr_log_level_t level);

/* Feed a run of interleaved bits (first bit -> A if *parity_io==0). Processes up to
 * nbits bits and returns as soon as an A/B pair is complete (or the buffer is exhausted).
 * Returns the number of bits actually consumed; *emitted_out tells whether pcmA/pcmB
 * were filled.
 */
size_t decoder_push_ab_bits(decoder_t *d, const uint8_t *bits, size_t nbits,
                            int *parity_io, int16_t pcmA[16], int16_t pcmB[16],
                            bool *emitted_out);

/* Feed 8 interleaved bits from one raw byte (parity in/out: first bit → A if *parity==0).
 * Returns true if at least one A/B frame pair was completed inside this byte.
 */
bool decoder_push_ab_u8(decoder_t *d, uint8_t raw_byte, bool msb_first,
                        int *parity_io, int16_t pcmA[16], int16_t pcmB[16]);

/* Restrict ZI/block PCM work to stereo pairs that contain subchannels Lch/Rch (0..31).
 * mono_out: only Lch is used. stream_focus_pairs==0 (after decoder_new) means full mux decode.
 */
void decoder_set_stream_focus(decoder_t *d, int Lch, int Rch, int mono_out);

/* Stats (e.g. BCH) */
void decoder_get_stats(decoder_t *d, uint64_t *bch_total, uint64_t *bch_bad);
/* Blocks where BCH(63,44) found errors but corrected them (subset of "ok", not "bad") */
uint64_t decoder_get_bch_corrected(const decoder_t *d);

// Decoder Setter Getter for external use
void decoder_attach_pa(decoder_t *d, pa_t *pa);
/* Clear A/B sync state, Williard, ZI/block FIFOs, probe counters; keep opts, pa, stream_focus */
void decoder_reset_sync(decoder_t *d);

/* NEW: Superframe / Williard Info */
uint64_t decoder_get_superframe_index(const decoder_t *d);  // 0,1,2,...
/* A-SSB ticks between consecutive SYNC1 superframes should be 512 (8×64); deviations are counted here */
uint64_t decoder_get_superframe_err_count(const decoder_t *d);
bool     decoder_is_williard_locked(const decoder_t *d);    // lock-state
/* true once auto-deint (EVEN/ODD) has been decided; always true if auto_deint was off */
bool     decoder_is_deint_ready(const decoder_t *d);

#endif /* DECODER_H */
