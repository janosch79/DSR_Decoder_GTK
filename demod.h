// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEMOD_FMT_S16LE = 0,  /* interleaved I(int16),Q(int16) */
    DEMOD_FMT_F32LE = 1,  /* interleaved I(float), Q(float) */
    DEMOD_FMT_S8    = 2   /* interleaved I(int8),  Q(int8)  — HackRF native / hackrf_transfer */
} demod_iqfmt_t;

typedef struct demod_s demod_t;

typedef struct {
    unsigned    fs_hz;        /* IQ sample rate (should be ~2*sym_hz) */
    unsigned    sym_hz;       /* symbol rate */
    float       rrc_beta;     /* e.g. 0.5 (encoder) */
    int         rrc_span;     /* in symbols, e.g. 10 (encoder) */
    demod_iqfmt_t fmt;        /* s16le/f32le/s8 */
    float       agc_ref;      /* target RMS */
    float       agc_tau;      /* time constant in s */
    int         track;        /* 0: fixed SPS=2 strobe, no carrier loop (encoder IQ files, bit-exact)
                               * 1: live RF / SDR recordings: DC blocker, Gardner symbol timing
                               *    recovery (any fs ~2*sym) and QPSK carrier loop */
} demod_cfg_t;

/* Receiver loop state of the tracking path (track=1); all zero otherwise. */
typedef struct {
    float cfo_hz;             /* carrier offset tracked by the Costas loop */
    float clock_ppm;          /* symbol clock offset tracked by the timing loop */
    float level_dbfs;         /* input level (peak envelope) before AGC */
    float evm;                /* RMS error vector magnitude of the symbols, 0..~1 (lower is better) */
} demod_track_info_t;

demod_t* demod_create(const demod_cfg_t *cfg);
void     demod_free(demod_t *d);
size_t   demod_process(demod_t *d, const void *iq, size_t iq_samples, uint8_t *out, size_t out_cap);
void     demod_reset(demod_t *d);
void     demod_get_track_info(const demod_t *d, demod_track_info_t *out);

#ifdef __cplusplus
}
#endif
