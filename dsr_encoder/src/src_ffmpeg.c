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

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <pthread.h>
#include "src.h"

/* Live (network) inputs: a reader thread decodes into a FIFO so the encoder never
 * blocks on the network. dsrtx runs on its own clock; a stream that delivers late or in
 * bursts only gets a gap of silence in its own channel instead of slowing down or
 * stopping the whole mux. */
#define LIVE_PREFILL_FRAMES (SRC_SAMPLE_RATE)       /* 1 s cushion before (re)starting */
#define LIVE_FIFO_FRAMES    (SRC_SAMPLE_RATE * 3)   /* 3 s; on overflow the oldest is dropped */
#define LIVE_CHUNK_FRAMES   1152                    /* max. frames returned per read */
#define LIVE_SILENCE_FRAMES 64                      /* one DSR block (2 ms) of silence */

typedef struct {
	
	AVFormatContext *format_ctx;
	
	/* Audio decoder */
	AVStream *audio_stream;
	AVCodecContext *audio_codec_ctx;
	AVFrame *frame;
	
	/* Audio resampler */
	struct SwrContext *swr_ctx;
	int16_t *audio;
	int audio_len;
	
	/* Audio level */
	double level;
	
	/* Live input: reader thread -> FIFO (interleaved stereo frames) */
	int live;
	char *url;
	pthread_t thread;
	pthread_mutex_t mu;
	int16_t *fifo;
	uint64_t head, tail;	/* frames written / read, under mu */
	int primed;		/* consumer side: cushion reached, playing */
	int eof;		/* reader thread finished (stream ended or failed) */
	volatile int stop;
	uint64_t underruns;
	int16_t out[LIVE_CHUNK_FRAMES * 2];
	
} src_ffmpeg_t;

static void _print_ffmpeg_error(int r)
{
	char sb[128];
	const char *sp = sb;
	
	if(av_strerror(r, sb, sizeof(sb)) < 0)
	{
		sp = strerror(AVUNERROR(r));
	}
	
	fprintf(stderr, "%s\n", sp);
}

static int _src_ffmpeg_read(src_ffmpeg_t *src, int16_t *audio[2], int audio_step[2])
{
	AVPacket pkt;
	int r;
	
	while((r = avcodec_receive_frame(src->audio_codec_ctx, src->frame)) == AVERROR(EAGAIN))
	{
		while((r = av_read_frame(src->format_ctx, &pkt)) >= 0)
		{
			if(pkt.stream_index == src->audio_stream->index)
			{
				/* Got an audio packet */
				avcodec_send_packet(src->audio_codec_ctx, &pkt);
				av_packet_unref(&pkt);
				break;
			}
			
			av_packet_unref(&pkt);
		}
		
		if(r < 0) break;
	}
	
	if(r < 0)
	{
		return(-1);
	}
	
	/* We have received a frame! Resample and return */
	r = swr_convert(
		src->swr_ctx,
		(uint8_t **) &src->audio,
		src->audio_len,
		(const uint8_t **) src->frame->data,
		src->frame->nb_samples
	);
	
	av_frame_unref(src->frame);
	
	/* Apply level scaling if level is not 1.0 */
	if(src->level != 1.0 && r > 0)
	{
		int i;
		int samples = r * 2; /* Stereo samples */
		for(i = 0; i < samples; i++)
		{
			src->audio[i] = (int16_t)(src->audio[i] * src->level);
		}
	}
	
	audio[0] = src->audio + 0;
	audio[1] = src->audio + 1;
	audio_step[0] = audio_step[1] = 2;
	
	return(r);
}

static int _live_interrupt(void *opaque)
{
	return(((src_ffmpeg_t *) opaque)->stop);
}

static void *_live_thread(void *arg)
{
	src_ffmpeg_t *src = arg;
	int16_t *audio[2];
	int audio_step[2];
	int r;
	
	while(!src->stop && (r = _src_ffmpeg_read(src, audio, audio_step)) >= 0)
	{
		pthread_mutex_lock(&src->mu);
		for(int i = 0; i < r; i++)
		{
			if(src->head - src->tail == LIVE_FIFO_FRAMES) src->tail++;
			int16_t *f = &src->fifo[(src->head % LIVE_FIFO_FRAMES) * 2];
			f[0] = audio[0][i * audio_step[0]];
			f[1] = audio[1][i * audio_step[1]];
			src->head++;
		}
		pthread_mutex_unlock(&src->mu);
	}
	
	pthread_mutex_lock(&src->mu);
	src->eof = 1;
	pthread_mutex_unlock(&src->mu);
	if(!src->stop) fprintf(stderr, "ffmpeg: '%s' ended\n", src->url);
	
	return(NULL);
}

