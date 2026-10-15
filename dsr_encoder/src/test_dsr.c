// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
/* dsr - Test program for DSR encoder functions                          */
/*=======================================================================*/
/* This program tests and demonstrates key DSR encoding functions        */
/* with trace output to help understand the encoding process.            */
/*                                                                       */
/* This file should only be compiled when DSR_ENABLE_TEST is defined.   */
/* The Makefile sets DSR_ENABLE_TEST when building test_dsr.             */
/*                                                                       */
/* This code has been modified by janosch79.                             */
/* This code is provided "as is" without warranty of any kind, express  */
/* or implied. The user assumes full responsibility for any use of      */
/* this code.                                                            */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include "dsr.h"
#include "bits.h"
#include "dsr_trace.h"

/* Forward declarations for internal functions we want to test */
/* These are only available when DSR_ENABLE_TEST is defined */
#ifdef DSR_ENABLE_TEST
extern const uint16_t _ileave[256];
extern const uint8_t _par[256];
#endif

/* Helper function to print binary representation */
static void print_bits(const uint8_t *data, int nbits, const char *label)
{
	printf("%s: ", label);
	for(int i = 0; i < nbits; i++) {
		int byte_idx = i / 8;
		int bit_idx = 7 - (i % 8);
		int bit = (data[byte_idx] >> bit_idx) & 1;
		printf("%d", bit);
		if((i + 1) % 8 == 0 && i + 1 < nbits) printf(" ");
	}
	printf("\n");
}

/* Helper function to print hex dump */
static void print_hex(const uint8_t *data, int len, const char *label)
{
	printf("%s: ", label);
	for(int i = 0; i < len; i++) {
		printf("%02X ", data[i]);
		if((i + 1) % 16 == 0) printf("\n");
	}
	if(len % 16 != 0) printf("\n");
}

/* Test bits_write_uint function */
static void test_bits_write(void)
{
	printf("\n=== Test: bits_write_uint ===\n");
	
	uint8_t buffer[16] = {0};
	int pos = 0;
	
	/* Test 1: Write simple value at byte boundary */
	printf("\nTest 1: Write 0xAB (8 bits) at position 0\n");
	pos = bits_write_uint(buffer, 0, 0xAB, 8);
	print_hex(buffer, 2, "Buffer");
	printf("Next position: %d\n", pos);
	assert(buffer[0] == 0xAB);
	assert(pos == 8);
	
	/* Test 2: Write value at bit offset */
	printf("\nTest 2: Write 0x5 (3 bits) at position 8\n");
	memset(buffer, 0, sizeof(buffer));
	pos = bits_write_uint(buffer, 8, 0x5, 3);
	print_bits(buffer, 16, "Buffer bits");
	print_hex(buffer, 2, "Buffer hex");
	printf("Next position: %d\n", pos);
	assert((buffer[1] & 0xE0) == 0xA0); /* 10100000 */
	assert(pos == 11);
	
	/* Test 3: Write value spanning multiple bytes */
	printf("\nTest 3: Write 0x1234 (16 bits) at position 4\n");
	memset(buffer, 0, sizeof(buffer));
	pos = bits_write_uint(buffer, 4, 0x1234, 16);
	print_bits(buffer, 24, "Buffer bits");
	print_hex(buffer, 3, "Buffer hex");
	printf("Next position: %d\n", pos);
	assert(pos == 20);
	
	/* Test 4: Write sync word like in DSR (0x712, 11 bits) */
	printf("\nTest 4: Write sync word 0x712 (11 bits) at position 0\n");
	memset(buffer, 0, sizeof(buffer));
	pos = bits_write_uint(buffer, 0, 0x712, 11);
	print_bits(buffer, 16, "Buffer bits");
	print_hex(buffer, 2, "Buffer hex");
	printf("Expected: 0x712 = 0b11100010010\n");
	printf("Next position: %d\n", pos);
	assert(pos == 11);
}

