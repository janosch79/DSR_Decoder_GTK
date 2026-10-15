// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// gtkmain.c — dsrdec-gtk: GTK 3 frontend for the DSR decoder (in the spirit of dablin_gtk).
// Uses dsr_engine.c for the decode pipeline; the CLI (dsrdec / decodermain.c) is separate
// and unchanged.
//
// Build: make gtk   (needs libgtk-3-dev)

#define _POSIX_C_SOURCE 200809L

#include <gtk/gtk.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsr_engine.h"

#define APP_TITLE   "DSR Decoder"
#define APP_VER     "0.9.5p-gtk"
#define TICK_MS     200
#define CONFIG_FILE "dsrdec-gtk.ini"
#define LIST_MIN_ROWS 16          /* channel list rows always visible */

enum {
    COL_MARK = 0,   /* "▶" on the row currently playing */
    COL_CH,         /* hidden: subchannel index 0..31 (pair base for stereo rows) */
    COL_CH_NAME,    /* shown: 1L, 1R, ... 16R; stereo pair "1L/1R" */
    COL_LABEL,
    COL_MODE,
    COL_MS,
    COL_GENRE,
    COL_WEIGHT,
    COL_STEREO,     /* hidden: row is a stereo pair (both subchannels) */
    COL_KNOWN,      /* hidden: PA info available */
    N_COLS
};

/* One channel-list row: a stereo pair (ch = even base) or a single subchannel. */
typedef struct {
    int  ch;
    bool stereo;
} row_t;

typedef struct {
    engine_t *eng;
    engine_status_t st;
    bool st_valid;
    bool updating;          /* suppress widget callbacks while syncing from status */
    row_t rows[ENGINE_NUM_CH];  /* current list layout */
    int   n_rows;               /* -1 = force rebuild */
    char  row_cache[ENGINE_NUM_CH][160];

    GtkWidget *win;
    /* source */
    GtkWidget *src_combo, *file_entry, *file_btn, *file_lbl;
    GtkWidget *iq_box, *iqrate_spin, *iqfmt_combo, *track_check;
    GtkWidget *rf_box, *freq_spin, *rfrate_combo, *lna_spin, *vga_spin, *amp_check, *agc_check;
    GtkWidget *udp_box, *host_entry, *port_spin;
    GtkWidget *lsb_check, *repeat_check, *audio_check, *realtime_check;
    GtkWidget *wav_check, *wav_entry, *wav_btn;
    /* advanced */
    GtkWidget *descr_check, *bch_check, *deint_check, *focus_check, *rate_label, *ups_spin;
    GtkWidget *start_btn;
    GtkWidget *src_frame;
    /* now playing */
    GtkWidget *np_title, *np_sub;
    GtkWidget *mute_btn, *mute_img, *vol_scale;
    /* channel list */
    GtkListStore *store;
    GtkWidget *tree;
    /* controls */
    GtkWidget *prev_btn, *next_btn, *stereo_radio, *mono_radio, *resync_btn, *log_combo;
    GtkWidget *ctrl_box;
    /* status */
    GtkWidget *ind_will, *ind_deint, *ind_sa, *ind_off, *ind_list, *ind_audio;
    GtkWidget *stats_lbl, *rf_lbl, *progress;
    GtkWidget *statusbar;
    guint sb_ctx;
} app_t;

static app_t A;

/* ------------------------------------------------------------------ helpers */

static void status_msg(const char *msg)
{
    gtk_statusbar_pop(GTK_STATUSBAR(A.statusbar), A.sb_ctx);
    gtk_statusbar_push(GTK_STATUSBAR(A.statusbar), A.sb_ctx, msg);
}

static void set_indicator(GtkWidget *lbl, const char *name, int state /* 0 off, 1 ok, 2 wait */)
{
    static const char *colors[] = { "#9e9e9e", "#2e7d32", "#ef6c00" };
    char *m = g_markup_printf_escaped("<span foreground=\"%s\">●</span> %s", colors[state], name);
    gtk_label_set_markup(GTK_LABEL(lbl), m);
    g_free(m);
}

static engine_src_t cur_src(void)
{
    return (engine_src_t)gtk_combo_box_get_active(GTK_COMBO_BOX(A.src_combo));
}

/* List layout from PA: a pair is stereo when either subchannel reports mode 2 (same rule
 * as the CLI channel table 't') and then gets a single row; all other subchannels (mono or
 * no PA yet) get one row each. s == NULL: no PA, 32 single rows. */
static int build_layout(const engine_status_t *s, row_t rows[ENGINE_NUM_CH])
{
    int n = 0;
    for (int b = 0; b < ENGINE_NUM_CH; b += 2) {
        bool stereo = s && (s->ch[b].mode == 2 || s->ch[b + 1].mode == 2);
        if (stereo) {
            rows[n++] = (row_t){ b, true };
        } else {
            rows[n++] = (row_t){ b, false };
            rows[n++] = (row_t){ b + 1, false };
        }
    }
    return n;
}

static bool row_eq(const row_t *a, const row_t *b)
{
    return a->ch == b->ch && a->stereo == b->stereo;
}

/* Apply a new list layout in place (changed rows are blank until filled). Rows are
 * overwritten / appended / removed at the end rather than clearing the store, so the
 * view keeps its scroll position and cursor. Compared per field: row_t has padding
 * bytes, and a memcmp over them made every tick look like a layout change. */
static void set_rows(const row_t *rows, int n)
{
    GtkTreeModel *m = GTK_TREE_MODEL(A.store);
    GtkTreeIter it;
    gboolean have = gtk_tree_model_get_iter_first(m, &it);
    for (int i = 0; i < n; i++) {
        if (!have) gtk_list_store_append(A.store, &it);
        if (!have || i >= A.n_rows || !row_eq(&rows[i], &A.rows[i])) {
            char nb[16], l[4], r[4];
            if (rows[i].stereo)
                snprintf(nb, sizeof(nb), "%s/%s", engine_ch_name(rows[i].ch, l), engine_ch_name(rows[i].ch + 1, r));
            else
                snprintf(nb, sizeof(nb), "%s", engine_ch_name(rows[i].ch, l));
            gtk_list_store_set(A.store, &it, COL_MARK, "", COL_CH, rows[i].ch, COL_CH_NAME, nb,
                               COL_LABEL, "", COL_MODE, "", COL_MS, "", COL_GENRE, "",
                               COL_WEIGHT, PANGO_WEIGHT_NORMAL,
                               COL_STEREO, rows[i].stereo, COL_KNOWN, rows[i].stereo, -1);
            A.rows[i] = rows[i];
            A.row_cache[i][0] = '\0';
        }
        have = gtk_tree_model_iter_next(m, &it);
    }
    while (have) have = gtk_list_store_remove(A.store, &it);
    A.n_rows = n;
}

static void reset_rows(void)
{
    row_t rows[ENGINE_NUM_CH];
    A.n_rows = -1;
    set_rows(rows, build_layout(NULL, rows));
}

/* Slider 0..100 % -> amplitude gain; squared for a roughly even loudness scale. */
static float vol_gain(void)
{
    double v = gtk_range_get_value(GTK_RANGE(A.vol_scale)) / 100.0;
    return (float)(v * v);
}

static void update_mute_icon(void)
{
    double v = gtk_range_get_value(GTK_RANGE(A.vol_scale));
    bool muted = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.mute_btn));
    const char *icon = (muted || v <= 0.0) ? "audio-volume-muted-symbolic"
                     : v < 34.0 ? "audio-volume-low-symbolic"
                     : v < 67.0 ? "audio-volume-medium-symbolic" : "audio-volume-high-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(A.mute_img), icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(A.mute_btn, muted ? "Unmute live audio" : "Mute live audio");
}

