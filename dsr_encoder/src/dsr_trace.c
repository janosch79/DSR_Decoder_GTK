// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
/* dsr - Trace/Debug functionality for DSR encoder                        */
/*=======================================================================*/
/* Provides trace output to help understand the encoding process         */
/*                                                                       */
/* This code has been modified by janosch79.                             */
/* This code is provided "as is" without warranty of any kind, express  */
/* or implied. The user assumes full responsibility for any use of      */
/* this code.                                                            */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include "dsr_trace.h"

static uint32_t trace_flags = DSR_TRACE_NONE;

void dsr_trace_set_flags(uint32_t flags)
{
	trace_flags = flags;
}

uint32_t dsr_trace_get_flags(void)
{
	return trace_flags;
}

void dsr_trace_bits(const uint8_t *data, int nbits, const char *label)
{
	printf("[TRACE] %s: ", label);
	for(int i = 0; i < nbits; i++) {
		int byte_idx = i / 8;
		int bit_idx = 7 - (i % 8);
		int bit = (data[byte_idx] >> bit_idx) & 1;
		printf("%d", bit);
		if((i + 1) % 8 == 0 && i + 1 < nbits) printf(" ");
	}
	printf("\n");
}

void dsr_trace_hex(const uint8_t *data, int len, const char *label)
{
	printf("[TRACE] %s: ", label);
	for(int i = 0; i < len; i++) {
		printf("%02X ", data[i]);
		if((i + 1) % 16 == 0 && i + 1 < len) {
			printf("\n[TRACE]        ");
		}
	}
	printf("\n");
}

void dsr_trace_printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	printf("[TRACE] ");
	vprintf(format, args);
	va_end(args);
}

