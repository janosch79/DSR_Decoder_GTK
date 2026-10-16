# DSR_Decoder
von janosch79 2025 · **Version 0.9.6** · [English version](README.md)

C-Programm zum Dekodieren von DSR-Signalen (Digitales Satellitenradio) aus I/Q-Sample-Dateien, Raw-Bitstrom-Dateien oder einem Live-UDP-Stream – mit eingebauter IQ-Demodulation und Audioausgabe über ALSA oder als WAV. Es gibt ein Kommandozeilenprogramm (`dsrdec`) und optional eine GTK-3-Oberfläche (`dsrdec-gtk`).

Ein passender DSR-Encoder (`dsrtx`) liegt in [dsr_encoder/](dsr_encoder/) bei. Damit läuft eine komplette Kette Sender → Empfänger auf einem Rechner.

## Schnellstart

```bash
git clone https://github.com/janosch79/DSR_Decoder.git
cd DSR_Decoder

# Build-Abhängigkeiten (Debian/Ubuntu)
sudo apt install build-essential pkg-config libasound2-dev libgtk-3-dev \
     libhackrf-dev libavcodec-dev libavformat-dev libavdevice-dev libswresample-dev libavutil-dev

# Bauen und starten (beim ersten Mal werden Encoder und Decoder kompiliert, das dauert ein paar Sekunden)
./DSRDemo start
```