static char *config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), CONFIG_FILE, NULL);
}

/* ------------------------------------------------------------------ source UI */

static void update_source_widgets(void)
{
    engine_src_t s = cur_src();
    bool running = A.eng != NULL;
    bool file = s == ENGINE_SRC_IQ || s == ENGINE_SRC_RAW;
    gtk_widget_set_visible(A.iq_box, s == ENGINE_SRC_IQ);
    gtk_widget_set_visible(A.udp_box, s == ENGINE_SRC_UDP);
    gtk_widget_set_visible(A.rf_box, s == ENGINE_SRC_HACKRF);
    gtk_widget_set_visible(A.file_lbl, file);
    gtk_widget_set_visible(A.file_entry, file);
    gtk_widget_set_visible(A.file_btn, file);
    gtk_widget_set_sensitive(A.lsb_check, s == ENGINE_SRC_RAW || s == ENGINE_SRC_UDP);
    gtk_widget_set_sensitive(A.repeat_check, file);
    gtk_widget_set_sensitive(A.realtime_check, file);
    bool agc = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.agc_check));
    gtk_widget_set_sensitive(A.lna_spin, !agc);
    gtk_widget_set_sensitive(A.vga_spin, !agc);
    bool wav = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.wav_check));
    gtk_widget_set_sensitive(A.wav_entry, wav);
    gtk_widget_set_sensitive(A.wav_btn, wav);

    gtk_widget_set_sensitive(A.src_frame, !running);
    gtk_widget_set_sensitive(A.ctrl_box, running);
    gtk_button_set_label(GTK_BUTTON(A.start_btn), running ? "■  Stop" : "▶  Start");
}

static void on_src_changed(GtkComboBox *c, gpointer u) { (void)c; (void)u; update_source_widgets(); }
static void on_wav_toggled(GtkToggleButton *b, gpointer u) { (void)b; (void)u; update_source_widgets(); }
static void on_agc_toggled(GtkToggleButton *b, gpointer u) { (void)b; (void)u; update_source_widgets(); }

/* HackRF sample rates offered: the encoder's 20.48 MS/s (exactly 2 samples/symbol) and
 * the HackRF's specified maximum 20 MS/s for hosts that drop samples at 20.48. */
static const unsigned rf_rates[] = { 20480000u, 20000000u };

static void on_browse_input(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        cur_src() == ENGINE_SRC_IQ ? "Open IQ file" : "Open raw bitstream file",
        GTK_WINDOW(A.win), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    const char *cur = gtk_entry_get_text(GTK_ENTRY(A.file_entry));
    if (cur[0]) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), cur);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, "IQ / raw (*.iq, *.raw, *.bin)");
    gtk_file_filter_add_pattern(f, "*.iq");
    gtk_file_filter_add_pattern(f, "*.raw");
    gtk_file_filter_add_pattern(f, "*.bin");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), all);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(A.file_entry), fn);
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

static void on_browse_wav(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Record to WAV file", GTK_WINDOW(A.win),
        GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    const char *cur = gtk_entry_get_text(GTK_ENTRY(A.wav_entry));
    if (cur[0]) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), cur);
    else        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "dsr.wav");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(GTK_ENTRY(A.wav_entry), fn);
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
}

/* The DSR mux carries 32 kHz audio; the output rate is not a free choice. Any other
 * value only plays too fast/slow and, on live UDP input, drains ALSA into xruns. */
#define DSR_OUT_RATE 32000

static void on_upsample_changed(GtkSpinButton *sb, gpointer user)
{
    (void)user;
    char t[64];
    snprintf(t, sizeof(t), "→ output rate %d Hz", DSR_OUT_RATE * gtk_spin_button_get_value_as_int(sb));
    gtk_label_set_text(GTK_LABEL(A.rate_label), t);
}

/* ------------------------------------------------------------------ settings */

static void settings_save(void)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "source", "type", (int)cur_src());
    g_key_file_set_string(kf, "source", "file", gtk_entry_get_text(GTK_ENTRY(A.file_entry)));
    g_key_file_set_integer(kf, "source", "iqrate", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.iqrate_spin)));
    g_key_file_set_integer(kf, "source", "iqfmt", gtk_combo_box_get_active(GTK_COMBO_BOX(A.iqfmt_combo)));
    g_key_file_set_boolean(kf, "source", "iq_track", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.track_check)));
    g_key_file_set_double(kf, "hackrf", "freq_mhz", gtk_spin_button_get_value(GTK_SPIN_BUTTON(A.freq_spin)));
    g_key_file_set_integer(kf, "hackrf", "rate", gtk_combo_box_get_active(GTK_COMBO_BOX(A.rfrate_combo)));
    g_key_file_set_integer(kf, "hackrf", "lna", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.lna_spin)));
    g_key_file_set_integer(kf, "hackrf", "vga", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.vga_spin)));
    g_key_file_set_boolean(kf, "hackrf", "amp", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.amp_check)));
    g_key_file_set_boolean(kf, "hackrf", "auto_gain", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.agc_check)));
    g_key_file_set_string(kf, "source", "host", gtk_entry_get_text(GTK_ENTRY(A.host_entry)));
    g_key_file_set_integer(kf, "source", "port", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.port_spin)));
    g_key_file_set_boolean(kf, "source", "lsb", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.lsb_check)));
    g_key_file_set_boolean(kf, "source", "repeat", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.repeat_check)));
    g_key_file_set_boolean(kf, "output", "audio", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.audio_check)));
    g_key_file_set_boolean(kf, "output", "realtime", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.realtime_check)));
    g_key_file_set_string(kf, "output", "wav", gtk_entry_get_text(GTK_ENTRY(A.wav_entry)));
    g_key_file_set_integer(kf, "output", "upsample", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.ups_spin)));
    g_key_file_set_integer(kf, "output", "volume", (int)gtk_range_get_value(GTK_RANGE(A.vol_scale)));
    g_key_file_set_boolean(kf, "output", "muted", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.mute_btn)));
    g_key_file_set_boolean(kf, "decoder", "descramble", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.descr_check)));
    g_key_file_set_boolean(kf, "decoder", "bch_check", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.bch_check)));
    g_key_file_set_boolean(kf, "decoder", "auto_deint", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.deint_check)));
    g_key_file_set_boolean(kf, "decoder", "stream_focus", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.focus_check)));
    g_key_file_set_integer(kf, "decoder", "log_level", gtk_combo_box_get_active(GTK_COMBO_BOX(A.log_combo)));
    if (A.st_valid) {
        g_key_file_set_integer(kf, "channel", "ch", A.st.sel_l);
        g_key_file_set_boolean(kf, "channel", "stereo", A.st.sel_stereo);
    }
    char *path = config_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(dir);
    g_free(path);
    g_key_file_free(kf);
}

