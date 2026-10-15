// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// hackrf_rx.h — HackRF One as live IQ source (RX) for the built-in demodulator.
// Delivers interleaved int8 I/Q (DEMOD_FMT_S8) from the libusb callback thread through
// a ring buffer to the decode thread. Used by dsrdec (--hackrf) and dsr_engine.c.
// Without libhackrf (built without HAVE_HACKRF) hackrf_rx_open always fails.

#ifndef HACKRF_RX_H
#define HACKRF_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HACKRF_RX_DEFAULT_FREQ   118000000ULL  /* DSR in German cable networks (S2) */
#define HACKRF_RX_DEFAULT_RATE   20480000u     /* 2 x 10.24 MBd, same as the encoder */
#define HACKRF_RX_DEFAULT_LNA    16u
#define HACKRF_RX_DEFAULT_VGA    20u
#define DSR_SYMBOL_RATE_HZ       10240000u

typedef struct hackrf_rx hackrf_rx_t;

typedef struct {
    char     serial[64];      /* "" = first device */
    uint64_t freq_hz;
    unsigned sample_rate;     /* 8..20.48 MS/s; ~2 x symbol rate */
    unsigned bw_hz;           /* baseband filter, 0 = auto from sample rate */
    unsigned lna_db;          /* 0..40, step 8 */
    unsigned vga_db;          /* 0..62, step 2 */
    bool     amp;             /* RF amp, +14 dB */
    bool     auto_gain;       /* adjust VGA/LNA from the demod input level */
} hackrf_rx_cfg_t;

void hackrf_rx_default_cfg(hackrf_rx_cfg_t *cfg);
bool hackrf_rx_available(void);   /* built with libhackrf */

hackrf_rx_t *hackrf_rx_open(const hackrf_rx_cfg_t *cfg, char *err, size_t errlen);
void         hackrf_rx_close(hackrf_rx_t *rx);

/* Waits up to timeout_ms for samples; returns IQ pairs copied (0 on timeout).
 * Returns (size_t)-1 when the device stopped streaming (unplugged). */
size_t hackrf_rx_read(hackrf_rx_t *rx, int8_t *iq, size_t max_pairs, int timeout_ms);

/* Drops everything queued (after a resync). */
void hackrf_rx_flush(hackrf_rx_t *rx);

/* Feed the demod input level (dBFS, demod_track_info_t.level_dbfs) periodically; with
 * auto_gain on, steps VGA/LNA towards the target window. Returns true if gains changed. */
bool hackrf_rx_auto_gain(hackrf_rx_t *rx, float level_dbfs);

void     hackrf_rx_get_gains(const hackrf_rx_t *rx, unsigned *lna_db, unsigned *vga_db, bool *amp);
uint64_t hackrf_rx_dropped_bytes(const hackrf_rx_t *rx);   /* ring overflows */
const hackrf_rx_cfg_t *hackrf_rx_cfg(const hackrf_rx_t *rx);

#endif /* HACKRF_RX_H */
