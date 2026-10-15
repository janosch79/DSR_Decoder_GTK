// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// dsr_engine.h — restartable DSR decode engine for the GTK frontend (dsrdec-gtk).
// Same pipeline as dsrdec (decodermain.c): IQ/raw/UDP/HackRF -> decoder/PA -> ALSA/WAV,
// but all state lives in one engine_t so a GUI can start/stop it repeatedly.
// The CLI does not use this module.

#ifndef DSR_ENGINE_H
#define DSR_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "decoder.h"
#include "hackrf_rx.h"

#define ENGINE_NUM_CH 32

typedef enum {
    ENGINE_SRC_IQ  = 0,   /* raw IQ file, demodulated internally */
    ENGINE_SRC_RAW = 1,   /* raw interleaved A/B bitstream file  */
    ENGINE_SRC_UDP = 2,   /* raw bitstream over UDP              */
    ENGINE_SRC_HACKRF = 3,/* live RF from a HackRF One, demodulated internally */
} engine_src_t;

typedef struct {
    engine_src_t src;
    char     path[1024];      /* IQ or raw file */
    unsigned iqrate;          /* IQ sample rate (default 20480000) */
    int      iq_fmt;          /* 0 s16le, 1 f32le, 2 s8 (= demod_iqfmt_t) */
    bool     iq_track;        /* IQ file: timing/carrier recovery (SDR recordings) */
    hackrf_rx_cfg_t rx;       /* ENGINE_SRC_HACKRF; sample_rate is the IQ rate */
    char     host[64];        /* UDP listen host */
    int      port;            /* UDP port */
    bool     msb_first;       /* raw/UDP bit order */
    bool     repeat;          /* file: rewind on EOF */
    bool     descramble;
    bool     bch_check;
    bool     auto_deint;
    bool     stream_focus;    /* decode only the selected pair (less CPU) */
    dsr_log_level_t log_level;
    int      rate;            /* output sample rate */
    int      upsample;        /* repeat each sample N times */
    bool     audioout;        /* live ALSA playback */
    bool     realtime;        /* pace file input at real time even without ALSA */
    char     wav[1024];       /* "" = no WAV recording; always written as stereo */
    int      start_ch;        /* initial channel 0..31 */
    bool     start_stereo;    /* initial mode */
} engine_cfg_t;

typedef enum {
    ENGINE_RUNNING  = 0,
    ENGINE_FINISHED = 1,      /* file reached EOF (no repeat) */
    ENGINE_FAILED   = 2,      /* runtime error, see msg */
} engine_state_t;

typedef struct {
    bool valid;
    char name[32];            /* UTF-8 station label */
    int  type;                /* PA programme type 0..15 */
    int  music;               /* 1 = music, 0 = speech */
    int  mode;                /* 1 = mono / stereo-left, 2 = stereo-right, 0 = unknown */
} engine_chan_t;

typedef struct {
    engine_state_t state;
    char     msg[256];
    engine_src_t src;

    uint64_t in_bytes;        /* bytes read from source */
    /* receiver (HackRF or IQ file with iq_track) */
    bool     rf_valid;
    float    rf_level_dbfs, rf_cfo_hz, rf_clock_ppm, rf_evm;
    unsigned rf_lna, rf_vga;  /* HackRF gains (auto gain may change them) */
    bool     rf_amp;
    uint64_t rf_freq_hz;
    uint64_t rf_drop_bytes;   /* HackRF USB overflows */
    unsigned rf_acq_retries;  /* acquisitions restarted for lack of a lock */
    uint64_t bits;            /* bits fed to decoder */
    double   progress;        /* file position 0..1, or -1 for UDP/stdin */

    uint64_t bch_total, bch_bad, bch_corr;
    uint64_t sf, sf_err;
    bool     will_locked, deint_ready, sa_locked, sa_offset_ready;
    /* station list (pa.c): only confirmed names/attributes are reported in ch[] */
    bool     pa_aligned;      /* SA alignment verified, new station data accepted */
    int      pa_names, pa_attrs;  /* channels with confirmed name / attributes */
    unsigned pa_realigns;     /* SA alignment dropped and redone (list kept) */
    bool     warmup;          /* audio muted until decoder/PA locked */
    bool     resync_wait;     /* resync issued, waiting for relock */

    bool     audio_on, wav_on, muted;
    uint64_t xruns, audio_drop_bytes;

    int      sel_l, sel_r;    /* current output channels */
    bool     sel_stereo;
    dsr_log_level_t log_level;

    engine_chan_t ch[ENGINE_NUM_CH];
} engine_status_t;

typedef struct engine engine_t;

void engine_default_cfg(engine_cfg_t *cfg);

/* Starts worker threads. Returns NULL on failure (reason in err). Only one engine at a
 * time: pa.c keeps SA-cycle state in module globals. */
engine_t *engine_start(const engine_cfg_t *cfg, char *err, size_t errlen);
/* Stops threads, closes ALSA/WAV/source, frees e. */
void      engine_stop(engine_t *e);

/* Thread-safe snapshot (refreshed by the worker ~10x per second). */
void engine_get_status(engine_t *e, engine_status_t *out);

/* Commands — thread-safe, applied by the worker on its next loop iteration. */
void engine_select(engine_t *e, int ch, bool stereo); /* stereo: pair ch&~1 / (ch&~1)+1 */
void engine_step(engine_t *e, int dir);               /* +1 / -1, like CLI u / d */
void engine_set_stereo(engine_t *e, bool stereo);     /* like CLI s / m */
void engine_resync(engine_t *e);                      /* like CLI r */
void engine_set_log_level(engine_t *e, dsr_log_level_t lvl);
void engine_set_mute(engine_t *e, bool mute);
/* Live audio gain 0.0..1.0 (1.0 = unchanged). Applies to ALSA output only; WAV recording
 * always gets the unscaled signal. */
void engine_set_volume(engine_t *e, float gain);

/* Display name of subchannel ch (0..31): 1L, 1R, 2L, ... 16R (pair ch/2+1, even = L). */
const char *engine_ch_name(int ch, char buf[4]);
const char *engine_pa_type_name(int type);
const char *engine_log_level_name(dsr_log_level_t lvl);

#endif /* DSR_ENGINE_H */