static int kf_int(GKeyFile *kf, const char *g, const char *k, int def)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(kf, g, k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

static bool kf_bool(GKeyFile *kf, const char *g, const char *k, bool def)
{
    GError *e = NULL;
    gboolean v = g_key_file_get_boolean(kf, g, k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

static double kf_double(GKeyFile *kf, const char *g, const char *k, double def)
{
    GError *e = NULL;
    double v = g_key_file_get_double(kf, g, k, &e);
    if (e) { g_error_free(e); return def; }
    return v;
}

static void kf_str(GKeyFile *kf, const char *g, const char *k, GtkWidget *entry)
{
    char *v = g_key_file_get_string(kf, g, k, NULL);
    if (v) { gtk_entry_set_text(GTK_ENTRY(entry), v); g_free(v); }
}

static int g_saved_ch = 0;
static bool g_saved_stereo = true;

static void settings_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = config_path();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(A.src_combo), CLAMP(kf_int(kf, "source", "type", 0), 0, 3));
        kf_str(kf, "source", "file", A.file_entry);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.iqrate_spin), kf_int(kf, "source", "iqrate", 20480000));
        gtk_combo_box_set_active(GTK_COMBO_BOX(A.iqfmt_combo), CLAMP(kf_int(kf, "source", "iqfmt", 0), 0, 2));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.track_check), kf_bool(kf, "source", "iq_track", false));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.freq_spin),
                                  kf_double(kf, "hackrf", "freq_mhz", HACKRF_RX_DEFAULT_FREQ / 1e6));
        gtk_combo_box_set_active(GTK_COMBO_BOX(A.rfrate_combo), CLAMP(kf_int(kf, "hackrf", "rate", 0), 0, 1));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.lna_spin), kf_int(kf, "hackrf", "lna", HACKRF_RX_DEFAULT_LNA));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.vga_spin), kf_int(kf, "hackrf", "vga", HACKRF_RX_DEFAULT_VGA));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.amp_check), kf_bool(kf, "hackrf", "amp", false));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.agc_check), kf_bool(kf, "hackrf", "auto_gain", true));
        kf_str(kf, "source", "host", A.host_entry);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.port_spin), kf_int(kf, "source", "port", 9000));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.lsb_check), kf_bool(kf, "source", "lsb", false));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.repeat_check), kf_bool(kf, "source", "repeat", false));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.audio_check), kf_bool(kf, "output", "audio", true));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.realtime_check), kf_bool(kf, "output", "realtime", true));
        kf_str(kf, "output", "wav", A.wav_entry);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.ups_spin), kf_int(kf, "output", "upsample", 1));
        gtk_range_set_value(GTK_RANGE(A.vol_scale), CLAMP(kf_int(kf, "output", "volume", 80), 0, 100));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.mute_btn), kf_bool(kf, "output", "muted", false));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.descr_check), kf_bool(kf, "decoder", "descramble", true));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.bch_check), kf_bool(kf, "decoder", "bch_check", false));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.deint_check), kf_bool(kf, "decoder", "auto_deint", true));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.focus_check), kf_bool(kf, "decoder", "stream_focus", true));
        gtk_combo_box_set_active(GTK_COMBO_BOX(A.log_combo), CLAMP(kf_int(kf, "decoder", "log_level", 0), 0, 3));
        g_saved_ch = kf_int(kf, "channel", "ch", 0) & 31;
        g_saved_stereo = kf_bool(kf, "channel", "stereo", true);
    }
    g_free(path);
    g_key_file_free(kf);
}

/* ------------------------------------------------------------------ start / stop */

static void reset_display(void)
{
    A.st_valid = false;
    reset_rows();
    gtk_label_set_markup(GTK_LABEL(A.np_title), "<span size=\"xx-large\" weight=\"bold\">—</span>");
    gtk_label_set_text(GTK_LABEL(A.np_sub), "Not running");
    set_indicator(A.ind_will, "Williard", 0);
    set_indicator(A.ind_deint, "Deinterleave", 0);
    set_indicator(A.ind_sa, "SA cycle", 0);
    set_indicator(A.ind_off, "SA offset", 0);
    set_indicator(A.ind_list, "Station list", 0);
    set_indicator(A.ind_audio, "Audio", 0);
    gtk_label_set_text(GTK_LABEL(A.stats_lbl), "");
    gtk_widget_set_visible(A.rf_lbl, false);
    gtk_widget_set_visible(A.progress, false);
}

/* After stop the tick no longer refreshes: drop the live indications (locks, "Playing",
 * ▶ marker) but keep station list and last counters for reference. */
static void show_stopped(void)
{
    set_indicator(A.ind_will, "Williard", 0);
    set_indicator(A.ind_deint, "Deinterleave", 0);
    set_indicator(A.ind_sa, "SA cycle", 0);
    set_indicator(A.ind_off, "SA offset", 0);
    set_indicator(A.ind_list, "Station list", 0);
    set_indicator(A.ind_audio, "Stopped", 0);

    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(A.store), &it);
    while (ok) {
        gtk_list_store_set(A.store, &it, COL_MARK, "", COL_WEIGHT, PANGO_WEIGHT_NORMAL, -1);
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(A.store), &it);
    }
    memset(A.row_cache, 0, sizeof(A.row_cache));

    if (A.st_valid) {
        char *sub = g_strdup_printf("%s · stopped", gtk_label_get_text(GTK_LABEL(A.np_sub)));
        gtk_label_set_text(GTK_LABEL(A.np_sub), sub);
        g_free(sub);
    } else {
        gtk_label_set_text(GTK_LABEL(A.np_sub), "Not running");
    }
}

static void engine_stop_ui(const char *why)
{
    if (A.eng) {
        engine_stop(A.eng);
        A.eng = NULL;
    }
    show_stopped();
    update_source_widgets();
    if (why) status_msg(why);
}

static void do_start(void)
{
    engine_cfg_t c;
    engine_default_cfg(&c);
    c.src = cur_src();
    snprintf(c.path, sizeof(c.path), "%s", gtk_entry_get_text(GTK_ENTRY(A.file_entry)));
    c.iqrate = (unsigned)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.iqrate_spin));
    c.iq_fmt = gtk_combo_box_get_active(GTK_COMBO_BOX(A.iqfmt_combo));
    c.iq_track = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.track_check));
    c.rx.freq_hz = (uint64_t)(gtk_spin_button_get_value(GTK_SPIN_BUTTON(A.freq_spin)) * 1e6 + 0.5);
    c.rx.sample_rate = rf_rates[CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(A.rfrate_combo)), 0, 1)];
    c.rx.lna_db = (unsigned)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.lna_spin));
    c.rx.vga_db = (unsigned)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.vga_spin));
    c.rx.amp = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.amp_check));
    c.rx.auto_gain = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.agc_check));
    snprintf(c.host, sizeof(c.host), "%s", gtk_entry_get_text(GTK_ENTRY(A.host_entry)));
    c.port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.port_spin));
    c.msb_first = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.lsb_check));
    c.repeat = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.repeat_check));
    c.audioout = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.audio_check));
    c.realtime = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.realtime_check));
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.wav_check)))
        snprintf(c.wav, sizeof(c.wav), "%s", gtk_entry_get_text(GTK_ENTRY(A.wav_entry)));
    c.upsample = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(A.ups_spin));
    c.rate = DSR_OUT_RATE * c.upsample;
    c.descramble = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.descr_check));
    c.bch_check = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.bch_check));
    c.auto_deint = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.deint_check));
    c.stream_focus = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.focus_check));
    c.log_level = (dsr_log_level_t)gtk_combo_box_get_active(GTK_COMBO_BOX(A.log_combo));
    c.start_ch = A.st_valid ? A.st.sel_l : g_saved_ch;
    c.start_stereo = A.st_valid ? A.st.sel_stereo : g_saved_stereo;

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.wav_check)) && !c.wav[0]) {
        status_msg("Choose a WAV file or disable recording");
        return;
    }

    char err[256];
    reset_display();
    A.eng = engine_start(&c, err, sizeof(err));
    if (!A.eng) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(A.win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                              GTK_BUTTONS_CLOSE, "Cannot start decoder");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", err);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        status_msg(err);
        return;
    }
    engine_set_volume(A.eng, vol_gain());
    engine_set_mute(A.eng, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.mute_btn)));
    settings_save();
    update_source_widgets();
    if (c.src == ENGINE_SRC_UDP) {
        char m[128];
        snprintf(m, sizeof(m), "Listening on UDP %s:%d", c.host, c.port);
        status_msg(m);
    } else if (c.src == ENGINE_SRC_HACKRF) {
        char m[128];
        snprintf(m, sizeof(m), "HackRF: searching for a DSR mux at %.3f MHz", (double)c.rx.freq_hz / 1e6);
        status_msg(m);
    } else {
        char *base = g_path_get_basename(c.path);
        char m[512];
        snprintf(m, sizeof(m), "Decoding %s", base);
        status_msg(m);
        g_free(base);
    }
}

