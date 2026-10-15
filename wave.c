// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
#include "wave.h"
#include <stdint.h>
#include <string.h>

/* Helper for header writing only (not hot path) */
static void le16_hdr(FILE *f, uint16_t v){ uint8_t b[2]={v&0xFF,(uint8_t)(v>>8)}; fwrite(b,1,2,f); }
static void le32_hdr(FILE *f, uint32_t v){ uint8_t b[4]={v&0xFF,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; fwrite(b,1,4,f); }

int wav_open(wav_t *w, const char *path, int channels, int rate){
    memset(w,0,sizeof(*w));
    w->fp = fopen(path,"wb");
    if(!w->fp){ perror("fopen wav"); return -1; }
    w->channels = channels; w->rate = rate; w->samples_written = 0;
    fwrite("RIFF",1,4,w->fp); le32_hdr(w->fp, 0);
    fwrite("WAVEfmt ",1,8,w->fp); le32_hdr(w->fp,16);
    le16_hdr(w->fp,1); le16_hdr(w->fp,(uint16_t)channels);
    le32_hdr(w->fp,(uint32_t)rate);
    uint32_t byteRate = (uint32_t)rate * (uint32_t)channels * 2;
    le32_hdr(w->fp, byteRate);
    le16_hdr(w->fp, (uint16_t)(channels * 2)); // block align
    le16_hdr(w->fp, 16);
    fwrite("data",1,4,w->fp); le32_hdr(w->fp, 0);
    return 0;
}

void wav_write_frames(wav_t *w, const int16_t *interleaved, size_t n){
    fwrite(interleaved, sizeof(int16_t), n * (size_t)w->channels, w->fp);
    w->samples_written += (uint32_t)n;
}

void wav_close(wav_t *w){
    if(!w->fp) return;
    long file_end = ftell(w->fp);
    uint32_t data_bytes = w->samples_written * (uint32_t)w->channels * 2;
    fseek(w->fp, 4, SEEK_SET);  le32_hdr(w->fp, 36 + data_bytes);
    fseek(w->fp, 40, SEEK_SET); le32_hdr(w->fp, data_bytes);
    fseek(w->fp, file_end, SEEK_SET);
    fclose(w->fp); w->fp=NULL;
}
