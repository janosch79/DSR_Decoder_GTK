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

/* A small procedural chiptune generator, in the style of an 8-bit home
 * computer/console sound chip: two square-wave "channels" (a fast
 * arpeggiated lead and a held bass note) stepping through an original
 * i-VI-III-VII style chord loop, a very common progression in early
 * computer game music. This is a generative composition, not a
 * transcription of any existing game's soundtrack. */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "src.h"

#define SRC_MUSIC_BPM            150.0
#define SRC_MUSIC_BEATS_PER_CHORD 2
#define SRC_MUSIC_ARP_STEPS       4

typedef struct {

	int root;   /* MIDI note number of the chord root, bass register */
	int minor;  /* 0 = major triad, 1 = minor triad */

} src_music_chord_t;

/* Em - C - G - D, a four-chord loop typical of 1980s 8-bit game themes */
static const src_music_chord_t _src_music_progression[] = {
	{ 40, 1 }, /* Em (E2) */
	{ 36, 0 }, /* C  (C2) */
	{ 43, 0 }, /* G  (G2) */
	{ 38, 0 }, /* D  (D2) */
};

#define SRC_MUSIC_PROGRESSION_LEN \
	(sizeof(_src_music_progression) / sizeof(_src_music_progression[0]))

static const int _src_music_major_arp[SRC_MUSIC_ARP_STEPS] = { 0, 4, 7, 4 };
static const int _src_music_minor_arp[SRC_MUSIC_ARP_STEPS] = { 0, 3, 7, 3 };

typedef struct {

	int16_t *audio;
	int audio_len;

	double level;

	/* Chord sequencer */
	unsigned int chord_index;
	double chord_time;

	/* Lead arpeggio */
	unsigned int arp_index;
	double arp_time;
	double lead_phase;
	double lead_freq;

	/* Bass note */
	double bass_phase;
	double bass_freq;

} src_music_t;

static double _src_music_note_freq(int note)
{
	return(440.0 * pow(2.0, (note - 69) / 12.0));
}

static int _src_music_read(src_music_t *src, int16_t *audio[2], int audio_step[2])
{
	int i;
	const double dt = 1.0 / SRC_SAMPLE_RATE;
	const double chord_dt = 60.0 / SRC_MUSIC_BPM * SRC_MUSIC_BEATS_PER_CHORD;
	const double arp_dt = chord_dt / (SRC_MUSIC_BEATS_PER_CHORD * SRC_MUSIC_ARP_STEPS);

	for(i = 0; i < src->audio_len; i++)
	{
		double lead, bass, mix;

		/* Advance to the next chord in the loop */
		src->chord_time -= dt;
		if(src->chord_time <= 0)
		{
			const src_music_chord_t *c;

			src->chord_time += chord_dt;
			src->chord_index = (src->chord_index + 1) % SRC_MUSIC_PROGRESSION_LEN;

			c = &_src_music_progression[src->chord_index];
			src->bass_freq = _src_music_note_freq(c->root);
		}

		/* Advance the lead arpeggio within the current chord */
		src->arp_time -= dt;
		if(src->arp_time <= 0)
		{
			const src_music_chord_t *c;
			const int *offsets;

			src->arp_time += arp_dt;
			src->arp_index = (src->arp_index + 1) % SRC_MUSIC_ARP_STEPS;

			c = &_src_music_progression[src->chord_index];
			offsets = c->minor ? _src_music_minor_arp : _src_music_major_arp;
			src->lead_freq = _src_music_note_freq(c->root + 12 + offsets[src->arp_index]);
		}

		/* Lead: narrow-duty square wave, one octave + arpeggio above the bass */
		lead = fmod(src->lead_phase, 1.0) < 0.25 ? 1.0 : -1.0;
		src->lead_phase += src->lead_freq / SRC_SAMPLE_RATE;
		if(src->lead_phase >= 1.0) src->lead_phase -= 1.0;

		/* Bass: 50% duty square wave, held for the whole chord */
		bass = fmod(src->bass_phase, 1.0) < 0.5 ? 1.0 : -1.0;
		src->bass_phase += src->bass_freq / SRC_SAMPLE_RATE;
		if(src->bass_phase >= 1.0) src->bass_phase -= 1.0;

		mix = (lead * 0.6 + bass * 0.4) * src->level;
		if(mix > 1.0) mix = 1.0;
		else if(mix < -1.0) mix = -1.0;

		src->audio[i] = mix * INT16_MAX;
	}

	/* Map our mono signal to the two stereo track */
	audio[0] = audio[1] = src->audio;
	audio_step[0] = audio_step[1] = 1;

	return(src->audio_len);
}

static int _src_music_close(src_music_t *src)
{
	free(src->audio);
	free(src);
	return(0);
}

int src_music_open(src_t *s, double level)
{
	src_music_t *src;

	memset(s, 0, sizeof(src_t));

	src = calloc(1, sizeof(src_music_t));
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

	/* Start on the first chord/arpeggio step on the very first sample */
	src->chord_index = SRC_MUSIC_PROGRESSION_LEN - 1;
	src->chord_time = 0;
	src->arp_index = SRC_MUSIC_ARP_STEPS - 1;
	src->arp_time = 0;
	src->bass_freq = _src_music_note_freq(_src_music_progression[0].root);
	src->lead_freq = _src_music_note_freq(_src_music_progression[0].root + 12);

	/* Register the callback functions */
	s->private = src;
	s->read = (src_read_t) _src_music_read;
	s->close = (src_close_t) _src_music_close;

	return(0);
}