/* Never blocks: returns buffered audio, or a block of silence while the cushion is
 * being (re)built. */
static int _src_ffmpeg_read_live(src_ffmpeg_t *src, int16_t *audio[2], int audio_step[2])
{
	uint64_t avail;
	int n;
	
	pthread_mutex_lock(&src->mu);
	avail = src->head - src->tail;
	
	if(avail == 0 && src->eof)
	{
		pthread_mutex_unlock(&src->mu);
		return(-1);
	}
	
	if(!src->primed && (avail >= LIVE_PREFILL_FRAMES || src->eof))
	{
		src->primed = 1;
	}
	else if(src->primed && avail == 0)
	{
		src->primed = 0;
		src->underruns++;
		fprintf(stderr, "ffmpeg: '%s' late, inserting silence (underrun %llu)\n",
			src->url, (unsigned long long) src->underruns);
	}
	
	if(!src->primed)
	{
		pthread_mutex_unlock(&src->mu);
		memset(src->out, 0, sizeof(int16_t) * 2 * LIVE_SILENCE_FRAMES);
		n = LIVE_SILENCE_FRAMES;
	}
	else
	{
		n = avail < LIVE_CHUNK_FRAMES ? (int) avail : LIVE_CHUNK_FRAMES;
		for(int i = 0; i < n; i++)
		{
			const int16_t *f = &src->fifo[((src->tail + i) % LIVE_FIFO_FRAMES) * 2];
			src->out[i * 2 + 0] = f[0];
			src->out[i * 2 + 1] = f[1];
		}
		src->tail += n;
		pthread_mutex_unlock(&src->mu);
	}
	
	audio[0] = src->out + 0;
	audio[1] = src->out + 1;
	audio_step[0] = audio_step[1] = 2;
	
	return(n);
}

int src_ffmpeg_is_live(const char *input_url)
{
	return(strstr(input_url, "://") != NULL && strncmp(input_url, "file:", 5) != 0);
}

static int _src_ffmpeg_close(src_ffmpeg_t *src)
{
	if(src->live)
	{
		src->stop = 1;
		pthread_join(src->thread, NULL);
		pthread_mutex_destroy(&src->mu);
		free(src->fifo);
		free(src->url);
	}
	
	avcodec_free_context(&src->audio_codec_ctx);
	swr_free(&src->swr_ctx);
	av_frame_free(&src->frame);
	av_free(src->audio);
	avformat_close_input(&src->format_ctx);
	free(src);
	
	return(0);
}