/* Test interleaving table */
static void test_interleaving(void)
{
#ifdef DSR_ENABLE_TEST
	printf("\n=== Test: Interleaving Table (_ileave) ===\n");
	
	printf("\nThe _ileave table spreads bits from an 8-bit byte into a 16-bit word\n");
	printf("by inserting zeros between each bit.\n\n");
	
	/* Validate the interleaving table */
	printf("Validating interleaving table...\n");
	int errors = 0;
	for(int i = 0; i < 256; i++) {
		uint16_t expected = 0;
		/* Calculate expected value: each bit from input goes to position bit*2 */
		for(int bit = 0; bit < 8; bit++) {
			if(i & (1 << bit)) {
				expected |= (1 << (bit * 2));
			}
		}
		if(_ileave[i] != expected) {
			if(errors == 0) {
				printf("ERRORS FOUND:\n");
			}
			printf("  Index 0x%02X: expected 0x%04X, got 0x%04X\n", i, expected, _ileave[i]);
			errors++;
			if(errors >= 10) {
				printf("  ... (showing first 10 errors)\n");
				break;
			}
		}
	}
	if(errors == 0) {
		printf("  ✓ All 256 entries are correct!\n");
	} else {
		printf("  ✗ Found %d error(s) in interleaving table!\n", errors);
	}
	
	/* Show pattern for first few values */
	printf("\nExamples:\n");
	for(int i = 0; i < 16; i++) {
		printf("  Input: 0x%02X (0b", i);
		for(int b = 7; b >= 0; b--) {
			printf("%d", (i >> b) & 1);
		}
		printf(") -> Output: 0x%04X (0b", _ileave[i]);
		for(int b = 15; b >= 0; b--) {
			printf("%d", (_ileave[i] >> b) & 1);
		}
		printf(")\n");
	}
	
	/* Test interleaving two bytes */
	printf("\nTest: Interleave two bytes (0xAB and 0xCD)\n");
	uint16_t byte1 = _ileave[0xAB];
	uint16_t byte2 = _ileave[0xCD];
	uint16_t interleaved = (byte1 << 1) | byte2;
	
	printf("Byte 1 (0xAB): 0x%04X\n", byte1);
	printf("Byte 2 (0xCD): 0x%04X\n", byte2);
	printf("Interleaved (byte1<<1 | byte2): 0x%04X\n", interleaved);
	print_bits((uint8_t*)&interleaved, 16, "Interleaved bits");
#else
	printf("\n=== Test: Interleaving Table (_ileave) ===\n");
	printf("ERROR: This test requires DSR_ENABLE_TEST to be defined!\n");
#endif
}

/* Test PRBS generation (simplified) */
static void test_prbs_pattern(void)
{
	printf("\n=== Test: PRBS Pattern ===\n");
	printf("\nPRBS (Pseudo-Random Binary Sequence) is used for spectrum shaping.\n");
	printf("It uses a linear feedback shift register (LFSR).\n\n");
	
	/* Show first few PRBS bits */
	uint16_t r = 0xBD; /* Initial state */
	printf("Initial state: 0x%02X\n", r);
	printf("First 32 PRBS bits:\n");
	
	for(int i = 0; i < 32; i++) {
		int bit = r & 1;
		printf("%d", bit);
		if((i + 1) % 8 == 0) printf(" ");
		
		/* LFSR update */
		int feedback = (r ^ (r >> 4)) & 1;
		r = (r >> 1) | (feedback << 8);
	}
	printf("\n");
}

/* Test BCH encoding pattern */
static void test_bch_pattern(void)
{
	printf("\n=== Test: BCH Encoding Pattern ===\n");
	printf("\nBCH(63,44) encoding adds 19 check bits to 44 data bits.\n");
	printf("This is used for error correction in DSR audio blocks.\n\n");
	
	/* Show example: encode a simple pattern */
	uint8_t test_data[8] = {0};
	
	/* Set first bit to 1 */
	test_data[0] = 0x80;
	printf("Input data (first bit set):\n");
	print_bits(test_data, 8, "Data");
	
	/* Note: We can't directly test _bch_encode_63_44 as it's static,
	 * but we can explain the pattern */
	printf("\nBCH encoding process:\n");
	printf("1. Read 44 data bits\n");
	printf("2. Calculate 19 check bits using polynomial 0x8751\n");
	printf("3. Append check bits to data\n");
	printf("4. Result: 63 bits total (44 data + 19 check)\n");
}

/* Test PS (Programme Service) encoding */
static void test_ps_encoding(void)
{
	printf("\n=== Test: PS (Programme Service) Encoding ===\n");
	printf("\nPS encoding converts UTF-8 text to DSR character set.\n\n");
	
	const char *test_strings[] = {
		"HELLO",
		"Test123",
		"Radio",
		"ABC",
		NULL
	};
	
	for(int i = 0; test_strings[i] != NULL; i++) {
		uint8_t encoded[8];
		char decoded[9];
		
		dsr_encode_ps(encoded, test_strings[i]);
		dsr_decode_ps(decoded, encoded);
		
		printf("Input:  \"%s\"\n", test_strings[i]);
		print_hex(encoded, 8, "Encoded");
		printf("Decoded: \"%s\"\n", decoded);
		printf("\n");
	}
}