static void on_start_clicked(GtkButton *b, gpointer u)
{
    (void)b; (void)u;
    if (A.eng) engine_stop_ui("Stopped");
    else       do_start();
}

/* ------------------------------------------------------------------ controls */

static void on_prev(GtkButton *b, gpointer u) { (void)b; (void)u; if (A.eng) engine_step(A.eng, -1); }
static void on_next(GtkButton *b, gpointer u) { (void)b; (void)u; if (A.eng) engine_step(A.eng, +1); }
static void on_resync(GtkButton *b, gpointer u) { (void)b; (void)u; if (A.eng) engine_resync(A.eng); }

static void on_mode_toggled(GtkToggleButton *b, gpointer u)
{
    (void)u;
    if (A.updating || !A.eng || !gtk_toggle_button_get_active(b)) return;
    engine_set_stereo(A.eng, GTK_WIDGET(b) == A.stereo_radio);
}

static void on_mute_toggled(GtkToggleButton *b, gpointer u)
{
    (void)u;
    update_mute_icon();
    if (A.eng) engine_set_mute(A.eng, gtk_toggle_button_get_active(b));
}

static void on_volume_changed(GtkRange *r, gpointer u)
{
    (void)r; (void)u;
    update_mute_icon();
    if (A.eng) engine_set_volume(A.eng, vol_gain());
}

static gchar *on_volume_format(GtkScale *sc, gdouble v, gpointer u)
{
    (void)sc; (void)u;
    return g_strdup_printf("%.0f %%", v);
}

static void volume_step(int delta)
{
    double v = gtk_range_get_value(GTK_RANGE(A.vol_scale)) + delta;
    gtk_range_set_value(GTK_RANGE(A.vol_scale), CLAMP(v, 0.0, 100.0));
}

static void on_log_changed(GtkComboBox *c, gpointer u)
{
    (void)u;
    if (A.updating || !A.eng) return;
    engine_set_log_level(A.eng, (dsr_log_level_t)gtk_combo_box_get_active(c));
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer u)
{
    (void)col; (void)u;
    if (!A.eng) return;
    GtkTreeIter it;
    if (!gtk_tree_model_get_iter(gtk_tree_view_get_model(tv), &it, path)) return;
    int ch = 0;
    gboolean stereo = FALSE, known = FALSE;
    gtk_tree_model_get(gtk_tree_view_get_model(tv), &it, COL_CH, &ch, COL_STEREO, &stereo,
                       COL_KNOWN, &known, -1);
    /* Unknown PA: keep the current mode */
    if (!known) stereo = A.st_valid ? A.st.sel_stereo : TRUE;
    engine_select(A.eng, ch, stereo);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer u)
{
    (void)w; (void)u;
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(A.win));
    if (focus && (GTK_IS_ENTRY(focus))) return FALSE;  /* typing in a field */
    if ((ev->state & GDK_CONTROL_MASK) && ev->keyval == GDK_KEY_q) {
        gtk_window_close(GTK_WINDOW(A.win));
        return TRUE;
    }
    if (ev->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) return FALSE;
    switch (ev->keyval) {
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
        volume_step(+5); return TRUE;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
        volume_step(-5); return TRUE;
    case GDK_KEY_x:
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.mute_btn),
                                     !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(A.mute_btn)));
        return TRUE;
    default: break;
    }
    if (!A.eng) return FALSE;
    switch (ev->keyval) {
    case GDK_KEY_u: engine_step(A.eng, +1); return TRUE;
    case GDK_KEY_d: engine_step(A.eng, -1); return TRUE;
    case GDK_KEY_s: engine_set_stereo(A.eng, true); return TRUE;
    case GDK_KEY_m: engine_set_stereo(A.eng, false); return TRUE;
    case GDK_KEY_r: engine_resync(A.eng); return TRUE;
    case GDK_KEY_v: {
        int l = (gtk_combo_box_get_active(GTK_COMBO_BOX(A.log_combo)) + 1) % 4;
        gtk_combo_box_set_active(GTK_COMBO_BOX(A.log_combo), l);
        return TRUE;
    }
    default: return FALSE;
    }
}

/* ------------------------------------------------------------------ periodic refresh */

static void update_channel_rows(const engine_status_t *s)
{
    row_t rows[ENGINE_NUM_CH];
    set_rows(rows, build_layout(s, rows));

    GtkTreeIter it;
    gboolean ok = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(A.store), &it);
    for (int i = 0; ok && i < A.n_rows; i++) {
        const row_t *r = &A.rows[i];
        const engine_chan_t *c = &s->ch[r->ch];
        const char *mode;
        bool playing, known;
        if (r->stereo) {
            /* pair row: label/type/content from L, falling back to R */
            if (!c->valid || !c->name[0]) c = &s->ch[r->ch + 1];
            mode = "Stereo";
            known = true;
            playing = (s->sel_l & ~1) == r->ch;
        } else {
            mode = !c->valid ? "" : c->mode == 1 ? "Mono" : "—";
            known = c->valid && c->mode == 1;
            playing = s->sel_stereo ? (s->sel_l & ~1) == (r->ch & ~1) : s->sel_l == r->ch;
        }
        const char *ms = c->valid ? (c->music ? "Music" : "Speech") : "";
        const char *genre = c->valid ? engine_pa_type_name(c->type) : "";
        const char *label = c->name[0] ? c->name : (c->valid ? "?" : "");

        char key[160];
        snprintf(key, sizeof(key), "%d|%s|%s|%s|%s", playing, label, mode, ms, genre);
        if (strcmp(key, A.row_cache[i]) != 0) {
            memcpy(A.row_cache[i], key, sizeof(key));
            gtk_list_store_set(A.store, &it,
                               COL_MARK, playing ? "▶" : "",
                               COL_LABEL, label, COL_MODE, mode, COL_MS, ms, COL_GENRE, genre,
                               COL_WEIGHT, playing ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                               COL_KNOWN, known, -1);
        }
        ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(A.store), &it);
    }
}

static void update_now_playing(const engine_status_t *s)
{
    const engine_chan_t *c = &s->ch[s->sel_l];
    const char *name = c->name[0] ? c->name : "—";
    char *t = g_markup_printf_escaped("<span size=\"xx-large\" weight=\"bold\">%s</span>", name);
    gtk_label_set_markup(GTK_LABEL(A.np_title), t);
    g_free(t);

    char sub[256];
    char chs[32], nl[4], nr[4];
    if (s->sel_stereo) snprintf(chs, sizeof(chs), "Ch %s/%s · Stereo", engine_ch_name(s->sel_l, nl),
                                engine_ch_name(s->sel_r, nr));
    else               snprintf(chs, sizeof(chs), "Ch %s · Mono", engine_ch_name(s->sel_l, nl));
    if (s->src == ENGINE_SRC_HACKRF && !(s->will_locked && s->sa_locked))
        snprintf(sub, sizeof(sub), "Searching for a DSR mux at %.3f MHz …", (double)s->rf_freq_hz / 1e6);
    else if (c->valid)
        snprintf(sub, sizeof(sub), "%s · %s · %s", chs, engine_pa_type_name(c->type),
                 c->music ? "Music" : "Speech");
    else
        snprintf(sub, sizeof(sub), "%s · waiting for programme info", chs);
    gtk_label_set_text(GTK_LABEL(A.np_sub), sub);
}

