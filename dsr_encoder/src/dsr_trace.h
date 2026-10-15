// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
/* dsr - Trace/Debug functionality for DSR encoder                        */
/*=======================================================================*/
/* Provides trace output to help understand the encoding process         */

#ifndef _DSR_TRACE_H
#define _DSR_TRACE_H

#include <stdint.h>
#include <stdio.h>

/* Trace flags - can be combined with bitwise OR */
#define DSR_TRACE_NONE      0x0000
#define DSR_TRACE_BITS      0x0001  /* Bit manipulation operations */
#define DSR_TRACE_INTERLEAVE 0x0002 /* Interleaving operations */
#define DSR_TRACE_PRBS      0x0004  /* PRBS generation */
#define DSR_TRACE_BCH       0x0008  /* BCH encoding */
#define DSR_TRACE_BLOCKS    0x0010  /* 77-bit blocks */
#define DSR_TRACE_FRAMES    0x0020  /* Frame structure */
#define DSR_TRACE_PS        0x0040  /* PS encoding */
#define DSR_TRACE_ALL       0xFFFF  /* All traces */

/* Set trace flags */
extern void dsr_trace_set_flags(uint32_t flags);

/* Get current trace flags */
extern uint32_t dsr_trace_get_flags(void);

/* Trace output functions */
extern void dsr_trace_bits(const uint8_t *data, int nbits, const char *label);
extern void dsr_trace_hex(const uint8_t *data, int len, const char *label);
extern void dsr_trace_printf(const char *format, ...);

/* Helper macros */
#define TRACE_BITS(data, nbits, label) \
	do { if(dsr_trace_get_flags() & DSR_TRACE_BITS) dsr_trace_bits(data, nbits, label); } while(0)

#define TRACE_HEX(data, len, label) \
	do { if(dsr_trace_get_flags() & DSR_TRACE_BITS) dsr_trace_hex(data, len, label); } while(0)

#define TRACE_PRINTF(...) \
	do { dsr_trace_printf(__VA_ARGS__); } while(0)

#endif

