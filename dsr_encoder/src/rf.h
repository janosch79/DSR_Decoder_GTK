/* dsr - Digitale Satelliten Radio (DSR) encoder                         */
/*=======================================================================*/
/* Copyright 2021 Philip Heron <phil@sanslogic.co.uk>                    */
/*                                                                       */
/* This program is free software: you can redistribute it and/or modify  */
/* it under the terms of the GNU General Public License as published by  */
/* the Free Software Foundation, either version 3 of the License, or     */
/* (at your option) any later version.                                   */
/*                                                                       */
/* This program is distributed in the hope that it will be useful,       */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of        */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         */
/* GNU General Public License for more details.                          */
/*                                                                       */
/* You should have received a copy of the GNU General Public License     */
/* along with this program.  If not, see <http://www.gnu.org/licenses/>. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>

#ifndef _RF_H
#define _RF_H

/* File output types */
#define RF_UINT8  0
#define RF_INT8   1
#define RF_UINT16 2
#define RF_INT16  3
#define RF_INT32  4
#define RF_FLOAT  5 /* 32-bit float */
#define RF_UNMOD_UINT8 6
#define RF_UNMOD_UDP 7
#define RF_UNMOD_TCP 8

// Common color strings - define once above if used multiple times:
#define COLOR_AMBER "\x1b[38;5;214m"
#define COLOR_BLUE  "\x1b[34m"
#define COLOR_RESET "\x1b[0m"


/* Callback prototypes */
typedef int (*rf_write_t)(void *private, int16_t *iq_data, int samples);
typedef int (*rf_close_t)(void *private);

typedef struct {
	
	void *private;
	rf_write_t write;
	rf_close_t close;
	
	double scale;
	
} rf_t;

typedef struct {
    int      sock;
    size_t   payload;
    int      preview_done;

    /* Target address for sendto() */
    struct sockaddr_storage addr;
    socklen_t addrlen;

    /* Pacing */
     /* --- NEU: Pacing --- */
    unsigned long long bitrate_bps;  /* 0 => Pacing aus */
    double   tokens_bytes;           /* Token-Bucket in Bytes */
    struct timespec last;            /* letzte Auffüllzeit */
} rf_udp_t;

typedef struct {
	int sock;
	int preview_done;
} rf_tcp_t;

extern double rf_scale(rf_t *s);
extern int rf_write(rf_t *s, int16_t *iq_data, int samples);
extern int rf_close(rf_t *s);

int rf_udp_open(void **out_private, const char *host, const char *port, size_t payload_bytes);
void rf_udp_set_bitrate(void *priv, uint64_t bps);
int rf_udp_send(void *priv, const uint8_t *data, size_t len);
int rf_udp_close(void *priv);

int rf_tcp_open(void **out_private, const char *host, const char *port);
int rf_tcp_close(void *priv);

typedef struct {
	
	int interpolation;
	int ntaps;
	int16_t *taps[4];
	
	/* Output window */
	int winx;
	int16_t *win;
	
	/* Differential state */
	int sym;
	
} rf_qpsk_t;


extern void rf_qpsk_free(rf_qpsk_t *s);
extern int rf_qpsk_init(rf_qpsk_t *s, int interpolation, double level);
extern int rf_qpsk_modulate(rf_qpsk_t *s, int16_t *dst, const uint8_t *src, int bits);

#include "rf_file.h"
#include "rf_hackrf.h"

#endif

