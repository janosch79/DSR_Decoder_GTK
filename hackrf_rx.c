// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// hackrf_rx.c — HackRF One RX source, see hackrf_rx.h.
//
// libhackrf calls rx_callback from its libusb thread with ~256 KiB blocks (~6 ms at
// 20.48 MS/s). The callback must never block, so on overflow the whole block is dropped
// and counted; the decoder resynchronizes on its own after the gap.

#define _POSIX_C_SOURCE 200809L

#include "hackrf_rx.h"

#include <stdio.h>
#include <string.h>

void hackrf_rx_default_cfg(hackrf_rx_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->freq_hz = HACKRF_RX_DEFAULT_FREQ;
    c->sample_rate = HACKRF_RX_DEFAULT_RATE;
    c->lna_db = HACKRF_RX_DEFAULT_LNA;
    c->vga_db = HACKRF_RX_DEFAULT_VGA;
    c->auto_gain = true;
}

#ifndef HAVE_HACKRF

bool hackrf_rx_available(void) { return false; }

hackrf_rx_t *hackrf_rx_open(const hackrf_rx_cfg_t *cfg, char *err, size_t errlen)
{
    (void)cfg;
    if (err) snprintf(err, errlen, "Built without HackRF support (install libhackrf-dev and rebuild)");
    return NULL;
}
void hackrf_rx_close(hackrf_rx_t *rx) { (void)rx; }
size_t hackrf_rx_read(hackrf_rx_t *rx, int8_t *iq, size_t max_pairs, int timeout_ms)
{ (void)rx; (void)iq; (void)max_pairs; (void)timeout_ms; return (size_t)-1; }
void hackrf_rx_flush(hackrf_rx_t *rx) { (void)rx; }
bool hackrf_rx_auto_gain(hackrf_rx_t *rx, float level_dbfs) { (void)rx; (void)level_dbfs; return false; }
void hackrf_rx_get_gains(const hackrf_rx_t *rx, unsigned *lna_db, unsigned *vga_db, bool *amp)
{ (void)rx; *lna_db = 0; *vga_db = 0; *amp = false; }
uint64_t hackrf_rx_dropped_bytes(const hackrf_rx_t *rx) { (void)rx; return 0; }
const hackrf_rx_cfg_t *hackrf_rx_cfg(const hackrf_rx_t *rx) { (void)rx; return NULL; }

#else /* HAVE_HACKRF */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <libhackrf/hackrf.h>

#define RX_RING_CAP      (32u * 1024u * 1024u)  /* power of 2, ~0.8 s at 20.48 MS/s */
#define AGC_PERIOD_NS    300000000LL            /* one gain step per 300 ms */
/* Window for the mean max(|I|,|Q|) envelope: peaks are ~3x higher, so -10 dBFS already
 * clips the int8 ADC; below -22 dBFS too few of its 8 bits are used. */
#define AGC_HIGH_DBFS    (-10.0f)
#define AGC_LOW_DBFS     (-22.0f)

struct hackrf_rx {
    hackrf_rx_cfg_t cfg;
    hackrf_device  *dev;
    bool            streaming;

    uint8_t        *buf;
    uint64_t        head, tail;      /* under mu */
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    _Atomic uint64_t dropped;

    unsigned        lna, vga;        /* current gains (auto gain changes them) */
    int64_t         agc_last_ns;
};

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

bool hackrf_rx_available(void) { return true; }

static int rx_callback(hackrf_transfer *t)
{
    hackrf_rx_t *rx = t->rx_ctx;
    size_t n = (size_t)t->valid_length & ~(size_t)1;   /* whole I/Q pairs */
    pthread_mutex_lock(&rx->mu);
    if ((uint64_t)RX_RING_CAP - (rx->head - rx->tail) < n) {
        pthread_mutex_unlock(&rx->mu);
        atomic_fetch_add(&rx->dropped, (uint64_t)n);
        return 0;
    }
    const uint64_t mask = RX_RING_CAP - 1u;
    size_t off = (size_t)(rx->head & mask);
    size_t first = (n < RX_RING_CAP - off) ? n : RX_RING_CAP - off;
    memcpy(rx->buf + off, t->buffer, first);
    memcpy(rx->buf, t->buffer + first, n - first);
    rx->head += n;
    pthread_cond_signal(&rx->cv);
    pthread_mutex_unlock(&rx->mu);
    return 0;
}

