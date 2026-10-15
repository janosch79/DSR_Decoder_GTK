// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
#ifndef PA_H
#define PA_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque State */
typedef struct pa_state pa_t;

/* Construction / reset. pa_new starts with an empty station list. pa_reset (resync)
 * drops the SA alignment and unconfirmed data, but keeps the confirmed station list:
 * names and attributes are static, a resync must not change them. */
pa_t* pa_new(void);
void  pa_free(pa_t *p);
void  pa_reset(pa_t *p);
/* Call exactly once per A main frame (when reading the SSB):
 * - ssb:            the SSB of this A frame (0/1)
 * - will_locked:    true if the decoder is locked to Williard
 * - sa_pos_mod64:   0..63, A-frame position in the 64-bit SA frame (caller's counter)
 * - code_at15:      last Williard word, updated ONLY when sa_pos_mod64==15
 * - octet_pos:      0..7, Williard octet position from the decoder; unused (pa.c anchors
 *                   the octet on SYNC1 itself: the decoder's value is already advanced
 *                   to the next frame, except on its acquire frame)
 */
void pa_push_a_ssb(pa_t *p, uint8_t ssb,
                   bool will_locked, uint8_t sa_pos_mod64, uint16_t code_at15, uint8_t octet_pos);

/* Getters report CONFIRMED data only: received identically several times among the last
 * receptions (frames with a valid Williard word of their own, SA alignment intact). A
 * confirmed value is replaced only by a new value received even more often, and more
 * often than the confirmed one. */

/* Get the station name as UTF-8.
 * out is NUL-terminated (maxlen >= 1). Returns: number of bytes written (excluding NUL).
 */
int pa_get_name_utf8(const pa_t *p, int ch, char *out, int maxlen);

/* Programme attributes of channel ch (0..31): type 0..15, music 0/1, mode 0..3.
 * Returns 1 if known (any out pointer may be NULL), 0 otherwise. */
int pa_get_pa(const pa_t *p, int ch, int *type, int *music, int *mode);

/* Status */
bool     pa_sa_cycle_locked(const pa_t *p);   /* true once the 128-SA block alignment is found */
/* true once that alignment is confirmed by a blank LB block (SK/PA row-aligned) */
bool     pa_is_sa_offset_ready(const pa_t *p);

typedef struct {
    bool     aligned;    /* SA alignment verified: new data is being accepted */
    int      names;      /* channels with a confirmed name (0..32) */
    int      attrs;      /* channels with confirmed programme attributes (0..32) */
    unsigned realigns;   /* alignments dropped (Williard loss, LB block not blank) */
} pa_status_t;

/* Station list state. names > 0 && !aligned: list frozen at its confirmed content while
 * the SA alignment is being re-established. */
void pa_get_status(const pa_t *p, pa_status_t *out);

/* Enable/disable PA debug logging */
void pa_enable_debug(int enable);

#ifdef __cplusplus
}
#endif

#endif /* PA_H */
