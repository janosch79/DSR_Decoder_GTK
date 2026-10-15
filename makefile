# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 janosch79
CC ?= cc
CFLAGS += -O3 -march=native -ffast-math -funroll-loops -Wall -Wextra -std=c11
# CFLAGS += -DDEMOD_DEBUG  # Uncomment for debugging

# ALSA via pkg-config (falls back to -lasound)
ALSA_CFLAGS := $(shell pkg-config --cflags alsa 2>/dev/null)
ALSA_LIBS   := $(shell pkg-config --libs   alsa 2>/dev/null)
ifeq ($(strip $(ALSA_LIBS)),)
  ALSA_LIBS := -lasound
endif

CFLAGS += $(ALSA_CFLAGS)

# HackRF RX (--hackrf / GUI source) via pkg-config; optional: without libhackrf-dev
# hackrf_rx.c builds as a stub and the HackRF source reports itself unavailable.
HACKRF_LIBS := $(shell pkg-config --libs libhackrf 2>/dev/null)
ifneq ($(strip $(HACKRF_LIBS)),)
  CFLAGS += -DHAVE_HACKRF $(shell pkg-config --cflags libhackrf 2>/dev/null)
endif

# Targets/Objs
DEC_OBJS   = decoder.o wave.o alsa.o pa.o demod.o hackrf_rx.o decodermain.o

all: dsrdec

# Optional GTK 3 frontend (make gtk) — needs libgtk-3-dev; the CLI build above does not.
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs   gtk+-3.0 2>/dev/null)
GTK_OBJS   = decoder.o wave.o alsa.o pa.o demod.o hackrf_rx.o dsr_engine.o gtkmain.o

gtk: dsrdec-gtk

dsrdec-gtk: $(GTK_OBJS)
	$(CC) $(CFLAGS) -pthread -o $@ $(GTK_OBJS) -lm $(ALSA_LIBS) $(HACKRF_LIBS) $(GTK_LIBS)

gtkmain.o: gtkmain.c dsr_engine.h decoder.h pa.h
	@if [ -z "$(GTK_LIBS)" ]; then echo "gtk+-3.0 not found via pkg-config (install libgtk-3-dev)"; exit 1; fi
	$(CC) $(CFLAGS) -std=gnu11 $(GTK_CFLAGS) -c -o $@ $<

dsr_engine.o: dsr_engine.c dsr_engine.h decoder.h pa.h alsa.h demod.h wave.h hackrf_rx.h
decodermain.o: decodermain.c decoder.h wave.h alsa.h pa.h demod.h hackrf_rx.h
demod.o: demod.c demod.h
hackrf_rx.o: hackrf_rx.c hackrf_rx.h

dsrdec: $(DEC_OBJS)
	$(CC) $(CFLAGS) -pthread -o $@ $(DEC_OBJS) -lm $(ALSA_LIBS) $(HACKRF_LIBS)

# --- Bundled DSR encoder dsrtx (make encoder) ----------------------------------
# Needs libhackrf-dev; the ffmpeg dev libs are optional (without them the
# internet stream channels don't work). Objects stay next to the sources in
# dsr_encoder/src, built with the encoder's own flags (not the decoder's CFLAGS).
ENC_DIR    = dsr_encoder
ENC_SRC    = $(ENC_DIR)/src
ENC_BIN    = $(ENC_SRC)/dsrtx
ENC_TEST   = $(ENC_SRC)/test_dsr

ENC_FFMPEG_PKGS := libavcodec libavformat libavdevice libswresample libavutil
ENC_HAVE_HACKRF := $(shell pkg-config --exists libhackrf 2>/dev/null && echo 1)
ENC_HAVE_FFMPEG := $(shell pkg-config --exists $(ENC_FFMPEG_PKGS) 2>/dev/null && echo 1)
ENC_PKGS        := libhackrf $(if $(ENC_HAVE_FFMPEG),$(ENC_FFMPEG_PKGS))

ENC_CFLAGS  = -g -Wall -O3 -pthread -MMD -MP $(if $(ENC_HAVE_FFMPEG),-DHAVE_FFMPEG) \
              $(shell pkg-config --cflags $(ENC_PKGS) 2>/dev/null)
ENC_LDLIBS  = -lm -pthread $(shell pkg-config --libs $(ENC_PKGS) 2>/dev/null)

ENC_OBJS   = $(addprefix $(ENC_SRC)/, dsrtx.o dsr.o bits.o conf.o src.o src_tone.o src_noise.o \
             src_music.o src_rawaudio.o rf.o rf_file.o rf_hackrf.o udpsink.o tcpsink.o \
             $(if $(ENC_HAVE_FFMPEG),src_ffmpeg.o))
# Legacy encoder test: dsr.c rebuilt with DSR_ENABLE_TEST to export internal tables
ENC_TEST_OBJS = $(addprefix $(ENC_SRC)/, test_dsr.o dsr_test.o bits.o dsr_trace.o)

encoder: $(ENC_BIN)

$(ENC_BIN): $(ENC_OBJS)
	$(CC) $(ENC_CFLAGS) -o $@ $(ENC_OBJS) $(ENC_LDLIBS)

$(ENC_SRC)/%.o: $(ENC_SRC)/%.c
	@if [ -z "$(ENC_HAVE_HACKRF)" ]; then echo "libhackrf not found via pkg-config (install libhackrf-dev)"; exit 1; fi
	$(CC) $(ENC_CFLAGS) -c -o $@ $<

$(ENC_SRC)/dsr_test.o: $(ENC_SRC)/dsr.c
	$(CC) $(ENC_CFLAGS) -DDSR_ENABLE_TEST -c -o $@ $<

$(ENC_SRC)/test_dsr.o $(ENC_SRC)/dsr_trace.o: $(ENC_SRC)/%.o: $(ENC_SRC)/%.c
	$(CC) $(ENC_CFLAGS) -DDSR_ENABLE_TEST -c -o $@ $<

$(ENC_TEST): $(ENC_TEST_OBJS)
	$(CC) $(ENC_CFLAGS) -o $@ $(ENC_TEST_OBJS) $(ENC_LDLIBS)

encoder-test: $(ENC_TEST)
	cd $(ENC_SRC) && ./test_dsr

# Ceedling unit tests (dsr_encoder/test, configured in dsr_encoder/project.yml).
# Prefers a user-installed gem (gem install ceedling --user-install).
CEEDLING := $(firstword $(wildcard $(HOME)/.local/share/gem/ruby/*/bin/ceedling))
ifeq ($(CEEDLING),)
  CEEDLING := ceedling
endif

encoder-test-ceedling:
	@command -v "$(CEEDLING)" >/dev/null 2>&1 || \
		{ echo "Ceedling not found. Install with: gem install ceedling --user-install"; exit 127; }
	cd $(ENC_DIR) && $(CEEDLING) test:all

encoder-clean:
	rm -f $(ENC_BIN) $(ENC_TEST) $(ENC_SRC)/*.o $(ENC_SRC)/*.d
	rm -rf $(ENC_DIR)/build

-include $(wildcard $(ENC_SRC)/*.d)

# --- Clean ----------------------------------------------------------------------
clean: encoder-clean
	rm -f dsrdec dsrdec-gtk $(DEC_OBJS) dsr_engine.o gtkmain.o

.PHONY: all gtk encoder encoder-test encoder-test-ceedling encoder-clean clean