static void update_status_area(const engine_status_t *s)
{
    set_indicator(A.ind_will, "Williard", s->will_locked ? 1 : 2);
    set_indicator(A.ind_deint, "Deinterleave", s->deint_ready ? 1 : 2);
    set_indicator(A.ind_sa, "SA cycle", s->sa_locked ? 1 : 2);
    set_indicator(A.ind_off, "SA offset", s->sa_offset_ready ? 1 : 2);
    bool have_list = s->pa_names || s->pa_attrs;
    set_indicator(A.ind_list, !have_list ? "Station list" : s->pa_aligned ? "Station list" : "Station list (frozen)",
                  have_list && s->pa_aligned ? 1 : 2);
    const char *audio = !s->audio_on ? (s->wav_on ? "WAV only" : "Audio off")
                      : s->muted ? "Muted" : s->warmup ? "Warm-up" : "Playing";
    set_indicator(A.ind_audio, audio, !s->audio_on ? 0 : (s->warmup || s->muted) ? 2 : 1);

    uint64_t ok = s->bch_total - s->bch_bad;
    double bad_pct = s->bch_total ? 100.0 * (double)s->bch_bad / (double)s->bch_total : 0.0;
    char txt[512];
    snprintf(txt, sizeof(txt),
             "BCH ok %" G_GUINT64_FORMAT "  bad %" G_GUINT64_FORMAT " (%.2f%%)  corr %" G_GUINT64_FORMAT
             "   ·   SF %" G_GUINT64_FORMAT "  SF err %" G_GUINT64_FORMAT
             "   ·   names %d/32  SA realigns %u"
             "   ·   in %.1f MB   ·   xruns %" G_GUINT64_FORMAT "  drops %" G_GUINT64_FORMAT " B%s",
             (guint64)ok, (guint64)s->bch_bad, bad_pct, (guint64)s->bch_corr,
             (guint64)s->sf, (guint64)s->sf_err, s->pa_names, s->pa_realigns,
             (double)s->in_bytes / (1024.0 * 1024.0),
             (guint64)s->xruns, (guint64)s->audio_drop_bytes,
             s->resync_wait ? "   ·   resync: waiting for lock" : "");
    gtk_label_set_text(GTK_LABEL(A.stats_lbl), txt);

    gtk_widget_set_visible(A.rf_lbl, s->rf_valid);
    if (s->rf_valid) {
        char rf[512];
        int n = 0;
        if (s->src == ENGINE_SRC_HACKRF)
            n = snprintf(rf, sizeof(rf), "RF %.3f MHz  LNA %u VGA %u%s   ·   ", (double)s->rf_freq_hz / 1e6,
                         s->rf_lna, s->rf_vga, s->rf_amp ? " amp" : "");
        n += snprintf(rf + n, sizeof(rf) - (size_t)n,
                      "level %.1f dBFS   ·   CFO %+.2f kHz   ·   clock %+.1f ppm   ·   EVM %.0f %%",
                      s->rf_level_dbfs, s->rf_cfo_hz / 1000.0f, s->rf_clock_ppm, s->rf_evm * 100.0f);
        if (s->src == ENGINE_SRC_HACKRF && s->rf_drop_bytes)
            n += snprintf(rf + n, sizeof(rf) - (size_t)n, "   ·   USB drops %.1f MB",
                          (double)s->rf_drop_bytes / (1024.0 * 1024.0));
        if (s->rf_acq_retries)
            snprintf(rf + n, sizeof(rf) - (size_t)n, "   ·   searches %u", s->rf_acq_retries);
        gtk_label_set_text(GTK_LABEL(A.rf_lbl), rf);
    }

    if (s->progress >= 0.0) {
        gtk_widget_set_visible(A.progress, true);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(A.progress), CLAMP(s->progress, 0.0, 1.0));
    } else {
        gtk_widget_set_visible(A.progress, false);
    }

    /* keep controls in sync with the engine without re-triggering commands */
    A.updating = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->sel_stereo ? A.stereo_radio : A.mono_radio), TRUE);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(A.log_combo)) != (int)s->log_level &&
        !gtk_widget_has_focus(A.log_combo))
        gtk_combo_box_set_active(GTK_COMBO_BOX(A.log_combo), (int)s->log_level);
    A.updating = false;
}

static gboolean on_tick(gpointer u)
{
    (void)u;
    if (!A.eng) return G_SOURCE_CONTINUE;
    engine_status_t s;
    engine_get_status(A.eng, &s);

    if (s.msg[0] && (!A.st_valid || strcmp(s.msg, A.st.msg) != 0)) status_msg(s.msg);
    A.st = s;
    A.st_valid = true;

    update_channel_rows(&s);
    update_now_playing(&s);
    update_status_area(&s);

    if (s.state != ENGINE_RUNNING) {
        char m[300];
        snprintf(m, sizeof(m), "%s: %s", s.state == ENGINE_FINISHED ? "Finished" : "Error", s.msg);
        engine_stop_ui(m);
    }
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ UI construction */

static GtkWidget *label_left(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    return l;
}

static GtkWidget *frame_with(const char *title, GtkWidget *child)
{
    GtkWidget *f = gtk_frame_new(NULL);
    char *m = g_markup_printf_escaped("<b>%s</b>", title);
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_frame_set_label_widget(GTK_FRAME(f), l);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(f), child);
    return f;
}

