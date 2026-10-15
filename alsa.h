// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// alsa.h
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>   // for ssize_t

typedef struct alsa_out_t alsa_out_t;

alsa_out_t *alsa_open(const char *dev, unsigned rate, unsigned channels,
                      unsigned period_ms, unsigned buffer_ms, int nonblock);
ssize_t     alsa_write(alsa_out_t *ao, const int16_t *interleaved, size_t frames);
void        alsa_drain(alsa_out_t *ao);
/* Discard scheduled playback and reset device (use after hard resync / file loop to avoid xruns) */
void        alsa_stream_reset(alsa_out_t *ao);
void        alsa_close(alsa_out_t *ao);

uint64_t    alsa_get_xruns(alsa_out_t *ao);