/* Test 77-bit block structure */
static void test_77block_structure(void)
{
	printf("\n=== Test: 77-bit Block Structure ===\n");
	printf("\nA 77-bit block contains:\n");
	printf("  - Bits 0-10:   l1 >> 3 (11 bits, left channel 1, MSB)\n");
	printf("  - Bits 11-21:  r1 >> 3 (11 bits, right channel 1, MSB)\n");
	printf("  - Bits 22-32:  l2 >> 3 (11 bits, left channel 2, MSB)\n");
	printf("  - Bits 33-43:  r2 >> 3 (11 bits, right channel 2, MSB)\n");
	printf("  - Bits 44-62:  BCH check bits (19 bits)\n");
	printf("  - Bit 63:      zi1 (zero indicator 1)\n");
	printf("  - Bit 64:      zi2 (zero indicator 2)\n");
	printf("  - Bits 65-67:  l1 & 0x07 (3 bits, left channel 1, LSB)\n");
	printf("  - Bits 68-70:  r1 & 0x07 (3 bits, right channel 1, LSB)\n");
	printf("  - Bits 71-73:  l2 & 0x07 (3 bits, left channel 2, LSB)\n");
	printf("  - Bits 74-76:  r2 & 0x07 (3 bits, right channel 2, LSB)\n");
	printf("\nTotal: 77 bits\n");
	
	/* Show example */
	printf("\nExample with sample values:\n");
	printf("  l1 = 0x1234, r1 = 0x5678, l2 = 0x9ABC, r2 = 0xDEF0\n");
	printf("  l1 >> 3 = 0x%03X (11 bits)\n", 0x1234 >> 3);
	printf("  r1 >> 3 = 0x%03X (11 bits)\n", 0x5678 >> 3);
	printf("  l2 >> 3 = 0x%03X (11 bits)\n", 0x9ABC >> 3);
	printf("  r2 >> 3 = 0x%03X (11 bits)\n", 0xDEF0 >> 3);
	printf("  l1 & 0x07 = 0x%X (3 bits)\n", 0x1234 & 0x07);
	printf("  r1 & 0x07 = 0x%X (3 bits)\n", 0x5678 & 0x07);
	printf("  l2 & 0x07 = 0x%X (3 bits)\n", 0x9ABC & 0x07);
	printf("  r2 & 0x07 = 0x%X (3 bits)\n", 0xDEF0 & 0x07);
}

/* Test frame structure */
static void test_frame_structure(void)
{
	printf("\n=== Test: DSR Frame Structure ===\n");
	printf("\nA DSR frame contains:\n");
	printf("  - Bits 0-10:   Sync word (0x712 for frame A, ~0x712 for frame B)\n");
	printf("  - Bit 11:      Special service bit (SA)\n");
	printf("  - Bits 12-165: First set of interleaved 77-bit blocks\n");
	printf("  - Bits 166-319: Second set of interleaved 77-bit blocks\n");
	printf("  - Bits 320+:   PRBS spectrum shaping\n");
	printf("\nTotal frame size: 320 bits = 40 bytes\n");
}

/* Main test function */
int main(int argc, char **argv)
{
	printf("========================================\n");
	printf("DSR Encoder Test Suite\n");
	printf("========================================\n");
	printf("\nThis program tests and demonstrates key DSR encoding functions.\n");
	printf("Each test shows the internal workings with trace output.\n");
	
	test_bits_write();
	test_interleaving();
	test_prbs_pattern();
	test_bch_pattern();
	test_ps_encoding();
	test_77block_structure();
	test_frame_structure();
	
	printf("\n========================================\n");
	printf("All tests completed!\n");
	printf("========================================\n");
	
#ifdef DSR_ENABLE_TEST
	/* Check if interleaving table validation found errors */
	/* This is a simple check - in a real test framework we'd return error codes */
	int has_errors = 0;
	for(int i = 0; i < 256; i++) {
		uint16_t expected = 0;
		for(int bit = 0; bit < 8; bit++) {
			if(i & (1 << bit)) {
				expected |= (1 << (bit * 2));
			}
		}
		if(_ileave[i] != expected) {
			has_errors = 1;
			break;
		}
	}
	if(has_errors) {
		printf("\nWARNING: Interleaving table contains errors!\n");
		return 1;
	}
#endif
	
	return 0;
}