static GtkWidget *build_source_frame(void)
{
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 8);
    int r = 0;

    A.src_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.src_combo), "IQ file (built-in demodulator)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.src_combo), "Raw A/B bitstream file");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.src_combo), "UDP stream (raw bitstream)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.src_combo),
                                   hackrf_rx_available() ? "HackRF One (live RF, built-in demodulator)"
                                                         : "HackRF One (not built in: needs libhackrf-dev)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(A.src_combo), 0);
    g_signal_connect(A.src_combo, "changed", G_CALLBACK(on_src_changed), NULL);
    gtk_grid_attach(GTK_GRID(g), label_left("Source"), 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(g), A.src_combo, 1, r++, 2, 1);

    A.file_lbl = label_left("File");
    A.file_entry = gtk_entry_new();
    gtk_widget_set_hexpand(A.file_entry, TRUE);
    A.file_btn = gtk_button_new_with_label("Browse…");
    g_signal_connect(A.file_btn, "clicked", G_CALLBACK(on_browse_input), NULL);
    gtk_grid_attach(GTK_GRID(g), A.file_lbl, 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(g), A.file_entry, 1, r, 1, 1);
    gtk_grid_attach(GTK_GRID(g), A.file_btn, 2, r++, 1, 1);

    A.iq_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    A.iqrate_spin = gtk_spin_button_new_with_range(1000, 200000000, 1000);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.iqrate_spin), 20480000);
    A.iqfmt_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.iqfmt_combo), "s16le");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.iqfmt_combo), "f32le");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.iqfmt_combo), "s8 (HackRF)");
    gtk_combo_box_set_active(GTK_COMBO_BOX(A.iqfmt_combo), 0);
    A.track_check = gtk_check_button_new_with_label("SDR recording");
    gtk_widget_set_tooltip_text(A.track_check,
        "Symbol timing and carrier recovery, for recordings made with an SDR "
        "(e.g. hackrf_transfer). Not needed for IQ files written by the encoder.");
    gtk_box_pack_start(GTK_BOX(A.iq_box), gtk_label_new("IQ rate [Hz]"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(A.iq_box), A.iqrate_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(A.iq_box), gtk_label_new("Format"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(A.iq_box), A.iqfmt_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(A.iq_box), A.track_check, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g), A.iq_box, 1, r++, 2, 1);

    /* HackRF */
    A.rf_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *rf1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    A.freq_spin = gtk_spin_button_new_with_range(1.0, 6000.0, 0.1);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(A.freq_spin), 3);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.freq_spin), HACKRF_RX_DEFAULT_FREQ / 1e6);
    gtk_widget_set_tooltip_text(A.freq_spin, "Centre frequency of the DSR mux (118 MHz = cable channel S2)");
    A.rfrate_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.rfrate_combo), "20.48 MS/s");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.rfrate_combo), "20.00 MS/s");
    gtk_combo_box_set_active(GTK_COMBO_BOX(A.rfrate_combo), 0);
    gtk_widget_set_tooltip_text(A.rfrate_combo,
        "20.48 MS/s = exactly 2 samples per symbol. Use 20.00 MS/s (HackRF maximum spec) "
        "if the status line reports USB drops.");
    gtk_box_pack_start(GTK_BOX(rf1), gtk_label_new("Frequency [MHz]"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rf1), A.freq_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rf1), gtk_label_new("Rate"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rf1), A.rfrate_combo, FALSE, FALSE, 0);
    GtkWidget *rf2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    A.agc_check = gtk_check_button_new_with_label("Auto gain");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.agc_check), TRUE);
    gtk_widget_set_tooltip_text(A.agc_check, "Step LNA/VGA so the ADC is neither clipping nor underused");
    g_signal_connect(A.agc_check, "toggled", G_CALLBACK(on_agc_toggled), NULL);
    A.lna_spin = gtk_spin_button_new_with_range(0, 40, 8);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.lna_spin), HACKRF_RX_DEFAULT_LNA);
    A.vga_spin = gtk_spin_button_new_with_range(0, 62, 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.vga_spin), HACKRF_RX_DEFAULT_VGA);
    A.amp_check = gtk_check_button_new_with_label("RF amp");
    gtk_widget_set_tooltip_text(A.amp_check, "HackRF RF amplifier (+14 dB) — for weak signals only");
    gtk_box_pack_start(GTK_BOX(rf2), A.agc_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rf2), gtk_label_new("LNA [dB]"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rf2), A.lna_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rf2), gtk_label_new("VGA [dB]"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rf2), A.vga_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rf2), A.amp_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(A.rf_box), rf1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(A.rf_box), rf2, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g), A.rf_box, 1, r++, 2, 1);

    A.udp_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    A.host_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(A.host_entry), "0.0.0.0");
    gtk_entry_set_width_chars(GTK_ENTRY(A.host_entry), 16);
    A.port_spin = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.port_spin), 9000);
    gtk_box_pack_start(GTK_BOX(A.udp_box), gtk_label_new("Listen host"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(A.udp_box), A.host_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(A.udp_box), gtk_label_new("Port"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(A.udp_box), A.port_spin, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g), A.udp_box, 1, r++, 2, 1);

    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    A.audio_check = gtk_check_button_new_with_label("Live audio (ALSA)");
    A.realtime_check = gtk_check_button_new_with_label("Real-time pacing");
    A.repeat_check = gtk_check_button_new_with_label("Repeat file");
    A.lsb_check = gtk_check_button_new_with_label("LSB-first");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.audio_check), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.realtime_check), TRUE);
    gtk_widget_set_tooltip_text(A.realtime_check, "Throttle file input to real time even without live audio");
    gtk_widget_set_tooltip_text(A.lsb_check, "Bits LSB-first within byte (raw file / UDP only)");
    gtk_box_pack_start(GTK_BOX(opts), A.audio_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), A.realtime_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), A.repeat_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opts), A.lsb_check, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(g), opts, 1, r++, 2, 1);

    A.wav_check = gtk_check_button_new_with_label("Record WAV");
    g_signal_connect(A.wav_check, "toggled", G_CALLBACK(on_wav_toggled), NULL);
    A.wav_entry = gtk_entry_new();
    A.wav_btn = gtk_button_new_with_label("Choose…");
    g_signal_connect(A.wav_btn, "clicked", G_CALLBACK(on_browse_wav), NULL);
    gtk_grid_attach(GTK_GRID(g), A.wav_check, 0, r, 1, 1);
    gtk_grid_attach(GTK_GRID(g), A.wav_entry, 1, r, 1, 1);
    gtk_grid_attach(GTK_GRID(g), A.wav_btn, 2, r++, 1, 1);

    /* advanced */
    GtkWidget *adv = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(adv), 6);
    gtk_grid_set_column_spacing(GTK_GRID(adv), 12);
    A.descr_check = gtk_check_button_new_with_label("PRBS descramble");
    A.bch_check = gtk_check_button_new_with_label("BCH check");
    A.deint_check = gtk_check_button_new_with_label("Auto deinterleave");
    A.focus_check = gtk_check_button_new_with_label("Stream focus (less CPU)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.descr_check), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.deint_check), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.focus_check), TRUE);
    A.ups_spin = gtk_spin_button_new_with_range(1, 128, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.ups_spin), 1);
    A.rate_label = gtk_label_new(NULL);
    g_signal_connect(A.ups_spin, "value-changed", G_CALLBACK(on_upsample_changed), NULL);
    on_upsample_changed(GTK_SPIN_BUTTON(A.ups_spin), NULL);
    gtk_grid_attach(GTK_GRID(adv), A.descr_check, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(adv), A.bch_check, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(adv), A.deint_check, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(adv), A.focus_check, 3, 0, 1, 1);
    GtkWidget *rb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(rb), gtk_label_new("Upsample ×"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rb), A.ups_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rb), A.rate_label, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(adv), rb, 0, 1, 4, 1);
    GtkWidget *exp = gtk_expander_new("Advanced decoder options");
    gtk_container_add(GTK_CONTAINER(exp), adv);
    gtk_grid_attach(GTK_GRID(g), exp, 0, r++, 3, 1);

    A.src_frame = frame_with("Input", g);
    return A.src_frame;
}

static GtkWidget *build_now_playing(void)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    A.np_title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(A.np_title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(A.np_title), PANGO_ELLIPSIZE_END);
    A.np_sub = label_left("");
    gtk_box_pack_start(GTK_BOX(v), A.np_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), A.np_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);

    /* live audio: mute + volume (usable while stopped; applied on Start) */
    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(vb, GTK_ALIGN_CENTER);
    A.mute_btn = gtk_toggle_button_new();
    A.mute_img = gtk_image_new();
    gtk_button_set_image(GTK_BUTTON(A.mute_btn), A.mute_img);
    gtk_button_set_relief(GTK_BUTTON(A.mute_btn), GTK_RELIEF_NONE);
    A.vol_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0);
    gtk_range_set_value(GTK_RANGE(A.vol_scale), 80.0);
    gtk_scale_set_digits(GTK_SCALE(A.vol_scale), 0);
    gtk_scale_set_value_pos(GTK_SCALE(A.vol_scale), GTK_POS_RIGHT);
    gtk_widget_set_size_request(A.vol_scale, 200, -1);
    gtk_widget_set_tooltip_text(A.vol_scale, "Live audio volume (+ / −); WAV recording is not affected");
    g_signal_connect(A.mute_btn, "toggled", G_CALLBACK(on_mute_toggled), NULL);
    g_signal_connect(A.vol_scale, "value-changed", G_CALLBACK(on_volume_changed), NULL);
    g_signal_connect(A.vol_scale, "format-value", G_CALLBACK(on_volume_format), NULL);
    update_mute_icon();
    gtk_box_pack_start(GTK_BOX(vb), A.mute_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vb), A.vol_scale, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(h), vb, FALSE, FALSE, 0);

    return frame_with("Now playing", h);
}