#define HRF_TRY(call, what) do { \
        int r_ = (call); \
        if (r_ != HACKRF_SUCCESS) { \
            snprintf(err, errlen, "%s failed: %s", what, hackrf_error_name((enum hackrf_error)r_)); \
            goto fail; \
        } \
    } while (0)

hackrf_rx_t *hackrf_rx_open(const hackrf_rx_cfg_t *cfg, char *err, size_t errlen)
{
    char dummy[8];
    if (!err) { err = dummy; errlen = sizeof(dummy); }
    err[0] = 0;

    hackrf_rx_t *rx = calloc(1, sizeof(*rx));
    if (!rx) { snprintf(err, errlen, "Out of memory"); return NULL; }
    rx->cfg = *cfg;
    if (rx->cfg.sample_rate == 0) rx->cfg.sample_rate = HACKRF_RX_DEFAULT_RATE;
    rx->lna = rx->cfg.lna_db > 40 ? 40 : rx->cfg.lna_db & ~7u;
    rx->vga = rx->cfg.vga_db > 62 ? 62 : rx->cfg.vga_db & ~1u;
    rx->buf = malloc(RX_RING_CAP);
    pthread_mutex_init(&rx->mu, NULL);
    pthread_cond_init(&rx->cv, NULL);
    if (!rx->buf) { snprintf(err, errlen, "Out of memory (RX ring)"); goto fail_mem; }

    HRF_TRY(hackrf_init(), "hackrf_init");
    {
        int r = hackrf_open_by_serial(rx->cfg.serial[0] ? rx->cfg.serial : NULL, &rx->dev);
        if (r != HACKRF_SUCCESS) {
            rx->dev = NULL;
            if (r == HACKRF_ERROR_NOT_FOUND)
                snprintf(err, errlen, "No HackRF found%s%s (USB connected? permissions/udev rules?)",
                         rx->cfg.serial[0] ? " with serial " : "", rx->cfg.serial);
            else
                snprintf(err, errlen, "hackrf_open failed: %s", hackrf_error_name((enum hackrf_error)r));
            goto fail;
        }
    }
    HRF_TRY(hackrf_set_sample_rate_manual(rx->dev, rx->cfg.sample_rate, 1), "hackrf_set_sample_rate");
    {
        uint32_t bw = rx->cfg.bw_hz ? rx->cfg.bw_hz : rx->cfg.sample_rate;
        HRF_TRY(hackrf_set_baseband_filter_bandwidth(rx->dev, hackrf_compute_baseband_filter_bw(bw)),
                "hackrf_set_baseband_filter_bandwidth");
    }
    HRF_TRY(hackrf_set_freq(rx->dev, rx->cfg.freq_hz), "hackrf_set_freq");
    HRF_TRY(hackrf_set_amp_enable(rx->dev, rx->cfg.amp ? 1 : 0), "hackrf_set_amp_enable");
    HRF_TRY(hackrf_set_lna_gain(rx->dev, rx->lna), "hackrf_set_lna_gain");
    HRF_TRY(hackrf_set_vga_gain(rx->dev, rx->vga), "hackrf_set_vga_gain");
    HRF_TRY(hackrf_start_rx(rx->dev, rx_callback, rx), "hackrf_start_rx");
    rx->streaming = true;
    rx->agc_last_ns = mono_ns();
    fprintf(stderr, "[hackrf] RX %.3f MHz, %.3f MS/s, LNA %u dB, VGA %u dB, amp %s%s\n",
            (double)rx->cfg.freq_hz / 1e6, (double)rx->cfg.sample_rate / 1e6, rx->lna, rx->vga,
            rx->cfg.amp ? "on" : "off", rx->cfg.auto_gain ? ", auto gain" : "");
    return rx;

fail:
    hackrf_rx_close(rx);
    return NULL;
fail_mem:
    pthread_cond_destroy(&rx->cv);
    pthread_mutex_destroy(&rx->mu);
    free(rx->buf);
    free(rx);
    return NULL;
}

