// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
#ifndef WAVE_H
#define WAVE_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

typedef struct {
    FILE *fp;
    int channels;
    int rate;
    uint32_t samples_written; /* per channel */
} wav_t;

int  wav_open(wav_t *w, const char *path, int channels, int rate);
/* Append n frames of interleaved samples (n * channels int16 values). */
void wav_write_frames(wav_t *w, const int16_t *interleaved, size_t n);
void wav_close(wav_t *w);

#endif /* WAVE_H */