static void add_text_col(const char *title, int col, float xalign, bool expand)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "xalign", xalign, NULL);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(title, r, "text", col,
                                                                     "weight", COL_WEIGHT, NULL);
    gtk_tree_view_column_set_expand(c, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(A.tree), c);
}

/* Height of n list rows (+ the column header if with_header), measured from the current
 * font/theme the same way GtkTreeView lays out a row: cell height + vertical-separator. */
static int list_rows_height(int n, bool with_header)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_ref_sink(r);
    g_object_set(r, "text", "16L/16R Ag", "weight", PANGO_WEIGHT_BOLD, NULL);
    int row_h = 0, sep = 0, hdr = 0;
    gtk_cell_renderer_get_preferred_height(r, A.tree, NULL, &row_h);
    g_object_unref(r);
    gtk_widget_style_get(A.tree, "vertical-separator", &sep, NULL);
    if (with_header) {
        GtkWidget *btn = gtk_tree_view_column_get_button(gtk_tree_view_get_column(GTK_TREE_VIEW(A.tree), 0));
        if (btn) gtk_widget_get_preferred_height(btn, NULL, &hdr);
    }
    return n * (row_h + sep) + hdr;
}

static GtkWidget *build_channel_list(void)
{
    A.store = gtk_list_store_new(N_COLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    reset_rows();
    A.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(A.store));
    g_object_unref(A.store);
    gtk_tree_view_set_activate_on_single_click(GTK_TREE_VIEW(A.tree), FALSE);
    add_text_col("", COL_MARK, 0.5f, false);
    add_text_col("Ch", COL_CH_NAME, 1.0f, false);
    add_text_col("Station", COL_LABEL, 0.0f, true);
    add_text_col("Mode", COL_MODE, 0.0f, false);
    add_text_col("Content", COL_MS, 0.0f, false);
    add_text_col("Programme type", COL_GENRE, 0.0f, true);
    gtk_widget_set_tooltip_text(A.tree, "Double-click a channel to tune");
    g_signal_connect(A.tree, "row-activated", G_CALLBACK(on_row_activated), NULL);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_widget_set_vexpand(sw, TRUE);
    /* At least LIST_MIN_ROWS rows visible (16 = one full stereo mux); build_window grows the
     * default size further so all 32 rows fit when the screen allows it. */
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw),
                                               list_rows_height(LIST_MIN_ROWS, true));
    gtk_container_add(GTK_CONTAINER(sw), A.tree);
    return sw;
}

static GtkWidget *build_controls(void)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    A.prev_btn = gtk_button_new_with_label("◀ Prev");
    A.next_btn = gtk_button_new_with_label("Next ▶");
    gtk_widget_set_tooltip_text(A.prev_btn, "Previous channel / pair (d)");
    gtk_widget_set_tooltip_text(A.next_btn, "Next channel / pair (u)");
    g_signal_connect(A.prev_btn, "clicked", G_CALLBACK(on_prev), NULL);
    g_signal_connect(A.next_btn, "clicked", G_CALLBACK(on_next), NULL);

    A.stereo_radio = gtk_radio_button_new_with_label(NULL, "Stereo");
    A.mono_radio = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(A.stereo_radio), "Mono");
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(A.stereo_radio), FALSE);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(A.mono_radio), FALSE);
    gtk_widget_set_tooltip_text(A.stereo_radio, "Force stereo (s)");
    gtk_widget_set_tooltip_text(A.mono_radio, "Force mono (m)");
    g_signal_connect(A.stereo_radio, "toggled", G_CALLBACK(on_mode_toggled), NULL);
    g_signal_connect(A.mono_radio, "toggled", G_CALLBACK(on_mode_toggled), NULL);
    GtkWidget *mode_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(mode_box), "linked");
    gtk_box_pack_start(GTK_BOX(mode_box), A.stereo_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mode_box), A.mono_radio, FALSE, FALSE, 0);

    A.resync_btn = gtk_button_new_with_label("Resync");
    gtk_widget_set_tooltip_text(A.resync_btn, "Hard resync: clear decoder/PA state (r)");
    g_signal_connect(A.resync_btn, "clicked", G_CALLBACK(on_resync), NULL);

    A.log_combo = gtk_combo_box_text_new();
    for (int i = 0; i < 4; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(A.log_combo),
                                       engine_log_level_name((dsr_log_level_t)i));
    gtk_combo_box_set_active(GTK_COMBO_BOX(A.log_combo), 0);
    gtk_widget_set_tooltip_text(A.log_combo, "Decoder log level on stderr (v)");
    g_signal_connect(A.log_combo, "changed", G_CALLBACK(on_log_changed), NULL);

    gtk_box_pack_start(GTK_BOX(h), A.prev_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), A.next_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), mode_box, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(h), A.resync_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(h), A.log_combo, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(h), gtk_label_new("Log"), FALSE, FALSE, 0);
    A.ctrl_box = h;
    return h;
}

static GtkWidget *build_status(void)
{
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    A.ind_will = gtk_label_new(NULL);
    A.ind_deint = gtk_label_new(NULL);
    A.ind_sa = gtk_label_new(NULL);
    A.ind_off = gtk_label_new(NULL);
    A.ind_list = gtk_label_new(NULL);
    gtk_widget_set_tooltip_text(A.ind_list,
        "Green: station names/attributes confirmed (received identically several times) "
        "and new data is accepted. Orange: not confirmed yet, or list frozen at its "
        "confirmed content while the SA alignment is redone after a sync loss.");
    A.ind_audio = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(h), A.ind_will, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), A.ind_deint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), A.ind_sa, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), A.ind_off, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), A.ind_list, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(h), A.ind_audio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
    A.stats_lbl = label_left("");
    gtk_label_set_selectable(GTK_LABEL(A.stats_lbl), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(A.stats_lbl), PANGO_ELLIPSIZE_END);
    PangoAttrList *al = pango_attr_list_new();
    pango_attr_list_insert(al, pango_attr_family_new("monospace"));
    pango_attr_list_insert(al, pango_attr_scale_new(0.9));
    gtk_label_set_attributes(GTK_LABEL(A.stats_lbl), al);
    pango_attr_list_unref(al);
    gtk_box_pack_start(GTK_BOX(v), A.stats_lbl, FALSE, FALSE, 0);
    A.rf_lbl = label_left("");
    gtk_label_set_selectable(GTK_LABEL(A.rf_lbl), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(A.rf_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_attributes(GTK_LABEL(A.rf_lbl), gtk_label_get_attributes(GTK_LABEL(A.stats_lbl)));
    gtk_box_pack_start(GTK_BOX(v), A.rf_lbl, FALSE, FALSE, 0);
    A.progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(v), A.progress, FALSE, FALSE, 0);
    return frame_with("Receiver status", v);
}

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, gpointer u)
{
    (void)w; (void)ev; (void)u;
    settings_save();
    if (A.eng) { engine_stop(A.eng); A.eng = NULL; }
    return FALSE;
}

