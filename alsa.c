// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// alsa.c
#define _POSIX_C_SOURCE 200809L   // enable POSIX.1-2008 APIs
#include <time.h>                 // provides struct timespec
#include <unistd.h>               // for usleep()
#include "alsa.h"
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct alsa_out_t {
    snd_pcm_t *h;
    unsigned rate, ch;
    snd_pcm_uframes_t period_frames;
    snd_pcm_uframes_t buffer_frames;
    int nonblock;
    uint64_t xruns;
};

static int ao_recover(struct alsa_out_t *ao, int err) {
    if (err == -EPIPE) { ao->xruns++; err = snd_pcm_prepare(ao->h); return err; }
    if (err == -ESTRPIPE) {
        while ((err = snd_pcm_resume(ao->h)) == -EAGAIN) sleep(1000);
        if (err < 0) err = snd_pcm_prepare(ao->h);
        return err;
    }
    if (err == -EINTR) return 0;
    if (err == -EAGAIN) return 0;
    return err;
}

alsa_out_t *alsa_open(const char *dev, unsigned rate, unsigned channels,
                      unsigned period_ms, unsigned buffer_ms, int nonblock)
{
    struct alsa_out_t *ao = calloc(1, sizeof *ao);
    if (!ao) return NULL;
    ao->rate = rate; ao->ch = channels; ao->nonblock = nonblock ? 1 : 0;

    int mode = ao->nonblock ? SND_PCM_NONBLOCK : 0;
    if (!dev) dev = "default";
    if (snd_pcm_open(&ao->h, dev, SND_PCM_STREAM_PLAYBACK, mode) < 0) {
        fprintf(stderr, "[alsa] open failed for '%s'\n", dev);
        free(ao); return NULL;
    }

    // Simple configuration: set_params with the desired total latency
    unsigned int latency_us = buffer_ms * 1000u;
    int err = snd_pcm_set_params(ao->h,
                                 SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 channels,
                                 rate,
                                 1 /* soft_resample */,
                                 latency_us);
    if (err < 0) {
        fprintf(stderr, "[alsa] set_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(ao->h); free(ao); return NULL;
    }

    snd_pcm_uframes_t bf = 0, pf = 0;
    snd_pcm_get_params(ao->h, &bf, &pf);
    ao->buffer_frames = bf;
    // Our "period" reference stays application-side (target derived from period_ms):
    ao->period_frames = (snd_pcm_uframes_t)((uint64_t)rate * period_ms / 1000u);
    if (ao->period_frames < 128) ao->period_frames = 128;

    fprintf(stderr, "[alsa] device=%s rate=%u ch=%u buf=%lu frames (~%ums) period~%lu frames\n",
            dev, rate, channels,
            (unsigned long)ao->buffer_frames, (unsigned)((ao->buffer_frames*1000ull)/rate),
            (unsigned long)ao->period_frames);
    return ao;
}

ssize_t alsa_write(alsa_out_t *ao, const int16_t *interleaved, size_t frames)
{
    const uint8_t *p = (const uint8_t*)interleaved;
    size_t frame_bytes = (size_t)ao->ch * sizeof(int16_t);
    size_t left = frames;

    while (left > 0) {
        snd_pcm_sframes_t want = (snd_pcm_sframes_t)left;
        snd_pcm_sframes_t r = snd_pcm_writei(ao->h, p, want);
        if (r < 0) {
            int e = ao_recover(ao, (int)r);
            if (e < 0) return e;
            if (r == -EAGAIN) {
                /* Non-blocking: buffer full - drop samples instead of waiting!
                 * This avoids timing problems under high system load.
                 */
                if (ao->nonblock) {
                    /* Drop remaining samples and return */
                    return (ssize_t)(frames - left);
                }
                snd_pcm_wait(ao->h, 50); continue;
            }
            continue; // retry after recover
        }
        if (r == 0) {
            if (ao->nonblock) {
                /* Non-blocking: no space - drop samples */
                return (ssize_t)(frames - left);
            }
            snd_pcm_wait(ao->h, 10); continue;
        }
        // r > 0
        size_t wrote = (size_t)r;
        p    += wrote * frame_bytes;
        left -= wrote;
    }
    return (ssize_t)frames;
}

void alsa_drain(alsa_out_t *ao)
{
    if (!ao || !ao->h) return;
    snd_pcm_drain(ao->h);
}

void alsa_stream_reset(alsa_out_t *ao)
{
    if (!ao || !ao->h) return;
    (void)snd_pcm_drop(ao->h);
    (void)snd_pcm_prepare(ao->h);
}

void alsa_close(alsa_out_t *ao)
{
    if (!ao) return;
    if (ao->h) snd_pcm_close(ao->h);
    free(ao);
}

uint64_t alsa_get_xruns(alsa_out_t *ao) { return ao ? ao->xruns : 0; }