int src_ffmpeg_open(src_t *s, const char *input_url, double level)
{
	src_ffmpeg_t *src;
	const AVCodec *codec;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
        AVChannelLayout dst_ch_layout = AV_CHANNEL_LAYOUT_STEREO;
#endif
	int r;
	int i;
	
	memset(s, 0, sizeof(src_t));
	
	src = calloc(1, sizeof(src_ffmpeg_t));
	if(!src)
	{
		return(-1);
	}
	
	/* Use 'pipe:' for stdin */
	if(strcmp(input_url, "-") == 0)
	{
		input_url = "pipe:";
	}
	
	/* Open the source */
	if((r = avformat_open_input(&src->format_ctx, input_url, NULL, NULL)) < 0)
	{
		fprintf(stderr, "Error opening file '%s'\n", input_url);
		_print_ffmpeg_error(r);
		return(-1);
	}
	
	/* Read stream info from the file */
	if(avformat_find_stream_info(src->format_ctx, NULL) < 0)
	{
		fprintf(stderr, "Error reading stream information from file\n");
		return(-1);
	}
	
	/* Dump some useful information to stderr */
	fprintf(stderr, "Opening '%s'...\n", input_url);
	av_dump_format(src->format_ctx, 0, input_url, 0);
	
	/* Find the first audio stream */
	/* TODO: Allow the user to select streams by number or name */
	src->audio_stream = NULL;
	
	for(i = 0; i < src->format_ctx->nb_streams; i++)
	{
		if(src->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
			if(src->format_ctx->streams[i]->codecpar->ch_layout.nb_channels <= 0) continue;
#else
			if(src->format_ctx->streams[i]->codecpar->channels <= 0) continue;
#endif
			src->audio_stream = src->format_ctx->streams[i];
			break;
		}
	}
	
	/* No audio? */
	if(src->audio_stream == NULL)
	{
		fprintf(stderr, "No audio streams found\n");
		return(-1);
	}
	
	fprintf(stderr, "Using audio stream %d.\n", src->audio_stream->index);
	
	/* Get a pointer to the codec context for the audio stream */
	src->audio_codec_ctx = avcodec_alloc_context3(NULL);
	if(!src->audio_codec_ctx)
	{
		return(-1);
	}
	
	if(avcodec_parameters_to_context(src->audio_codec_ctx, src->audio_stream->codecpar) < 0)
	{
		return(-1);
	}
	
	src->audio_codec_ctx->thread_count = 0; /* Let ffmpeg decide number of threads */
	
	/* Find the decoder for the audio stream */
	codec = avcodec_find_decoder(src->audio_codec_ctx->codec_id);
	if(codec == NULL)
	{
		fprintf(stderr, "Unsupported audio codec\n");
		return(-1);
	}
	
	/* Open audio codec */
	if(avcodec_open2(src->audio_codec_ctx, codec, NULL) < 0)
	{
		fprintf(stderr, "Error opening audio codec\n");
		return(-1);
	}
	
	/* Allocate AVFrame */
	src->frame = av_frame_alloc();
	if(!src->frame)
	{
		fprintf(stderr, "Error allocating memory for AVFrame\n");
		return(-1);
	}
	
	/* Prepare the resampler */
	src->swr_ctx = swr_alloc();
	if(!src->swr_ctx)
	{
		return(-1);
	}
	
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
	av_opt_set_chlayout(src->swr_ctx, "in_chlayout",     &src->audio_codec_ctx->ch_layout, 0);
	av_opt_set_int(src->swr_ctx, "in_sample_rate",       src->audio_codec_ctx->sample_rate, 0);
	av_opt_set_sample_fmt(src->swr_ctx, "in_sample_fmt", src->audio_codec_ctx->sample_fmt, 0);
	
	av_opt_set_chlayout(src->swr_ctx, "out_chlayout",    &dst_ch_layout, 0);
#else
	if(!src->audio_codec_ctx->channel_layout)
	{
		/* Set the default layout for codecs that don't specify any */
		src->audio_codec_ctx->channel_layout = av_get_default_channel_layout(src->audio_codec_ctx->channels);
	}
	
	av_opt_set_int(src->swr_ctx, "in_channel_layout",    src->audio_codec_ctx->channel_layout, 0);
	av_opt_set_int(src->swr_ctx, "in_sample_rate",       src->audio_codec_ctx->sample_rate, 0);
	av_opt_set_sample_fmt(src->swr_ctx, "in_sample_fmt", src->audio_codec_ctx->sample_fmt, 0);
	
	av_opt_set_int(src->swr_ctx, "out_channel_layout",    AV_CH_LAYOUT_STEREO, 0);
#endif
	
	av_opt_set_int(src->swr_ctx, "out_sample_rate",       SRC_SAMPLE_RATE, 0);
	av_opt_set_sample_fmt(src->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
	
	if(swr_init(src->swr_ctx) < 0)
	{
		fprintf(stderr, "Failed to initialise the resampling context\n");
		return(-1);
	}
	
	/* Calculate the number of samples needed for output */
	src->audio_len = av_rescale_rnd(
		src->audio_codec_ctx->frame_size, /* Can this be trusted? */
		SRC_SAMPLE_RATE,
		src->audio_codec_ctx->sample_rate,
		AV_ROUND_UP
	);
	
	if(src->audio_len <= 0)
	{
		src->audio_len = SRC_SAMPLE_RATE;
	}
	
	src->audio = av_malloc(sizeof(int16_t) * 2 * src->audio_len);
	
	/* Store the audio level */
	src->level = level;
	
	/* Register the callback functions */
	s->private = src;
	s->read = (src_read_t) _src_ffmpeg_read;
	s->close = (src_close_t) _src_ffmpeg_close;
	
	if(src_ffmpeg_is_live(input_url))
	{
		src->url = strdup(input_url);
		src->fifo = malloc(sizeof(int16_t) * 2 * LIVE_FIFO_FRAMES);
		if(!src->url || !src->fifo)
		{
			free(src->url);
			free(src->fifo);
			return(-1);
		}
		pthread_mutex_init(&src->mu, NULL);
		src->format_ctx->interrupt_callback.callback = _live_interrupt;
		src->format_ctx->interrupt_callback.opaque = src;
		if(pthread_create(&src->thread, NULL, _live_thread, src) != 0)
		{
			fprintf(stderr, "Error starting the reader thread for '%s'\n", input_url);
			pthread_mutex_destroy(&src->mu);
			free(src->url);
			free(src->fifo);
			return(-1);
		}
		src->live = 1;
		s->read = (src_read_t) _src_ffmpeg_read_live;
	}
	
	return(0);
}

void src_ffmpeg_init(void)
{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
	av_register_all();
#endif
	avdevice_register_all();
	avformat_network_init();
}

void src_ffmpeg_deinit(void)
{
	avformat_network_deinit();
}