static void build_window(void)
{
    A.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(A.win), APP_TITLE " " APP_VER);
    gtk_window_set_icon_name(GTK_WINDOW(A.win), "audio-x-generic");
    g_signal_connect(A.win, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(A.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(A.win, "key-press-event", G_CALLBACK(on_key), NULL);

    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(v), 10);

    gtk_box_pack_start(GTK_BOX(v), build_source_frame(), FALSE, FALSE, 0);

    A.start_btn = gtk_button_new_with_label("▶  Start");
    gtk_style_context_add_class(gtk_widget_get_style_context(A.start_btn), "suggested-action");
    g_signal_connect(A.start_btn, "clicked", G_CALLBACK(on_start_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(v), A.start_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(v), build_now_playing(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), build_channel_list(), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(v), build_controls(), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), build_status(), FALSE, FALSE, 0);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(outer), v, TRUE, TRUE, 0);
    A.statusbar = gtk_statusbar_new();
    A.sb_ctx = gtk_statusbar_get_context_id(GTK_STATUSBAR(A.statusbar), "main");
    gtk_box_pack_start(GTK_BOX(outer), A.statusbar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(A.win), outer);

    /* Default height: the natural layout (list at LIST_MIN_ROWS) plus the remaining rows,
     * so all 32 single-channel rows fit — capped to the monitor work area. */
    int win_h = 0;
    gtk_widget_get_preferred_height(A.win, NULL, &win_h);
    win_h += list_rows_height(ENGINE_NUM_CH - LIST_MIN_ROWS, false);
    GdkDisplay *dpy = gtk_widget_get_display(A.win);
    GdkMonitor *mon = gdk_display_get_primary_monitor(dpy);
    if (!mon) mon = gdk_display_get_monitor(dpy, 0);
    if (mon) {
        GdkRectangle wa;
        gdk_monitor_get_workarea(mon, &wa);
        int max_h = wa.height - 48;   /* leave room for the window title bar */
        if (win_h > max_h) win_h = max_h;
    }
    gtk_window_set_default_size(GTK_WINDOW(A.win), 820, win_h);
}

/* ------------------------------------------------------------------ command line */

static void usage(const char *p)
{
    fprintf(stderr,
        "dsrdec-gtk %s — GTK frontend for the DSR decoder\n"
        "Usage: %s [infile] [options]   (all options only pre-fill the window)\n"
        "  infile / --in PATH  Raw A/B bitstream file\n"
        "  --iqfile PATH       IQ file (built-in demodulator)\n"
        "  --iqrate FS         IQ sample rate (default 20480000)\n"
        "  --iqfmt s16le|f32le|s8  IQ sample format\n"
        "  --track             IQ file is an SDR recording (timing/carrier recovery)\n"
        "  --hackrf            HackRF One as source\n"
        "  --freq MHZ          HackRF frequency (default 118.0)\n"
        "  --lna DB --vga DB   HackRF gains (sets manual gain)\n"
        "  --amp               HackRF RF amplifier on\n"
        "  --host H --port P   UDP source\n"
        "  --lsb               LSB-first bit order (raw/UDP)\n"
        "  --repeat            Loop file input\n"
        "  --wav FILE.wav      Record WAV\n"
        "  --mono N | --stereo L,R  Initial channel\n"
        "  --no-audioout       Disable live audio\n"
        "  --start             Start decoding immediately\n",
        APP_VER, p);
}

static int parse_args(int argc, char **argv, bool *autostart)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has = i + 1 < argc;
        if (a[0] != '-') {
            gtk_combo_box_set_active(GTK_COMBO_BOX(A.src_combo), ENGINE_SRC_RAW);
            gtk_entry_set_text(GTK_ENTRY(A.file_entry), a);
        } else if (!strcmp(a, "--in") && has) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(A.src_combo), ENGINE_SRC_RAW);
            gtk_entry_set_text(GTK_ENTRY(A.file_entry), argv[++i]);
        } else if (!strcmp(a, "--iqfile") && has) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(A.src_combo), ENGINE_SRC_IQ);
            gtk_entry_set_text(GTK_ENTRY(A.file_entry), argv[++i]);
        } else if (!strcmp(a, "--iqrate") && has) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.iqrate_spin), atoi(argv[++i]));
        } else if (!strcmp(a, "--iqfmt") && has) {
            const char *f = argv[++i];
            gtk_combo_box_set_active(GTK_COMBO_BOX(A.iqfmt_combo),
                                     !strcmp(f, "f32le") ? 1 : (!strcmp(f, "s8") || !strcmp(f, "cs8")) ? 2 : 0);
        } else if (!strcmp(a, "--track")) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.track_check), TRUE);
        } else if (!strcmp(a, "--hackrf")) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(A.src_combo), ENGINE_SRC_HACKRF);
        } else if (!strcmp(a, "--freq") && has) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(A.src_combo), ENGINE_SRC_HACKRF);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.freq_spin), atof(argv[++i]));
        } else if (!strcmp(a, "--lna") && has) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.agc_check), FALSE);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.lna_spin), atoi(argv[++i]));
        } else if (!strcmp(a, "--vga") && has) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.agc_check), FALSE);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.vga_spin), atoi(argv[++i]));
        } else if (!strcmp(a, "--amp")) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.amp_check), TRUE);
        } else if (!strcmp(a, "--host") && has) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(A.src_combo), ENGINE_SRC_UDP);
            gtk_entry_set_text(GTK_ENTRY(A.host_entry), argv[++i]);
        } else if (!strcmp(a, "--port") && has) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(A.src_combo), ENGINE_SRC_UDP);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(A.port_spin), atoi(argv[++i]));
        } else if (!strcmp(a, "--lsb")) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.lsb_check), TRUE);
        } else if (!strcmp(a, "--repeat")) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.repeat_check), TRUE);
        } else if (!strcmp(a, "--wav") && has) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.wav_check), TRUE);
            gtk_entry_set_text(GTK_ENTRY(A.wav_entry), argv[++i]);
        } else if (!strcmp(a, "--mono") && has) {
            g_saved_ch = atoi(argv[++i]) & 31;
            g_saved_stereo = false;
        } else if (!strcmp(a, "--stereo") && has) {
            int l = 0, r = 1;
            sscanf(argv[++i], "%d,%d", &l, &r);
            g_saved_ch = (l < r ? l : r) & 31;
            g_saved_stereo = true;
        } else if (!strcmp(a, "--no-audioout")) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.audio_check), FALSE);
        } else if (!strcmp(a, "--start")) {
            *autostart = true;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(argv[0]);
            return -1;
        } else {
            fprintf(stderr, "Unknown option: %s\n", a);
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static gboolean autostart_cb(gpointer u) { (void)u; do_start(); return G_SOURCE_REMOVE; }

/* SIGINT/SIGTERM/SIGHUP (Ctrl+C, DSRDemo stop, closed terminal): quit like closing
 * the window, so the engine is stopped and the settings are saved. */
static gboolean on_quit_signal(gpointer u)
{
    (void)u;
    gtk_window_close(GTK_WINDOW(A.win));
    return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    build_window();
    settings_load();

    bool autostart = false;
    if (parse_args(argc, argv, &autostart) < 0) return 1;

    gtk_widget_show_all(A.win);
    reset_display();
    update_source_widgets();
    status_msg("Ready — choose an input and press Start");
    g_timeout_add(TICK_MS, on_tick, NULL);
    if (autostart) g_idle_add(autostart_cb, NULL);
    g_unix_signal_add(SIGINT, on_quit_signal, NULL);
    g_unix_signal_add(SIGTERM, on_quit_signal, NULL);
    g_unix_signal_add(SIGHUP, on_quit_signal, NULL);
    gtk_main();
    return 0;
}