Unter WSL2 (Windows) ist außerdem einmalig die Brücke ALSA → PulseAudio einzurichten, sonst gibt es keinen Ton: siehe [Audio unter WSL2](#audio-unter-wsl2).

Einen gesonderten Build-Schritt gibt es nicht: `./DSRDemo start` ruft selbst `make` auf und baut, was fehlt oder sich geändert hat (von Hand bauen: siehe [Kompilieren](#kompilieren)). Fehlt eine Bibliothek, nennt es das passende `apt install`.

`DSRDemo` startet die ganze Kette: Der Encoder holt 16 Internetradio-Streams aus seiner Konfiguration, packt sie in einen DSR-Multiplex und schickt den Raw-Bitstrom per UDP an den Decoder.

| Befehl | Wirkung |
|--------|---------|
| `./DSRDemo start [Optionen]` | Baut Encoder und Decoder (nur Geändertes), startet den Encoder im Hintergrund, wartet, bis er sendet (er öffnet seine Streams nacheinander, meist 5–10 s), und startet dann den Decoder |
| `./DSRDemo stop` | Beendet erst den Decoder sauber (die GUI speichert ihre Einstellungen), dann den Encoder. Funktioniert aus jedem Terminal |
| `./DSRDemo restart [Optionen]` | `stop` + `start`; ohne Optionen werden die des letzten Starts verwendet |
| `./DSRDemo status` | Zeigt, ob Encoder und Decoder laufen, sowie Konfiguration und UDP-Ziel |

Der GUI-Decoder läuft losgelöst, das Terminal ist also gleich wieder frei; wird sein Fenster geschlossen, endet auch der Encoder. Der CLI-Decoder läuft im Terminal (Tasten `u`/`d`, `t`, `h`, `q`); wird er beendet oder Strg+C gedrückt, stoppt auch der Encoder.

| Option | Wirkung |
|--------|---------|
| `-c DATEI` | Encoder-Konfiguration (Standard [dsr_encoder/src/lifeorg.conf](dsr_encoder/src/lifeorg.conf); außerdem `hq.conf`, `eupaket.conf`) |
| `--gui` / `--cli` | GTK-GUI oder Kommandozeilen-Decoder (Standard: GUI, wenn ein Display und GTK vorhanden sind) |
| `--stereo L,R` / `--mono N` | Startkanal |
| `--wait SEK` | Maximale Wartezeit, bis der Encoder sendet (Standard 60) |
| `--rebuild` / `--no-build` | Alles neu bauen / Bauen überspringen |
| `-- ...` | Weitere Optionen an den Decoder durchreichen |

Hinweise:
- Der UDP-Port wird aus dem Abschnitt `[output]` der Konfiguration gelesen (`udp://127.0.0.1:5000` in den mitgelieferten Konfigurationen). Ist der Port schon belegt, etwa von einem noch laufenden Decoder, bricht `start` mit einer Meldung ab.
- PIDs und Logs liegen in `run/` (`run/encoder.log`, `run/decoder.log` für die GUI).
- Die mitgelieferten Konfigurationen verwenden Internetradio-Streams, der Encoder braucht also Netzwerkzugang.

## Kompilieren

| Befehl | Baut |
|--------|------|
| `make` | Kommandozeilen-Decoder `dsrdec` (Standard) |
| `make gtk` | GTK-GUI `dsrdec-gtk` (optional) |
| `make encoder` | Mitgelieferten Encoder `dsr_encoder/src/dsrtx` (optional) |
| `make encoder-test` | Baut und startet den alten Encoder-Test `test_dsr` |
| `make encoder-test-ceedling` | Startet die Ceedling-Unit-Tests des Encoders in `dsr_encoder/test` (braucht `gem install ceedling`) |
| `make clean` | Löscht alle Programme und Objektdateien (Decoder und Encoder) |
| `make encoder-clean` | Löscht nur den Encoder-Build |

Es gibt ein einziges Makefile für alles, im Hauptverzeichnis; die Objektdateien des Encoders liegen neben seinen Quellen in `dsr_encoder/src`.

Alles neu bauen: `make clean && make && make gtk`

### Abhängigkeiten
- ALSA-Entwicklungsbibliotheken (libasound2-dev unter Debian/Ubuntu)
- Standard-C-Bibliothek mit POSIX-Unterstützung
- Mathe-Bibliothek (-lm)
- Nur für die GUI: GTK-3-Entwicklungsbibliotheken (libgtk-3-dev unter Debian/Ubuntu)
- Optional, für Live-Empfang mit einem HackRF One: libhackrf (libhackrf-dev)

Debian/Ubuntu:
```bash
sudo apt install build-essential pkg-config libasound2-dev   # CLI
sudo apt install libgtk-3-dev                                # zusätzlich für die GUI
sudo apt install libhackrf-dev                               # optional: HackRF-Empfang
```

Das Build-System findet ALSA über pkg-config und weicht notfalls auf `-lasound` aus. GTK wird über `pkg-config gtk+-3.0` erkannt. Fehlt GTK, bricht `make gtk` mit einem Hinweis ab; der CLI-Build braucht GTK gar nicht. libhackrf wird über `pkg-config libhackrf` erkannt; ohne die Bibliothek lassen sich beide Programme trotzdem bauen, die HackRF-Quelle meldet dann, dass sie nicht eingebaut ist.

### Audio unter WSL2

WSL2 hat keine eigene Soundkarte. WSLg stellt einen PulseAudio-Server bereit (`PULSE_SERVER=unix:/mnt/wslg/PulseServer`), beide Decoder spielen aber über ALSA, und Ubuntu unter WSL installiert die Brücke von ALSA zu PulseAudio nicht mit. Ohne sie lässt sich die Live-Audioausgabe nicht öffnen oder bleibt stumm. Einmalig einrichten:

```bash
sudo apt install libasound2-plugins alsa-utils    # ALSA-Pulse-Plugin + Testwerkzeuge

sudo tee /etc/asound.conf >/dev/null <<'EOF'
pcm.!default {
    type pulse
    hint.description "Default Audio Device"
}

ctl.!default {
    type pulse
}
EOF

aplay /usr/share/sounds/alsa/Front_Center.wav    # Test: „Front Center“ muss zu hören sein
```

Spielt der Ton eine Weile und bricht dann ab, oder meldet `aplay` `PulseAudio: Unable to create stream: Timeout`, hängt der Audioserver von WSLg selbst (das passiert auch ohne den Decoder). Ein Neustart von WSL behebt das meist: in PowerShell `wsl --shutdown` ausführen und dann das WSL-Terminal neu öffnen.

Das Makefile kompiliert mit `-O3 -march=native`, die Programme sind also auf die CPU des Build-Rechners optimiert und laufen auf anderen (älteren) CPUs eventuell nicht. Für ein portables Programm auf dem Zielrechner bauen oder `-march=native` aus `CFLAGS` entfernen.

## Programme

Das Projekt besteht aus zwei unabhängigen Programmen mit demselben Decoder-Kern:

- **dsrdec**: Kommandozeilen-Decoder mit Tastatursteuerung (siehe [Aufruf](#aufruf))
- **dsrdec-gtk**: optionale GTK-3-GUI (siehe [GTK-GUI](#gtk-gui-optional))

Beide kennen vier Eingabearten:

1. **IQ-Datei** (`--iqfile`) – demoduliert eine I/Q-Sample-Datei intern (kein separates Werkzeug, keine Pipe nötig) und dekodiert sie
2. **Raw-Datei** (`--in` / Datei als Argument / `-` für stdin) – dekodiert einen bereits demodulierten Raw-A/B-Bitstrom
3. **UDP** (`--host`/`--port`) – empfängt einen Raw-Bitstrom live per UDP (Standard, wenn keine Datei angegeben ist)
4. **HackRF** (`--hackrf`) – empfängt einen DSR-Mux live über Antenne oder Kabel mit einem HackRF One (Standard 118 MHz) und demoduliert ihn intern, siehe [HackRF-Empfang](#hackrf-empfang)

## Aufruf

Die Optionen und Tasten in diesem Abschnitt gelten für das Kommandozeilenprogramm `dsrdec`. Zur GUI siehe [GTK-GUI](#gtk-gui-optional).

Aufruf: `./dsrdec [infile|-] [Optionen]`

```
  infile              Raw-Bytedatei mit verschränktem A/B-Bitstrom; '-' für stdin
  --in PATH           Wie infile als Argument
  --host H            Empfangsadresse (Standard 0.0.0.0)  [ignoriert bei infile/iqfile]
  --port P            UDP-Port (Standard 9000)            [ignoriert bei infile/iqfile]
  --iqfile PATH       Raw-IQ-Datei (s16le|f32le|s8); wird intern demoduliert, kein
                      separates dsrdemod/keine Pipe nötig. Hat Vorrang vor infile/UDP.
  --iqrate FS         IQ-Abtastrate für --iqfile/--hackrf (Standard 20480000)
  --iqfmt {s16le|f32le|s8}  IQ-Sampleformat für --iqfile (Standard s16le;
                      s8 = Aufnahmen von hackrf_transfer)
  --track             Symboltakt- und Trägerrückgewinnung für --iqfile (SDR-Aufnahmen;
                      für IQ-Dateien des Encoders nicht nötig)
  --hackrf            Live-Empfang mit einem HackRF One (eingebauter Demodulator)
  --freq MHZ          HackRF-Mittenfrequenz in MHz (Standard 118.0)
  --lna DB            HackRF-LNA-Verstärkung 0..40, Schritt 8 (Standard 16; schaltet Auto-Gain ab)
  --vga DB            HackRF-VGA-Verstärkung 0..62, Schritt 2 (Standard 20; schaltet Auto-Gain ab)
  --amp               HackRF-HF-Verstärker an (+14 dB)
  --no-auto-gain      LNA/VGA fest lassen (Standard: an den Eingangspegel angepasst)
  --bw HZ             HackRF-Basisbandfilter (Standard: aus der Abtastrate)
  --serial S          HackRF-Seriennummer (Standard: erstes Gerät)
  --lsb               Bits im Byte LSB-first (Standard MSB-first) [nur infile/UDP]
  --repeat            Bei Dateiende (Dateieingabe) von vorn beginnen
  --no-descramble     PRBS-Entwürfelung der Nutzdaten abschalten
  --bch-check         BCH(63,44)-Prüfung einschalten (ohne Korrektur)
  --no-auto-deint     BCH-gesteuertes automatisches Deinterleaving aus (EVEN->blk0)
  --log-level LVL     Decoder-Log: off|sync|warn|zi (oder 0..3; Standard off)
  --verbose           Kurzform für --log-level zi
  --wav FILE.wav      WAV schreiben (16 Bit PCM, immer stereo; mono ergibt L=R)
  --rate SR           Ausgabe-Abtastrate (Standard 32000); muss 32000 * upsample sein
  --mono N            Mono aus Unterkanal N (0..31)
  --stereo L,R        Stereo aus den Unterkanälen L,R (0..31)
  --upsample N        Jedes dekodierte 32-kHz-Sample N-mal wiederholen (Standard 1);
                      --rate entsprechend auf 32000*N setzen
  --stream-focus      Nur das gewählte Paar dekodieren (Standard)
  --no-stream-focus   Ganzen Mux dekodieren (alle Paare); Standard ist stream-focus (weniger CPU)
  --audioout          Live-Audioausgabe über ALSA einschalten
  -h, --help          Hilfe anzeigen
```

Hinweise:
- Ohne `--track` arbeitet der Demodulator mit festen 2 Samples pro Symbol, `--iqrate` muss also die doppelte Symbolrate sein (Standard des Encoders: 20480000 für 10,24 MSym/s). Mit `--track` (bei `--hackrf` immer an) geht jede Rate in der Nähe, z. B. 20000000.
- Der Decoder gibt 32-kHz-Audio aus. `--rate` und `--upsample` sind in der CLI nicht gekoppelt: Mit `--upsample N` ist `--rate` von Hand auf 32000 × N zu setzen, sonst läuft das Audio zu langsam (die GUI macht das automatisch).

**Tasten (zur Laufzeit, interaktiv):**
| Taste | Aktion |
|-------|--------|
| `u` / `d` | Kanal hoch / runter (Stereopaar oder Monokanal) |
| `m` | Mono erzwingen (aktueller Kanal) |
| `s` | Stereo erzwingen |
| `v` | Decoder-Loglevel durchschalten: off -> sync -> warn -> zi |
| `t` | Kanaltabelle anzeigen (alle 32 Unterkanäle) |
| `p` | PA-Debug an/aus (rohe SA-Bytes auf stderr) |
| `r` | Harter Resync (setzt den Zustand zurück; nächstes `r` gesperrt bis will+sa+offset eingerastet sind) |
| `h` | Tastenhilfe anzeigen |
| `q` | Beenden |

## Beispiele

**IQ-Datei → Live-Audio (ein Schritt, keine Pipe nötig):**
```bash
./dsrdec --iqfile signal.iq --iqrate 20480000 --iqfmt s16le --audioout --stereo 1,2
```

**UDP:** Auf dem lokalen UDP-Port 9000 einen Raw-Datenstrom empfangen (z. B. von einem kompatiblen DSR-Encoder) und sofort über ALSA abspielen:
```bash
./dsrdec --port 9000 --audioout --rate 32000 --repeat --stereo 4,5
```

(Hinweis: Der Sender ist der mitgelieferte Encoder in [dsr_encoder/](dsr_encoder/); `./DSRDemo start` startet beide Seiten gemeinsam. Die mitgelieferten Konfigurationen senden an Port 5000.)

**Datei:** Einen bereits demodulierten Raw-Bitstrom aus einer Datei dekodieren und über ALSA abspielen:
```bash
./dsrdec signal.raw --audioout --rate 32000 --repeat --stereo 1,2
```

**In eine WAV-Datei schreiben statt live abspielen:**
```bash
./dsrdec --iqfile signal.iq --wav out.wav --stereo 1,2
```

Die WAV-Datei ist immer 16 Bit stereo, damit sich der Modus zur Laufzeit umschalten lässt (`m`/`s`, automatische Erkennung); im Monomodus tragen beide Kanäle dasselbe Signal.

## HackRF-Empfang

Mit einem HackRF One ersetzen `dsrdec` und `dsrdec-gtk` einen DSR-Empfänger: Mux abstimmen, demodulieren, dekodieren, abspielen.

```bash
./dsrdec --hackrf --audioout                    # 118 MHz, Auto-Gain
./dsrdec --hackrf --freq 118 --stereo 4,5 --audioout
./dsrdec-gtk --hackrf --start                   # GUI, sofort starten
```

Auf 118 MHz wurde DSR in deutschen Kabelnetzen übertragen (Sonderkanal S2). Jede andere Frequenz lässt sich mit `--freq` einstellen (GUI: *Frequency [MHz]*).

Für den Empfang des mitgelieferten Encoders per Funk ist ein zweiter HackRF nötig (ein Board kann nicht gleichzeitig senden und empfangen); in dessen Konfiguration gehören in den Abschnitt `[output]` `type = hackrf` und `frequency = 118e6` (die Konfigurationen zeigen 855 MHz als Beispiel).

> [!WARNING]
> **Das HF-Signal des Encoders darf niemals über eine Antenne abgestrahlt werden.** 118 MHz liegt im Flugfunkband (118–137 MHz, Flugsicherung und Sprechfunk mit Piloten), und DSR belegt rund 14 MHz Bandbreite um diese Frequenz. Senden dort stört sicherheitsrelevante Funkdienste und ist ohne Frequenzzuteilung durch die zuständige Behörde (in Deutschland die Bundesnetzagentur) verboten. Dasselbe gilt für jede andere Frequenz, für die keine Zulassung vorliegt.
>
> Der sendende HackRF darf **nur per Kabel** mit dem Empfänger verbunden werden, über ein passendes Dämpfungsglied (z. B. 30–60 dB) oder mit einem Abschlusswiderstand, ohne Antenne und mit ausgeschaltetem TX-Verstärker. Der Empfang mit `--hackrf` ist davon nicht betroffen. Siehe [Rechtliche Hinweise](#rechtliche-hinweise).

**Was nach dem Start passiert**

1. Der HackRF liefert 8-Bit-I/Q mit 20,48 MS/s (2 Samples pro Symbol), Basisbandfilter 20 MHz.
2. Der Demodulator arbeitet im Tracking-Modus: DC-Blocker gegen die Mittenspitze des HackRF, Gardner-Symboltaktrückgewinnung und eine QPSK-Trägerregelschleife. Zusammen gleichen sie den Quarzfehler des HackRF aus (etwa ±20 ppm: einige kHz Trägerversatz bei 118 MHz und ein Abtasttakt, der sonst innerhalb von Millisekunden ein Symbol verrutschen würde). Getestet wurden Trägerversätze bis etwa ±25 kHz.
3. Nach etwa 13 ms legt der Demodulator seine Symbolzuordnung fest, und der Decoder sucht den Mux.
4. Rastet innerhalb von 3 s kein Mux ein (Williard **und** SA-Zyklus; Williard allein kann auf Rauschen einrasten), beginnt die Suche von vorn. Das deckt auch einen später auftauchenden Mux ab oder eine falsche Zuordnung, die auf einem Störimpuls entschieden wurde. Die CLI meldet `[hackrf] no DSR mux locked at … - retrying`; die GUI zeigt *Searching for a DSR mux at … MHz* und zählt die *searches*.
5. **Auto-Gain** (Standard) verstellt LNA/VGA alle 300 ms, bis der Eingangspegel zwischen −22 und −10 dBFS liegt (8-Bit-ADC weder übersteuert noch schlecht ausgenutzt). `--no-auto-gain`, `--lna`/`--vga` oder das Abwählen von *Auto gain* in der GUI halten die Verstärkung fest. Der HF-Verstärker bleibt aus, solange er nicht eingeschaltet wird (`--amp`), denn ein Kabelsignal ist meist kräftig.
6. Gehen auf USB Samples verloren (CPU oder USB zu langsam), synchronisiert der Decoder selbst neu, da eine Lücke im Bitstrom sonst verstümmelte Programmnamen hinterlassen würde.

**Empfängerstatus.** Die CLI gibt unter jeder `[stat]`-Zeile eine `[rf]`-Zeile aus; die GUI zeigt dieselben Werte unter den Decoder-Zählern:

```
[rf]   level -13.5 dBFS | CFO +3.00 kHz | clock +26.5 ppm | EVM 12 % | LNA 16 VGA 20 amp off | drops 0.0 MB
```

| Wert | Bedeutung |
|------|-----------|
| level | Eingangspegel vor der digitalen AGC (mittlerer Spitzenwert von I/Q). Auto-Gain hält ihn bei −22…−10 dBFS |
| CFO | Von der Trägerregelschleife verfolgter Trägerversatz (Frequenzfehler von HackRF + Sender) |
| clock | Abtasttaktversatz zwischen HackRF und Sender |
| EVM | Fehlervektorbetrag der Symbole. In Tests: 3–12 % bei sauberem Signal, fehlerfreie Dekodierung noch bei ~30 % (8 dB SNR); reines Rauschen ergibt ~50 % |
| drops | Auf USB verlorene Samples. Steigt der Wert, hilft `--iqrate 20000000` (GUI: *Rate 20.00 MS/s*), das spezifizierte Maximum des HackRF |

**Hinweise zur Einrichtung**

- Der HackRF muss ohne root erreichbar sein (udev-Regeln aus dem Paket `hackrf`). Prüfen mit `hackrf_info`.
- WSL2: WSL sieht standardmäßig keine USB-Geräte (`No HackRF found`). Abhilfe schafft das Durchreichen mit [usbipd-win](https://github.com/dorssel/usbipd-win): `usbipd list` zeigt seine Bus-ID (`1d50:6089 HackRF One`); einmalig `usbipd bind --busid <id>` aus einer Admin-Shell, dann `usbipd attach --wsl --busid <id>` (mit `--auto-attach` bleibt er auch nach dem Umstecken verbunden). Das Anhängen ist nach jedem Umstecken und jedem Neustart von Windows/WSL zu wiederholen; solange er angehängt ist, kann Windows selbst den HackRF nicht nutzen.
- CPU: Der Tracking-Demodulator dekodiert auf einem i7-11800H etwa 3× schneller als Echtzeit, auf einem Kern.

**SDR-Aufnahmen.** Eine mit `hackrf_transfer` erstellte Aufnahme wird genauso dekodiert wie der Live-Empfang:

```bash
hackrf_transfer -r dsr.cs8 -f 118000000 -s 20480000 -l 16 -g 20 -n 409600000   # 20 s
./dsrdec --iqfile dsr.cs8 --iqfmt s8 --track --audioout
```

`--track` (GUI: *SDR recording*) schaltet dieselbe Takt- und Trägerrückgewinnung und dieselbe Wiederholung der Suche ein. Vom mitgelieferten Encoder geschriebene IQ-Dateien brauchen das nicht; ohne `--track` werden sie genau wie bisher dekodiert.

## GTK-GUI (optional)

**dsrdec-gtk** ist eine grafische Oberfläche im Geist von `dablin_gtk` aus [dablin](https://github.com/Opendigitalradio/dablin). Sie verwendet dieselben Decoder-, Demodulator- und PA-Module wie die CLI. Das Kommandozeilenprogramm `dsrdec` bleibt unverändert und ist weiterhin eigenständig nutzbar.

Bauen und starten:
```bash
make gtk
./dsrdec-gtk
```

Unter WSL2 wird das Fenster über WSLg angezeigt; ein zusätzlicher X-Server ist nicht nötig. Für den Ton siehe [Audio unter WSL2](#audio-unter-wsl2).

### Fensteraufbau

Die Oberfläche selbst ist englisch beschriftet; die Begriffe in *kursiv* entsprechen den Beschriftungen im Fenster.

| Bereich | Inhalt |
|---------|--------|
| **Input** | Quelle (IQ-Datei / Raw-Bitstrom-Datei / UDP-Stream / HackRF One), Dateiauswahl, IQ-Rate und -Format, *SDR recording* (Takt- und Trägerrückgewinnung für IQ-Dateien), UDP-Adresse und -Port, HackRF-Frequenz, Rate, Auto-Gain, LNA/VGA und HF-Verstärker, Live-Audio (ALSA), Echtzeit-Takt, Datei wiederholen, LSB-first, WAV-Aufnahme. *Advanced decoder options*: PRBS-Entwürfelung, BCH-Prüfung, automatisches Deinterleaving, Stream-Fokus, Upsample (angezeigte Ausgaberate, immer 32000 × Upsample) |
| **Start / Stop** | Startet oder stoppt das Dekodieren; die Eingabeeinstellungen sind währenddessen gesperrt |
| **Now playing** | Sendername, Kanal/Kanäle, Stereo/Mono, Programmart, Musik/Sprache. Rechts: **Stummschalter** (Lautsprecher-Symbol) und **Lautstärkeregler** (0–100 %) für das Live-Audio |
| **Kanalliste** | Eine Zeile pro Programm mit Sendername, Modus, Inhalt (Musik/Sprache) und Programmart aus den PA-Daten. Ein Stereopaar erscheint als **eine Zeile** (z. B. `2L/2R · Stereo`); Monokanäle und Unterkanäle noch ohne PA-Daten bekommen eine eigene Zeile (z. B. `9L · Mono`). Die laufende Zeile ist mit ▶ markiert. **Doppelklick** auf eine Zeile stimmt ab (Stereozeile → Paar in Stereo, Monozeile → dieser Unterkanal in Mono). Mindestens 16 Zeilen (ein voller Stereo-Mux) sind immer sichtbar; die Standardhöhe des Fensters zeigt alle 32 Zeilen, wenn der Bildschirm es zulässt. Scrollposition und Auswahl bleiben erhalten, wenn sich die Liste ändert |
| **Controls** | Vorheriger / nächster Kanal, Stereo / Mono, Resync, Decoder-Loglevel |
| **Receiver status** | Lock-Anzeigen für Williard, Deinterleave, SA-Zyklus, SA-Offset und Senderliste (grün = eingerastet/bestätigt, orange = wartet oder eingefroren), Audiozustand (Warm-up / spielt / stumm / aus / gestoppt; nach Stop oder Dateiende werden die Lock-Anzeigen grau und die ▶-Markierung verschwindet, die letzten Zähler bleiben sichtbar), BCH ok/fehlerhaft/korrigiert, Superframe-Zähler und -Fehler, Eingang in MB, ALSA-Xruns, verworfene Audio-Bytes, Dateifortschritt. Bei HackRF und SDR-Aufnahmen zeigt eine zweite Zeile Frequenz, Verstärkung, Eingangspegel, Träger- und Taktversatz, EVM, USB-Verluste und Suchwiederholungen (siehe [HackRF-Empfang](#hackrf-empfang)) |
| **Statusleiste** | Ereignisse wie Resync, automatische Stereo/Mono-Erkennung, Dateiende und Fehler |

### Tastenkürzel

Die Kanaltasten entsprechen der CLI. Lautstärke- und Stummtasten funktionieren auch im gestoppten Zustand. Solange ein Textfeld den Fokus hat, werden alle Tasten ignoriert.

| Taste | Aktion |
|-------|--------|
| `u` / `d` | Kanal hoch / runter (Stereopaar oder Monokanal) |
| `s` / `m` | Stereo / Mono erzwingen |
| `r` | Harter Resync |
| `v` | Decoder-Loglevel durchschalten |
| `+` (oder `=`) / `-` | Lauter / leiser (Schritte zu 5 %); `+` / `-` auf dem Ziffernblock gehen auch |
| `x` | Stumm an / aus |
| `Strg+Q` | Beenden |

Die CLI-Tasten `t` (Kanaltabelle) und `h` (Hilfe) haben in der GUI keine Entsprechung, da Kanalliste und Tooltips immer sichtbar sind.

### Kommandozeilenoptionen

Alle Optionen füllen nur das Fenster vor. Das Dekodieren beginnt mit einem Klick auf **Start** oder sofort mit `--start`.

Aufruf: `./dsrdec-gtk [infile] [Optionen]`

```
  infile / --in PATH  Raw-A/B-Bitstrom-Datei
  --iqfile PATH       IQ-Datei (eingebauter Demodulator)
  --iqrate FS         IQ-Abtastrate (Standard 20480000)
  --iqfmt s16le|f32le|s8  IQ-Sampleformat
  --track             IQ-Datei ist eine SDR-Aufnahme (Takt-/Trägerrückgewinnung)
  --hackrf            HackRF One als Quelle
  --freq MHZ          HackRF-Frequenz (Standard 118.0)
  --lna DB --vga DB   HackRF-Verstärkung (setzt manuelle Verstärkung)
  --amp               HackRF-HF-Verstärker an
  --host H --port P   UDP-Quelle
  --lsb               Bitreihenfolge LSB-first (Raw/UDP)
  --repeat            Dateieingabe wiederholen
  --wav FILE.wav      WAV aufnehmen
  --mono N | --stereo L,R  Startkanal
  --no-audioout       Live-Audio aus
  --start             Sofort mit dem Dekodieren beginnen
```

Beispiele:
```bash
./dsrdec-gtk --iqfile signal.iq --stereo 4,5 --start     # IQ-Datei, Start auf Paar 4/5
./dsrdec-gtk --port 9000 --start                         # Live-UDP-Stream
./dsrdec-gtk --hackrf --freq 118 --start                 # Live-HF mit einem HackRF One
./dsrdec-gtk signal.raw --wav out.wav --no-audioout      # Raw-Datei, nur aufnehmen
```

### Verhalten im Detail

- **Einstellungen** (Eingabe, Optionen, Lautstärke, Stummschaltung, letzter Kanal) werden beim Start und beim Beenden in `~/.config/dsrdec-gtk.ini` gespeichert und beim nächsten Start wiederhergestellt. Kommandozeilenoptionen haben Vorrang vor den gespeicherten Werten.
- **Beenden per Signal**: Strg+C im Terminal, SIGTERM (wie von `./DSRDemo stop` gesendet) und SIGHUP schließen das Fenster auf normalem Weg, das Dekodieren wird also beendet und die Einstellungen werden gespeichert.
- **Audio-Warm-up**: Wie in der CLI bleibt der Ton nach Start oder Resync mindestens 2 s stumm, und so lange, bis Williard, Deinterleave, SA-Zyklus und SA-Offset alle eingerastet sind.
- **Senderliste** (Namen, Programmart, Musik/Sprache, Modus): Angezeigt werden nur *bestätigte* Werte, also solche, die mehrfach identisch empfangen wurden (ein Name 2× unter den letzten 4 Empfängen, Attribute 4× unter den letzten 7). Die SA-Daten, die sie tragen, haben keinen Fehlerschutz, einzelnen Empfängen wird daher nicht vertraut. Einmal bestätigt, bleibt die Liste über Resyncs, Lock-Verluste und Suchwiederholungen erhalten; ein Wert ändert sich nur, wenn ein anderer häufiger empfangen wird (ein Name 3× unter den letzten 6) als der bestätigte, wie nach einer echten Änderung im Mux. Die Anzeige **Station list** ist grün, solange neue Daten angenommen werden, und orange *(frozen)*, solange die Liste nach einem Synchronverlust festgehalten wird, bis die SA-Ausrichtung wiedergefunden ist; die Zähler zeigen bestätigte Namen und SA-Neuausrichtungen. Die CLI gibt dasselbe als `[pa]`-Zeile unter jeder `[stat]`-Zeile aus.
- **Automatisch Stereo/Mono**: Nach dem ersten Einrasten wird der Modus anhand der PA-Daten des aktuellen Paars gewählt, sofern nicht bereits von Hand ein Kanal oder Modus gewählt wurde.
- **UDP-Jitterpuffer**: Im UDP-Modus beginnt die Wiedergabe erst, wenn 0,5 s Audio gepuffert sind, und der Puffer wird nach einem ALSA-Xrun neu aufgebaut. `dsrtx` sendet den Mux im Echtzeittakt nach seiner eigenen Uhr und puffert jeden Internetstream 1 s lang, sodass ein verspäteter Stream nur in seinem eigenen Kanal eine kurze Stille bekommt; der Puffer hier fängt Netzwerk-Jitter und die Drift zwischen Encoder-Uhr und Soundkarte ab. Das bringt etwa 0,5 s Latenz, auch nach einem Kanalwechsel.
- **HackRF-Audiopuffer**: Mit dem HackRF beginnt die Wiedergabe nach 0,2 s gepuffertem Audio (der HackRF liefert gleichmäßig, also braucht es weniger als bei UDP). Der Puffer fängt auch die langsame Drift zwischen den Takten von HackRF und Soundkarte ab; läuft er leer, baut ein Xrun ihn neu auf.
- **Ausgaberate** ist immer 32000 × Upsample und in der GUI nicht einstellbar (ein alter `rate`-Wert in der Einstellungsdatei wird ignoriert). Jede andere Rate ließe das Audio zu schnell oder zu langsam laufen und verursachte im UDP-Modus regelmäßige Xruns.
- **Echtzeit-Takt**: Dateieingaben werden auf Echtzeit gebremst, wenn Live-Audio an ist oder *Real-time pacing* angehakt ist. Ist beides abgewählt, wird eine Datei so schnell wie möglich dekodiert, z. B. für eine WAV-Umwandlung.
- **Lautstärke und Stummschaltung** wirken nur auf das Live-Audio über ALSA. Der Regler arbeitet mit einer quadratischen Kennlinie (50 % ≈ −12 dB) für eine gleichmäßigere Lautheitsskala. Beides lässt sich schon vor dem Start einstellen und wird beim Start übernommen.
- **WAV-Aufnahme** aus der GUI ist immer 16 Bit stereo, im Originalpegel (unabhängig von Lautstärke und Stummschaltung); im Monomodus tragen beide Kanäle dasselbe Signal.
- **Decoder-Logs** (Loglevel) gehen auf stderr des Terminals, aus dem die GUI gestartet wurde.

## Architektur

- **decodermain.c** – Kommandozeilenoberfläche, Verteilung auf die Eingabearten (IQ/Datei/UDP/HackRF), Tastatursteuerung, Statistikausgabe
- **demod.c/h** – Differenzielle QPSK-Demodulation (4-PSK): AGC, RRC-Matched-Filter und automatische Symbolzuordnung. Zwei Pfade: ein fester Pfad mit 2 Samples pro Symbol und Suche der Abtastphase (IQ-Dateien des Encoders, bitgenau wie bisher) und ein Tracking-Pfad für Live-HF und SDR-Aufnahmen mit DC-Blocker, Gardner-Taktrückgewinnung (kubische Interpolation, jede Rate nahe 2 × Symbolrate) und QPSK-Trägerregelschleife (`--track`, `--hackrf`)
- **hackrf_rx.c/h** – HackRF-One-Empfang: Geräteeinrichtung, Ringpuffer zwischen libusb-Callback und Dekodier-Thread, Überlaufzähler, Auto-Gain. Wird ohne libhackrf als Platzhalter gebaut
- **decoder.c/h** – DSR-Decoder-Kern (Zustandsautomat für den A/B-Strom, PRBS-Entwürfler, Deinterleaver, BCH-Prüfung, Audio-Rekonstruktion)
- **pa.c/h** – PA/SK-Rahmendecoder: SA-Blockausrichtung anhand des leeren LB-Blocks (in jedem Zyklus geprüft, nach einem Synchronverlust neu bestimmt) und eine Senderliste, die nur durch wiederholten Empfang bestätigte Werte übernimmt
- **wave.c/h** – WAV-Dateiausgabe
- **alsa.c/h** – ALSA-Audioausgabe (direkte PCM-Schnittstelle mit nicht blockierendem Modus, Fehlerbehandlung, einstellbaren Puffern)
- **dsr_engine.c/h** – Dekodier-Engine für die GUI: dieselbe Verarbeitungskette wie die CLI (Warm-up-Sperre, DC-Blocker, Flankenbegrenzer, Datei-Takt, Resync mit Sperrzeit, Suchwiederholung beim HackRF), mit dem gesamten Zustand in einem Objekt, damit sie wiederholt gestartet und gestoppt werden kann. Sie betreibt einen Dekodier-Thread, einen UDP-Empfangs-Thread (nur UDP) und einen ALSA-Ausgabe-Thread, nimmt Befehle threadsicher an und liefert Status-Schnappschüsse. Wird von der CLI nicht verwendet
- **gtkmain.c** – GTK-3-Oberfläche (`dsrdec-gtk`): Fenster, Kanalliste, Bedienelemente und die regelmäßige Statusaktualisierung (alle 200 ms)

```
dsrdec      = decodermain.c + decoder + demod + hackrf_rx + pa + wave + alsa
dsrdec-gtk  = gtkmain.c + dsr_engine.c + decoder + demod + hackrf_rx + pa + wave + alsa
```

[architecture.puml](architecture.puml) enthält zwei PlantUML-Diagramme: Module, Eingabearten und Datenfluss sowie die Threads und Puffer von `dsrdec` und `dsrdec-gtk`.


## Mitgelieferter Encoder

[dsr_encoder/](dsr_encoder/) enthält `dsrtx`, einen Fork von Philip Herons DSR-Encoder ([codeberg.org/fsphil/dsr](https://codeberg.org/fsphil/dsr)), der die rohe (unmodulierte) Bitstrom-Ausgabe über UDP/TCP und zusätzliche Audioquellen ergänzt. Er steht unter der GPL, siehe [dsr_encoder/COPYING](dsr_encoder/COPYING) und [dsr_encoder/README](dsr_encoder/README).

- Bauen: `make encoder` im Hauptverzeichnis (braucht `libhackrf-dev` und die ffmpeg-Entwicklungsbibliotheken; ohne ffmpeg funktionieren die Internetstream-Kanäle nicht)
- Eigenständig starten: `cd dsr_encoder/src && ./dsrtx -c lifeorg.conf`
- Konfigurationen: `lifeorg.conf` (deutsche öffentlich-rechtliche Sender), `hq.conf` (verlustfreie FLAC-Streams), `eupaket.conf` (europäische Sender), `example.conf` (alle Optionen), `dsrtest.conf` (schreibt eine IQ-Datei statt UDP)
- Unit-Tests: `make encoder-test` (alt) oder `make encoder-test-ceedling`

## Rechtliche Hinweise

Diese Software dient der Forschung, der Lehre und dem Erhalt eines historischen Rundfunkstandards und wird ohne jede Gewährleistung bereitgestellt (siehe [LICENSE](LICENSE)). Für die Einhaltung der jeweils geltenden Gesetze sind allein die Nutzenden verantwortlich:

- **Funkaussendungen**: Der mitgelieferte Encoder kann SDR-Sender ansteuern (HackRF, SoapySDR). HF-Aussendungen ohne Zulassung oder Frequenzzuteilung sind in den meisten Ländern verboten; zulässig sind nur Kabelverbindungen, Dämpfungsglieder oder Abschlusswiderstände (siehe die Warnung unter [HackRF-Empfang](#hackrf-empfang)).
- **Audio von Dritten**: Die Beispielkonfigurationen des Encoders verweisen auf öffentliche Internetradio-Streams. Das sind urheberrechtlich geschützte Inhalte Dritter, gedacht nur für lokale, private Tests. Eine Weiterverbreitung oder Weitersendung ist nicht gestattet.

## Verwandte Projekte

- Eigenständiges Repository des geforkten Encoders: https://github.com/janosch79/dsr_enc_rawopt

## Lizenz

Copyright (C) 2025-2026 janosch79

Dieses Programm ist freie Software: Es darf unter den Bedingungen der GNU General Public License, wie von der Free Software Foundation veröffentlicht, weitergegeben und/oder verändert werden, entweder gemäß Version 3 der Lizenz oder (nach eigener Wahl) jeder späteren Version. Siehe [LICENSE](LICENSE). Maßgeblich ist der englische Originaltext der Lizenz.

Der Decoder verwendet Tabellen und Details der Rahmenstruktur aus dem GPL-lizenzierten DSR-Encoder von Philip Heron; das gesamte Repository einschließlich des mitgelieferten [dsr_encoder/](dsr_encoder/) steht daher unter GPL-3.0-or-later.