void hackrf_rx_close(hackrf_rx_t *rx)
{
    if (!rx) return;
    if (rx->dev) {
        if (rx->streaming) hackrf_stop_rx(rx->dev);
        hackrf_close(rx->dev);
    }
    hackrf_exit();
    pthread_cond_destroy(&rx->cv);
    pthread_mutex_destroy(&rx->mu);
    free(rx->buf);
    free(rx);
}

size_t hackrf_rx_read(hackrf_rx_t *rx, int8_t *iq, size_t max_pairs, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)timeout_ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;

    pthread_mutex_lock(&rx->mu);
    while (rx->head - rx->tail < 2) {
        if (pthread_cond_timedwait(&rx->cv, &rx->mu, &ts) == ETIMEDOUT) {
            pthread_mutex_unlock(&rx->mu);
            return hackrf_is_streaming(rx->dev) == HACKRF_TRUE ? 0 : (size_t)-1;
        }
    }
    const uint64_t mask = RX_RING_CAP - 1u;
    size_t n = (size_t)(rx->head - rx->tail) & ~(size_t)1;
    if (n > max_pairs * 2) n = max_pairs * 2;
    size_t off = (size_t)(rx->tail & mask);
    size_t first = (n < RX_RING_CAP - off) ? n : RX_RING_CAP - off;
    memcpy(iq, rx->buf + off, first);
    memcpy((uint8_t *)iq + first, rx->buf, n - first);
    rx->tail += n;
    pthread_mutex_unlock(&rx->mu);
    return n / 2;
}

void hackrf_rx_flush(hackrf_rx_t *rx)
{
    pthread_mutex_lock(&rx->mu);
    rx->tail = rx->head;
    pthread_mutex_unlock(&rx->mu);
}

bool hackrf_rx_auto_gain(hackrf_rx_t *rx, float level_dbfs)
{
    if (!rx->cfg.auto_gain) return false;
    int64_t now = mono_ns();
    if (now - rx->agc_last_ns < AGC_PERIOD_NS) return false;
    rx->agc_last_ns = now;

    unsigned lna = rx->lna, vga = rx->vga;
    if (level_dbfs > AGC_HIGH_DBFS) {           /* reduce: IF first, then RF */
        if (vga > 16)     vga -= 4;
        else if (lna >= 8) lna -= 8;
        else if (vga >= 4) vga -= 4;
    } else if (level_dbfs < AGC_LOW_DBFS) {     /* raise: RF first (better noise figure) */
        if (lna < 32)       lna += 8;
        else if (vga <= 58) vga += 4;
        else if (lna < 40)  lna += 8;
    }
    if (lna == rx->lna && vga == rx->vga) return false;
    if (lna != rx->lna && hackrf_set_lna_gain(rx->dev, lna) == HACKRF_SUCCESS) rx->lna = lna;
    if (vga != rx->vga && hackrf_set_vga_gain(rx->dev, vga) == HACKRF_SUCCESS) rx->vga = vga;
    return true;
}

void hackrf_rx_get_gains(const hackrf_rx_t *rx, unsigned *lna_db, unsigned *vga_db, bool *amp)
{
    *lna_db = rx->lna;
    *vga_db = rx->vga;
    *amp = rx->cfg.amp;
}

uint64_t hackrf_rx_dropped_bytes(const hackrf_rx_t *rx) { return atomic_load(&rx->dropped); }

const hackrf_rx_cfg_t *hackrf_rx_cfg(const hackrf_rx_t *rx) { return &rx->cfg; }

#endif /* HAVE_HACKRF */
