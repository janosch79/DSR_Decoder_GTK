# DSR_Decoder
by janosch79 2025 · **Version 0.9.6** · [Deutsche Version](README.de.md)
C application to decode DSR (Digital Satellite Radio) signals from I/Q sample files, raw bitstream files, or a live UDP stream — with built-in IQ demodulation and ALSA/WAV audio output. Available as a command-line tool (`dsrdec`) and as an optional GTK 3 GUI (`dsrdec-gtk`).

A matching DSR encoder (`dsrtx`) is bundled in [dsr_encoder/](dsr_encoder/), so you can run a complete transmitter → receiver chain on one machine.

## Quick start

```bash
git clone https://github.com/janosch79/DSR_Decoder_GTK.git
cd DSR_Decoder

# Build dependencies (Debian/Ubuntu)
sudo apt install build-essential pkg-config libasound2-dev libgtk-3-dev \
     libhackrf-dev libavcodec-dev libavformat-dev libavdevice-dev libswresample-dev libavutil-dev

# Build and start (the first run compiles encoder and decoder, takes a few seconds)
./DSRDemo start
```

On WSL2 (Windows), also set up the ALSA → PulseAudio bridge once, otherwise there is no sound: see [Audio on WSL2](#audio-on-wsl2).

There is no separate build step: `./DSRDemo start` runs `make` itself and builds whatever is missing or changed (see [Build](#build) to build by hand). If a library is missing, it tells you which `apt install` to run.

`DSRDemo` runs the whole chain: the encoder takes 16 internet radio streams from its config, packs them into one DSR multiplex and sends the raw bitstream over UDP to the decoder.

| Command | Effect |
|---------|--------|
| `./DSRDemo start [options]` | Builds encoder and decoder (only what changed), starts the encoder in the background, waits until it sends (it opens its streams one after another, usually 5–10 s), then starts the decoder |
| `./DSRDemo stop` | Stops the decoder cleanly (the GUI saves its settings), then the encoder. Works from any terminal |
| `./DSRDemo restart [options]` | `stop` + `start`; without options it reuses those of the last start |
| `./DSRDemo status` | Shows whether encoder and decoder are running, config and UDP target |

The GUI decoder runs detached, so the terminal is free again; closing its window also stops the encoder. The CLI decoder runs in the terminal (keys `u`/`d`, `t`, `h`, `q`); quitting it or pressing Ctrl+C stops the encoder too.

| Option | Effect |
|--------|--------|
| `-c FILE` | Encoder config (default [dsr_encoder/src/lifeorg.conf](dsr_encoder/src/lifeorg.conf); also `hq.conf`, `eupaket.conf`) |
| `--gui` / `--cli` | GTK GUI or command-line decoder (default: GUI if a display and GTK are available) |
| `--stereo L,R` / `--mono N` | Initial channel |
| `--wait SEC` | Max. time to wait for the encoder to start sending (default 60) |
| `--rebuild` / `--no-build` | Clean rebuild / skip the build |
| `-- ...` | Pass further options to the decoder |

Notes:
- The UDP port is read from the config's `[output]` section (`udp://127.0.0.1:5000` in the bundled configs). If the port is already taken, for example by a decoder that is still running, `start` stops with a message.
- PIDs and logs are kept in `run/` (`run/encoder.log`, `run/decoder.log` for the GUI).
- The bundled configs use internet radio streams, so the encoder needs network access.

## Build

| Command | Builds |
|---------|--------|
| `make` | Command-line decoder `dsrdec` (default) |
| `make gtk` | GTK GUI `dsrdec-gtk` (optional) |
| `make encoder` | Bundled encoder `dsr_encoder/src/dsrtx` (optional) |
| `make encoder-test` | Builds and runs the encoder's legacy test `test_dsr` |
| `make encoder-test-ceedling` | Runs the encoder's Ceedling unit tests in `dsr_encoder/test` (needs `gem install ceedling`) |
| `make clean` | Removes all binaries and object files (decoder and encoder) |
| `make encoder-clean` | Removes only the encoder build |

There is one makefile for everything, at the top level; the encoder's objects are kept next to its sources in `dsr_encoder/src`.

Rebuild everything: `make clean && make && make gtk`

### Dependencies
- ALSA development libraries (libasound2-dev on Debian/Ubuntu)
- Standard C library with POSIX support
- Math library (-lm)
- GUI only: GTK 3 development libraries (libgtk-3-dev on Debian/Ubuntu)
- Optional, for live reception with a HackRF One: libhackrf (libhackrf-dev)

Debian/Ubuntu:
```bash
sudo apt install build-essential pkg-config libasound2-dev   # CLI
sudo apt install libgtk-3-dev                                # additionally for the GUI
sudo apt install libhackrf-dev                               # optional: HackRF reception
```

The build system automatically detects ALSA via pkg-config, falling back to `-lasound` if needed. GTK is detected via `pkg-config gtk+-3.0`. `make gtk` stops with a hint if GTK is missing, and the CLI build does not need GTK at all. libhackrf is detected via `pkg-config libhackrf`; without it both programs still build, and the HackRF source reports that it is not built in.

### Audio on WSL2

WSL2 has no sound card of its own. WSLg provides a PulseAudio server (`PULSE_SERVER=unix:/mnt/wslg/PulseServer`), but both decoders play through ALSA, and Ubuntu on WSL does not install the bridge from ALSA to PulseAudio. Without it, live audio fails to open or stays silent. Set it up once:

```bash
sudo apt install libasound2-plugins alsa-utils    # ALSA pulse plugin + test tools

sudo tee /etc/asound.conf >/dev/null <<'EOF'
pcm.!default {
    type pulse
    hint.description "Default Audio Device"
}

ctl.!default {
    type pulse
}
EOF

aplay /usr/share/sounds/alsa/Front_Center.wav    # test: you should hear "Front Center"
```

If audio plays for a while and then stops, or `aplay` reports `PulseAudio: Unable to create stream: Timeout`, the WSLg audio server itself has stalled (this happens without the decoder too). Restarting WSL usually brings it back: run `wsl --shutdown` in PowerShell, then open the WSL terminal again.

The makefile compiles with `-O3 -march=native`, so the binaries are tuned for the build machine's CPU and may not run on other (older) CPUs. Build on the target machine, or remove `-march=native` from `CFLAGS` for a portable binary.

## Application

The project provides two independent programs that share the same decoder core:

- **dsrdec**: command-line decoder with keyboard control (see [Usage](#usage))
- **dsrdec-gtk**: optional GTK 3 GUI (see [GTK GUI](#gtk-gui-optional))

Both support four input modes:

1. **IQ file mode** (`--iqfile`) - demodulates an I/Q sample file internally (no separate tool or pipe needed) and decodes it
2. **Raw file mode** (`--in` / positional infile / `-` for stdin) - decodes an already-demodulated raw A/B bitstream
3. **UDP mode** (`--host`/`--port`) - receives a raw bitstream live over UDP (default mode when no file input is given)
4. **HackRF mode** (`--hackrf`) - receives a DSR mux live over the air or from a cable feed with a HackRF One (default 118 MHz) and demodulates it internally, see [HackRF reception](#hackrf-reception)

## Usage

The options and keys in this section are for the command-line `dsrdec`. For the GUI, see [GTK GUI](#gtk-gui-optional).

Usage: `./dsrdec [infile|-] [options]`

```
  infile              Raw byte file with interleaved A/B bitstream; '-' for stdin
  --in PATH           Same as positional infile
  --host H            Listen host (default 0.0.0.0)   [ignored if infile/iqfile given]
  --port P            UDP port (default 9000)      [ignored if infile/iqfile given]
  --iqfile PATH       Raw IQ file (s16le|f32le|s8); demodulated internally, no
                      separate dsrdemod/pipe needed. Takes priority over infile/UDP.
  --iqrate FS         IQ sample rate for --iqfile/--hackrf (default 20480000)
  --iqfmt {s16le|f32le|s8}  IQ sample format for --iqfile (default s16le;
                      s8 = hackrf_transfer recordings)
  --track             Symbol timing + carrier recovery for --iqfile (SDR recordings;
                      not needed for encoder IQ files)
  --hackrf            Live reception with a HackRF One (built-in demodulator)
  --freq MHZ          HackRF centre frequency in MHz (default 118.0)
  --lna DB            HackRF LNA gain 0..40, step 8 (default 16; disables auto gain)
  --vga DB            HackRF VGA gain 0..62, step 2 (default 20; disables auto gain)
  --amp               HackRF RF amplifier on (+14 dB)
  --no-auto-gain      Keep LNA/VGA fixed (default: adjusted to the input level)
  --bw HZ             HackRF baseband filter (default: from sample rate)
  --serial S          HackRF serial number (default: first device)
  --lsb               Bits LSB-first within byte (default MSB-first) [infile/UDP only]
  --repeat            On EOF (file input) restart from beginning
  --no-descramble     Disable PRBS descramble of payload
  --bch-check         Enable BCH(63,44) verification (no correction)
  --no-auto-deint     Disable BCH-driven auto deinterleave (use EVEN->blk0)
  --log-level LVL     Decoder log: off|sync|warn|zi (or 0..3; default off)
  --verbose           Alias for --log-level zi
  --wav FILE.wav      Write WAV (16-bit PCM, always stereo; mono output is L=R)
  --rate SR           Output sample rate (default 32000); must be 32000 * upsample
  --mono N            Use mono from subchannel N (0..31)
  --stereo L,R        Use stereo from subchannels L,R (0..31)
  --upsample N        Repeat each decoded 32 kHz sample N times (default 1);
                      set --rate to 32000*N accordingly
  --stream-focus      Decode only the selected pair (default)
  --no-stream-focus   Decode full mux (all pairs); default is stream-focus on (less CPU)
  --audioout          Enable live audio output via ALSA
  -h, --help          Show this help
```

Notes:
- Without `--track` the demodulator works at a fixed 2 samples per symbol, so `--iqrate` must be twice the symbol rate (the encoder's default 20480000 for 10.24 Msym/s). With `--track` (always on for `--hackrf`) any rate near that works, e.g. 20000000.
- The decoder outputs 32 kHz audio. `--rate` and `--upsample` are not linked in the CLI: with `--upsample N`, pass `--rate` 32000 × N yourself, otherwise audio plays too slow (the GUI does this automatically).

**Keys (runtime, interactive):**
| Key | Action |
|-----|--------|
| `u` / `d` | Channel up / down (stereo pair or mono channel) |
| `m` | Force mono mode (current channel) |
| `s` | Force stereo mode |
| `v` | Cycle decoder log level: off -> sync -> warn -> zi |
| `t` | Show channel table (all 32 subchannels) |
| `p` | Toggle PA debug (raw SA bytes on stderr) |
| `r` | Hard resync (clears state; next `r` blocked until will+sa+offset lock) |
| `h` | Show key help |
| `q` | Quit |

## Examples

**IQ file → live audio (single step, no piping needed):**
```bash
./dsrdec --iqfile signal.iq --iqrate 20480000 --iqfmt s16le --audioout --stereo 1,2
```

**UDP Mode:** Listen on local UDP port 9000 for a raw datastream (e.g. from a compatible DSR encoder) and play instantly via ALSA:
```bash
./dsrdec --port 9000 --audioout --rate 32000 --repeat --stereo 4,5
```

(note: The sender is the bundled encoder in [dsr_encoder/](dsr_encoder/); `./DSRDemo start` starts both ends for you. The bundled configs send to port 5000.)

**File Mode:** Decode from an already-demodulated raw bitstream file and play via ALSA:
```bash
./dsrdec signal.raw --audioout --rate 32000 --repeat --stereo 1,2
```

**Write to WAV instead of live playback:**
```bash
./dsrdec --iqfile signal.iq --wav out.wav --stereo 1,2
```

The WAV file is always 16-bit stereo, so the mode can be switched at runtime (`m`/`s`, auto-detect); in mono mode both channels carry the same signal.

## HackRF reception

With a HackRF One, `dsrdec` and `dsrdec-gtk` replace a DSR receiver: tune to the mux, demodulate, decode, play.

```bash
./dsrdec --hackrf --audioout                    # 118 MHz, auto gain
./dsrdec --hackrf --freq 118 --stereo 4,5 --audioout
./dsrdec-gtk --hackrf --start                   # GUI, start right away
```

118 MHz is where DSR was carried in German cable networks (special channel S2). Any other frequency can be set with `--freq` (GUI: *Frequency [MHz]*).

To receive the bundled encoder over the air you need a second HackRF (one board cannot transmit and receive at the same time): set its `[output]` to `type = hackrf` and `frequency = 118e6` (the configs show 855 MHz as an example).

> [!WARNING]
> **Never radiate the encoder's RF output over the air.** 118 MHz lies in the aeronautical VHF band (118–137 MHz, air traffic control and pilot communication), and DSR occupies about 14 MHz of bandwidth around it. Transmitting there interferes with safety-of-life services and is illegal without a frequency assignment from your national regulator (e.g. Bundesnetzagentur). The same applies to any other frequency you are not licensed for.
>
> Connect the HackRF transmitter to the receiver **by cable only**, through a suitable attenuator (e.g. 30–60 dB) or into a dummy load, with no antenna attached and the TX amplifier off. Receiving with `--hackrf` is not affected by this. See [Legal notice](#legal-notice).

**What happens after start**

1. The HackRF streams 8-bit I/Q at 20.48 MS/s (2 samples per symbol), baseband filter 20 MHz.
2. The demodulator runs in tracking mode: DC blocker for the HackRF's centre spike, Gardner symbol timing recovery, and a QPSK carrier loop. Together they correct the HackRF's crystal error (about ±20 ppm: a few kHz carrier offset at 118 MHz, and a sample clock that would otherwise slip a symbol within milliseconds). Carrier offsets up to about ±25 kHz were tested.
3. After about 13 ms the demodulator fixes its symbol mapping and the decoder looks for the mux.
4. If no mux is locked within 3 s (Williard **and** SA cycle; Williard alone can lock on noise), acquisition starts over. This also covers a mux that appears later or a wrong mapping decided on a transient. The CLI prints `[hackrf] no DSR mux locked at … - retrying`; the GUI shows *Searching for a DSR mux at … MHz* and counts *searches*.
5. **Auto gain** (default) steps LNA/VGA every 300 ms until the input level sits between −22 and −10 dBFS (8-bit ADC neither clipping nor underused). `--no-auto-gain`, `--lna`/`--vga`, or unticking *Auto gain* in the GUI keep the gains fixed. The RF amp is off unless you enable it (`--amp`), since a cable feed is usually strong.
6. If samples are lost on USB (CPU or USB too slow), the decoder resynchronizes on its own, since a gap in the bitstream would otherwise leave the programme names garbled.

**Receiver status.** The CLI prints an `[rf]` line under each `[stat]` line; the GUI shows the same values below the decoder counters:

```
[rf]   level -13.5 dBFS | CFO +3.00 kHz | clock +26.5 ppm | EVM 12 % | LNA 16 VGA 20 amp off | drops 0.0 MB
```

| Value | Meaning |
|-------|---------|
| level | Input level before the digital AGC (mean peak of I/Q). Auto gain keeps it at −22…−10 dBFS |
| CFO | Carrier offset tracked by the carrier loop (HackRF + transmitter frequency error) |
| clock | Sample clock offset between HackRF and transmitter |
| EVM | Error vector magnitude of the symbols. In tests: 3–12 % for a clean signal, still error-free decoding at ~30 % (8 dB SNR); noise alone reads ~50 % |
| drops | Samples lost on USB. If this grows, use `--iqrate 20000000` (GUI: *Rate 20.00 MS/s*), the HackRF's specified maximum |

**Setup notes**

- The HackRF must be accessible without root (udev rules from the `hackrf` package). Check with `hackrf_info`.
- WSL2: WSL sees no USB devices by default (`No HackRF found`). Pass the HackRF through with [usbipd-win](https://github.com/dorssel/usbipd-win): `usbipd list` shows its bus ID (`1d50:6089 HackRF One`); `usbipd bind --busid <id>` once from an admin shell, then `usbipd attach --wsl --busid <id>` (add `--auto-attach` to keep it attached across replugging). Attaching needs to be repeated after every replug or Windows/WSL restart; while attached, Windows itself cannot use the HackRF.
- CPU: the tracking demodulator decodes about 3× faster than real time on an i7-11800H, on a single core.

**SDR recordings.** A recording made with `hackrf_transfer` decodes the same way as live reception:

```bash
hackrf_transfer -r dsr.cs8 -f 118000000 -s 20480000 -l 16 -g 20 -n 409600000   # 20 s
./dsrdec --iqfile dsr.cs8 --iqfmt s8 --track --audioout
```

`--track` (GUI: *SDR recording*) enables the same timing and carrier recovery and the same acquisition retry. IQ files written by the bundled encoder don't need it; without `--track` they decode exactly as before.

## GTK GUI (optional)

**dsrdec-gtk** is a graphical frontend similar in spirit to [dablin](https://github.com/Opendigitalradio/dablin)'s `dablin_gtk`. It uses the same decoder, demodulator and PA modules as the CLI. The command-line `dsrdec` is unchanged and remains fully usable on its own.

Build and start:
```bash
make gtk
./dsrdec-gtk
```

On WSL2 the window is displayed via WSLg; no extra X server is needed. For sound, see [Audio on WSL2](#audio-on-wsl2).

### Window layout

| Section | Content |
|---------|---------|
| **Input** | Source (IQ file / raw bitstream file / UDP stream / HackRF One), file chooser, IQ rate and format, *SDR recording* (timing and carrier recovery for IQ files), UDP host and port, HackRF frequency, rate, auto gain, LNA/VGA and RF amp, live audio (ALSA), real-time pacing, repeat file, LSB-first, WAV recording. *Advanced decoder options*: PRBS descramble, BCH check, auto deinterleave, stream focus, upsample (output rate shown, always 32000 × upsample) |
| **Start / Stop** | Starts or stops decoding; input settings are locked while running |
| **Now playing** | Station label, channel(s), stereo/mono, programme type, music/speech. On the right: **mute** button (speaker icon) and **volume** slider (0–100 %) for live audio |
| **Channel list** | One row per programme with station label, mode, content (music/speech) and programme type from the PA data. A stereo pair is shown as **one row** (e.g. `2L/2R · Stereo`); mono subchannels, and subchannels without PA data yet, get their own row (e.g. `9L · Mono`). The playing row is marked with ▶. **Double-click** a row to tune (stereo row → pair in stereo, mono row → that subchannel in mono). At least 16 rows (one full stereo mux) are always visible; the default window height fits all 32 rows if the screen allows it. Scroll position and selection are kept when the list changes |
| **Controls** | Prev / Next channel, Stereo / Mono, Resync, decoder log level |
| **Receiver status** | Lock indicators for Williard, deinterleave, SA cycle, SA offset and station list (green = locked/confirmed, orange = waiting or frozen), audio state (warm-up / playing / muted / off / stopped; after Stop or end of file the lock indicators turn grey and the ▶ marker is cleared, the last counters stay visible), BCH ok/bad/corrected, superframe count and errors, input MB, ALSA xruns, dropped audio bytes, file progress. For HackRF and SDR recordings a second line shows frequency, gains, input level, carrier and clock offset, EVM, USB drops and acquisition retries (see [HackRF reception](#hackrf-reception)) |
| **Status bar** | Events such as resync, auto stereo/mono detection, end of file and errors |

### Keyboard shortcuts

The channel keys follow the CLI. Volume and mute keys also work while stopped. All keys are ignored while a text field has focus.

| Key | Action |
|-----|--------|
| `u` / `d` | Channel up / down (stereo pair or mono channel) |
| `s` / `m` | Force stereo / mono |
| `r` | Hard resync |
| `v` | Cycle decoder log level |
| `+` (or `=`) / `-` | Volume up / down (5 % steps); keypad `+` / `-` also work |
| `x` | Mute / unmute |
| `Ctrl+Q` | Quit |

The CLI's `t` (channel table) and `h` (help) keys have no GUI equivalent, since the channel list and tooltips are always visible.

### Command-line options

All options only pre-fill the window. Decoding starts when you press **Start**, or immediately with `--start`.

Usage: `./dsrdec-gtk [infile] [options]`

```
  infile / --in PATH  Raw A/B bitstream file
  --iqfile PATH       IQ file (built-in demodulator)
  --iqrate FS         IQ sample rate (default 20480000)
  --iqfmt s16le|f32le|s8  IQ sample format
  --track             IQ file is an SDR recording (timing/carrier recovery)
  --hackrf            HackRF One as source
  --freq MHZ          HackRF frequency (default 118.0)
  --lna DB --vga DB   HackRF gains (sets manual gain)
  --amp               HackRF RF amplifier on
  --host H --port P   UDP source
  --lsb               LSB-first bit order (raw/UDP)
  --repeat            Loop file input
  --wav FILE.wav      Record WAV
  --mono N | --stereo L,R  Initial channel
  --no-audioout       Disable live audio
  --start             Start decoding immediately
```

Examples:
```bash
./dsrdec-gtk --iqfile signal.iq --stereo 4,5 --start     # IQ file, start on pair 4/5
./dsrdec-gtk --port 9000 --start                         # live UDP stream
./dsrdec-gtk --hackrf --freq 118 --start                 # live RF with a HackRF One
./dsrdec-gtk signal.raw --wav out.wav --no-audioout      # raw file, record only
```

### Behaviour notes

- **Settings** (input, options, volume, mute, last channel) are saved to `~/.config/dsrdec-gtk.ini` on Start and on exit, and restored on the next launch. Command-line options override the saved values.
- **Quit signals**: Ctrl+C in the terminal, SIGTERM (as sent by `./DSRDemo stop`) and SIGHUP close the window the normal way, so decoding is stopped and the settings are saved.
- **Audio warm-up**: like the CLI, audio stays muted for at least 2 s after start or resync, and until Williard, deinterleave, SA cycle and SA offset are all locked.
- **Station list** (names, programme type, music/speech, mode): only *confirmed* values are shown, i.e. values received identically several times (a name 2× among the last 4 receptions, attributes 4× among the last 7). The SA data carrying them has no error protection, so single receptions are not trusted. Once confirmed, the list is kept across resyncs, lost locks and acquisition retries; a value only changes when a different one is received more often (a name 3× among the last 6) than the confirmed one, as after a real change in the mux. The **Station list** indicator is green while new data is accepted, orange *(frozen)* while the list is held after a sync loss until the SA alignment is found again; the counters show confirmed names and SA realigns. The CLI prints the same as a `[pa]` line under each `[stat]` line.
- **Auto stereo/mono**: after the first lock, the mode is chosen from the PA data for the current pair, unless you already picked a channel or mode yourself.
- **UDP jitter buffer**: in UDP mode, playback starts only after 0.5 s of audio is buffered, and the buffer is rebuilt after an ALSA xrun. `dsrtx` paces the mux to real time on its own clock and buffers each internet stream for 1 s, so a late stream only gets a short gap of silence in its own channel; the cushion here absorbs network jitter and the drift between the encoder clock and the sound card. This adds about 0.5 s latency, also after a channel change.
- **HackRF audio buffer**: with the HackRF, playback starts after 0.2 s of buffered audio (the HackRF delivers evenly, so less than for UDP is needed). The cushion also absorbs the slow drift between the HackRF and sound card clocks; if it runs out, an xrun rebuilds it.
- **Output rate** is always 32000 × upsample and is not editable in the GUI (an old `rate` value in the settings file is ignored). Any other rate would make audio run fast or slow and cause regular xruns in UDP mode.
- **Real-time pacing**: file input is throttled to real time when live audio is on or *Real-time pacing* is checked. Uncheck both to decode a file as fast as possible, e.g. for WAV conversion.
- **Volume and mute** only affect live ALSA audio. The slider works on a squared curve (50 % ≈ −12 dB) for a more even loudness scale. Both can be set before pressing Start and are applied when decoding starts.
- **WAV recording** from the GUI is always 16-bit stereo, at the original level (independent of volume and mute); in mono mode both channels carry the same signal.
- **Decoder logs** (log level) go to stderr of the terminal the GUI was started from.

## Architecture

- **decodermain.c** - Command-line interface, input mode dispatch (IQ/file/UDP/HackRF), keyboard control, stats reporting
- **demod.c/h** - Differential QPSK (4-PSK) demodulation: AGC, RRC matched filter and automatic symbol mapping. Two paths: a fixed 2-samples-per-symbol path with strobe-offset search (encoder IQ files, bit-exact as before), and a tracking path for live RF and SDR recordings with DC blocker, Gardner timing recovery (cubic interpolation, any rate near 2 × symbol rate) and a QPSK carrier loop (`--track`, `--hackrf`)
- **hackrf_rx.c/h** - HackRF One RX: device setup, ring buffer between the libusb callback and the decode thread, overflow counter, auto gain. Builds as a stub without libhackrf
- **decoder.c/h** - Core DSR decoder (A/B stream FSM, PRBS descrambler, deinterleaver, BCH checker, audio reconstruction)
- **pa.c/h** - PA/SK frame decoder: SA block alignment from the blank LB block (re-checked every cycle, redone after a sync loss), and a station list that only takes over values confirmed by repeated reception
- **wave.c/h** - WAV file output
- **alsa.c/h** - ALSA audio output (direct PCM interface with non-blocking mode, error recovery, configurable buffers)
- **dsr_engine.c/h** - Decode engine for the GUI: same pipeline as the CLI (warm-up gate, DC blocker, slew limiter, file pacing, resync with cooldown, HackRF acquisition retry), with all state in one object so it can be started and stopped repeatedly. Runs a decode thread, a UDP ingress thread (UDP only) and an ALSA output thread. It takes commands and provides status snapshots thread-safely. Not used by the CLI
- **gtkmain.c** - GTK 3 frontend (`dsrdec-gtk`): window, channel list, controls and the periodic status refresh (every 200 ms)

```
dsrdec      = decodermain.c + decoder + demod + hackrf_rx + pa + wave + alsa
dsrdec-gtk  = gtkmain.c + dsr_engine.c + decoder + demod + hackrf_rx + pa + wave + alsa
```

See [architecture.puml](architecture.puml) for two PlantUML diagrams: modules, input modes and data flow, and the threads and buffers of `dsrdec` and `dsrdec-gtk`.


## Bundled encoder

[dsr_encoder/](dsr_encoder/) contains `dsrtx`, a fork of Philip Heron's DSR encoder ([codeberg.org/fsphil/dsr](https://codeberg.org/fsphil/dsr)) that adds the raw (unmodulated) bitstream output over UDP/TCP and extra audio sources. It is GPL-licensed, see [dsr_encoder/COPYING](dsr_encoder/COPYING) and [dsr_encoder/README](dsr_encoder/README).

- Build: `make encoder` from the top level (needs `libhackrf-dev` and the ffmpeg dev libraries; without ffmpeg the internet stream channels don't work)
- Run on its own: `cd dsr_encoder/src && ./dsrtx -c lifeorg.conf`
- Configs: `lifeorg.conf` (German public radio), `hq.conf` (lossless FLAC streams), `eupaket.conf` (European stations), `example.conf` (all options), `dsrtest.conf` (writes an IQ file instead of UDP)
- Unit tests: `make encoder-test` (legacy) or `make encoder-test-ceedling`

## Legal notice

This software is provided for research, education and the preservation of a historic broadcast standard, without any warranty (see [LICENSE](LICENSE)). You are solely responsible for complying with the laws that apply to you:

- **Radio transmission**: the bundled encoder can drive SDR transmitters (HackRF, SoapySDR). Emitting RF without a licence or frequency assignment is prohibited in most countries; use cable connections, attenuators or dummy loads only (see the warning under [HackRF reception](#hackrf-reception)).
- **Third-party audio**: the example encoder configs reference public internet radio streams. They are third-party content, protected by copyright, and are meant for local, private testing only. Do not redistribute or retransmit them.

## Related Projects

- Standalone repository of the forked encoder: https://github.com/janosch79/dsr_enc_rawopt

## License

Copyright (C) 2025-2026 janosch79

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See [LICENSE](LICENSE).

The decoder reuses lookup tables and framing details from the GPL-licensed DSR encoder by Philip Heron, so the whole repository, including the bundled [dsr_encoder/](dsr_encoder/), is distributed under GPL-3.0-or-later.
