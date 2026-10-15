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
/*                                                                       */
/* This code has been modified by janosch79.                             */
/* This code is provided "as is" without warranty of any kind, express  */
/* or implied. The user assumes full responsibility for any use of      */
/* this code.                                                            */

#include <stdlib.h>
#include <string.h>
#include "src.h"

typedef struct {

	int16_t *audio;
	int audio_len;

	double level;
	uint32_t state;

} src_noise_t;

/* xorshift32: fast, deterministic given a fixed seed so the same config
 * always renders bit-identical noise. Not cryptographic. */
static uint32_t _xorshift32(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;

	return(*state = x);
}

static int _src_noise_read(src_noise_t *src, int16_t *audio[2], int audio_step[2])
{
	int i;

	/* Render white noise, uniform over the full sample range */
	for(i = 0; i < src->audio_len; i++)
	{
		int32_t r = (int32_t)(_xorshift32(&src->state) & 0xFFFF) - 32768;
		src->audio[i] = r * src->level;
	}

	/* Map our mono signal to the two stereo track */
	audio[0] = audio[1] = src->audio;
	audio_step[0] = audio_step[1] = 1;

	return(src->audio_len);
}

static int _src_noise_close(src_noise_t *src)
{
	free(src->audio);
	free(src);
	return(0);
}

int src_noise_open(src_t *s, double level, unsigned int seed)
{
	src_noise_t *src;

	memset(s, 0, sizeof(src_t));

	src = calloc(1, sizeof(src_noise_t));
	if(!src)
	{
		return(-1);
	}

	/* Allocate memory for output buffer (1 second) */
	src->audio_len = SRC_SAMPLE_RATE;
	src->audio = malloc(src->audio_len * sizeof(int16_t));
	if(!src->audio)
	{
		free(src);
		return(-1);
	}

	src->level = level;
	src->state = seed != 0 ? seed : 1; /* xorshift requires a non-zero state */

	/* Register the callback functions */
	s->private = src;
	s->read = (src_read_t) _src_noise_read;
	s->close = (src_close_t) _src_noise_close;

	return(0);
}
