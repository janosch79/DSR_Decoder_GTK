// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 janosch79
// pa.c — SAÜ/PA + SAÜ/SK Decoder (from SSB of A-Frames)
// Builds 64-bit SA-Frames (16b Williard + 48b Payload) and decodes:
//  - PA: Programme Attributes (type/music/mode) via reverse mapping _par[]
//  - SK: 8 character programme name per channel (DSR Charset -> UTF-8)
//
// SA cycle (encoder dsr_update_sa): 128 SA frames = 16 blocks of 8. Blocks 0..6 PA,
// block 7 LB (all zero), blocks 8..15 SK (octet b of the names). The Williard word marks
// octet 0 of each block (SYNC1 0x05CF, else SYNC2), but not the block, so the alignment comes
// from the LB block: the only one that is blank. It is found per block within one cycle
// (~256 ms), checked again on every cycle, and redone after a Williard lock loss, an
// octet jump or a LB block that is not blank (nothing is committed meanwhile).
//
// Station data is static, so it is only reported once CONFIRMED: the same value was
// received several times among the last receptions (only frames with a valid Williard
// word of their own, alignment intact). A confirmed value is replaced only by a new value
// received even more often, and more often than the confirmed one. Confirmed data survives
// realignment and pa_reset(); only pa_new() starts with an empty list.

#include "pa.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Logging switch (default: off) ---- */
static int g_pa_log = 0;
static int g_pa_debug_count = 0;
static inline void pa_set_log(int on){ g_pa_log = on ? 1 : 0; }
#define SA_LOGF(...) do { if (g_pa_log) fprintf(stderr, __VA_ARGS__); } while(0)

/* Williard codes (as used in decoder) */
#define WILLIARD_CODE_SYNC1 0x05CFu /* 0000 0101 1100 1111 */
#define WILLIARD_CODE_SYNC2 0x05FFu /* 0000 0101 1111 1111 */

/* ---------- DSR Lookup-Tables (from Encoder) ---------- */

static const uint8_t _par[256] = {
    0x00,0x00,0x03,0x03,0x05,0x05,0x06,0x06,0x09,0x09,0x0A,0x0A,0x0C,0x0C,0x0F,0x0F,
    0x11,0x11,0x12,0x12,0x14,0x14,0x17,0x17,0x18,0x18,0x1B,0x1B,0x1D,0x1D,0x1E,0x1E,
    0x21,0x21,0x22,0x22,0x24,0x24,0x27,0x27,0x28,0x28,0x2B,0x2B,0x2D,0x2D,0x2E,0x2E,
    0x30,0x30,0x33,0x33,0x35,0x35,0x36,0x36,0x39,0x39,0x3A,0x3A,0x3C,0x3C,0x3F,0x3F,
    0x41,0x41,0x42,0x42,0x44,0x44,0x47,0x47,0x48,0x48,0x4B,0x4B,0x4D,0x4D,0x4E,0x4E,
    0x50,0x50,0x53,0x53,0x55,0x55,0x56,0x56,0x59,0x59,0x5A,0x5A,0x5C,0x5C,0x5F,0x5F,
    0x60,0x60,0x63,0x63,0x65,0x65,0x66,0x66,0x69,0x69,0x6A,0x6A,0x6C,0x6C,0x6F,0x6F,
    0x71,0x71,0x72,0x72,0x74,0x74,0x77,0x77,0x78,0x78,0x7B,0x7B,0x7D,0x7D,0x7E,0x7E,
    0x81,0x81,0x82,0x82,0x84,0x84,0x87,0x87,0x88,0x88,0x8B,0x8B,0x8D,0x8D,0x8E,0x8E,
    0x90,0x90,0x93,0x93,0x95,0x95,0x96,0x96,0x99,0x99,0x9A,0x9A,0x9C,0x9C,0x9F,0x9F,
    0xA0,0xA0,0xA3,0xA3,0xA5,0xA5,0xA6,0xA6,0xA9,0xA9,0xAA,0xAA,0xAC,0xAC,0xAF,0xAF,
    0xB1,0xB1,0xB2,0xB2,0xB4,0xB4,0xB7,0xB7,0xB8,0xB8,0xBB,0xBB,0xBD,0xBD,0xBE,0xBE,
    0xC0,0xC0,0xC3,0xC3,0xC5,0xC5,0xC6,0xC6,0xC9,0xC9,0xCA,0xCA,0xCC,0xCC,0xCF,0xCF,
    0xD1,0xD1,0xD2,0xD2,0xD4,0xD4,0xD7,0xD7,0xD8,0xD8,0xDB,0xDB,0xDD,0xDD,0xDE,0xDE,
    0xE1,0xE1,0xE2,0xE2,0xE4,0xE4,0xE7,0xE7,0xE8,0xE8,0xEB,0xEB,0xED,0xED,0xEE,0xEE,
    0xF0,0xF0,0xF3,0xF3,0xF5,0xF5,0xF6,0xF6,0xF9,0xF9,0xFA,0xFA,0xFC,0xFC,0xFF,0xFF,
};

static const char *_charset[256] = {
    "Ã","Å","Æ","Œ","ŷ","Ý","Õ","Ø","Þ","Ŋ","Ŕ","Ć","Ś","Ź","Ŧ","ð",
    "ã","å","æ","œ","ŵ","ý","õ","ø","þ","ŋ","ŕ","ć","ś","ź","ŧ","",
    " ","!","\"","#","¤","%","&","'","(",")","*","+",",","-",".","/",
    "0","1","2","3","4","5","6","7","8","9",":",";","<","=",">","?",
    "@","A","B","C","D","E","F","G","H","I","J","K","L","M","N","O",
    "P","Q","R","S","T","U","V","W","X","Y","Z","[","\\","]","―","_",
    "‖","a","b","c","d","e","f","g","h","i","j","k","l","m","n","o",
    "p","q","r","s","t","u","v","w","x","y","z","{","|","}","¯","",
    "á","à","é","è","í","ì","ó","ò","ú","ù","Ñ","Ç","Ş","β","¡","Ĳ",
    "â","ä","ê","ë","î","ï","ô","ö","û","ü","ñ","ç","ş","ǧ","ı","ĳ",
    "ª","α","©","‰","Ǧ","ě","ň","ő","π","₠","£","$","←","↑","→","↓",
    "º","¹","²","³","±","İ","ń","ű","µ","¿","÷","°","¼","½","¾","§",
    "Á","À","É","È","Í","Ì","Ó","Ò","Ú","Ù","Ř","Č","Š","Ž","Ð","Ŀ",
    "Â","Ä","Ê","Ë","Î","Ï","Ô","Ö","Û","Ü","ř","č","š","ž","đ","ŀ",
    "","","","","","","","","","","","","","","","",
    "","","","","","","","","","","","","","","","",
};

/* ---------- PA/SK State ---------- */

/* Confirmation over the last receptions (alignment intact, frame Williard valid). Bit
 * errors give random values that practically never repeat; a real value repeats every
 * time. PA is sent 7x per 128-SA cycle (~256 ms), a name once per cycle.
 * First value: >= *_CONFIRM equal among the last *_WIN receptions. Change: the new
 * value >= *_CHANGE times among the last *_HIST, and more often than the confirmed one. */
#define PA_HIST        14   /* 2 cycles */
#define PA_WIN          7
#define PA_CONFIRM      4
#define PA_CHANGE      10
#define NAME_HIST       6   /* 6 cycles */
#define NAME_WIN        4
#define NAME_CONFIRM    2
#define NAME_CHANGE     3
/* LB windows with fewer than half the slots blank before realigning */
#define LB_BAD_REALIGN  2
/* A slot counts as blank with at most this many set bits in pay[0..3] (single bit
 * errors on the unprotected SSB must not break the LB detection). */
#define BLANK_MAX_BITS  2
/* Block detection: the LB candidate needs >= this many blank slots of 8, every other
 * block at most BLOCK_OTHER_MAX. */
#define BLOCK_LB_MIN    6
#define BLOCK_OTHER_MAX 3

typedef struct {
    /* confirmed, i.e. reported */
    int16_t  pa_idx;          /* (type<<4)|(music<<3)|(mode<<1); -1 = none yet */
    uint8_t  name[8];
    bool     name_valid;
    /* recent receptions, newest at [n-1] */
    int16_t  pa_hist[PA_HIST];
    uint8_t  pa_n;
    uint8_t  sk_buf[8];       /* name being assembled from the 8 SK octets */
    uint8_t  sk_mask;
    uint8_t  name_hist[NAME_HIST][8];
    uint8_t  name_n;
} pa_ch_t;

struct pa_state {
    /* Collection of current SA-Frame payload (48 bits) */
    uint8_t  pay[6];
    uint8_t  collecting;     /* remaining bits 0..48 */
    uint8_t  pending_sa_idx; /* raw SA index (0..127) for this pay[] */
    bool     pending_ok;     /* this frame's own Williard word matched its octet */

    /* raw SA index: octet anchored on SYNC1 (= encoder octet 0), block counted */
    uint8_t  sa_idx_0_127;
    bool     idx_valid;      /* octet anchored since the last Williard (re)lock */
    uint8_t  octet_err;      /* consecutive SYNC1 words at a non-zero octet */

    /* block alignment */
    bool     aligned;        /* blk_off known */
    uint8_t  blk_off;        /* adjusted block = (raw block + blk_off) & 15 */
    bool     verified;       /* >= 1 blank LB window seen since aligning */
    uint8_t  blank_mask[16]; /* per raw block: octets seen blank (last value) */
    uint8_t  seen_mask[16];  /* per raw block: octets seen since search start */

    /* LB monitor */
    uint8_t  lb_seen, lb_bad, lb_bad_windows;
    unsigned realigns;

    pa_ch_t  ch[32];
};

static inline int popcount8(uint8_t v){ return __builtin_popcount(v); }

static inline bool sa_is_blank_payload(const uint8_t pay[6]){
    return popcount8(pay[0]) + popcount8(pay[1]) + popcount8(pay[2]) + popcount8(pay[3])
           <= BLANK_MAX_BITS;
}

/* ---------- _par Reverse Mapping ---------- */
/* Encoder mode values (from dsrtx.c):
 *   mode=0: unallocated
 *   mode=1: L channel of stereo pair, or mono A/B
 *   mode=2: R channel of stereo pair
 *   mode=3: reserved
 */

static int16_t g_par_inv[256];  /* int16_t to hold indices 0-255 without overflow */
static void build_par_inverse(void){
    for(int inv_idx=0;inv_idx<256;inv_idx++) g_par_inv[inv_idx] = -1;
    for(int type=0; type<16; ++type){
        for(int music=0; music<2; ++music){
            for(int mode=0; mode<4; ++mode){  /* mode can be 0-3 (encoder uses 1,2) */
                int idx = (type<<4) | (music<<3) | (mode<<1);
                uint8_t code = _par[idx];
                g_par_inv[code] = (int16_t)idx;
            }
        }
    }
}

void pa_enable_debug(int enable) {
    g_pa_log = enable ? 1 : 0;
    if (enable) {
        g_pa_debug_count = 0;  /* Reset debug counter */
        fprintf(stderr, "[PA] g_par_inv verification: 0xAA->%d (expect 170), 0xA5->%d (expect 164), 0x00->%d (expect 0)\n",
                (int)g_par_inv[0xAA], (int)g_par_inv[0xA5], (int)g_par_inv[0x00]);
    }
}

/* ---------- Charset Helper Function ---------- */

static int name_codes_to_utf8(const uint8_t codes[8], char *out, int maxlen){
    int written_bytes = 0;
    if(maxlen<=0) return 0;
    for(int char_idx=0;char_idx<8;char_idx++){
        /* Code 0 = unused / padding in SK; _charset[0] is a national char — must not prefix names. */
        if(codes[char_idx] == 0) {
            continue;
        }
        const char *charset_str = _charset[codes[char_idx]];
        if(!charset_str || !*charset_str) charset_str = "?";
        for(const char *char_ptr=charset_str; *char_ptr; ++char_ptr){
            if(written_bytes+1 >= maxlen){ out[written_bytes]='\0'; return written_bytes; }
            out[written_bytes++] = *char_ptr;
        }
    }
    out[written_bytes] = '\0';
    return written_bytes;
}

/* ---------- Commit the 48 payload bits of an SA-Frame ---------- */

/* Any code with an empty charset entry (0x1F, 0x7F, 0xE0..0xFF) cannot be part of a name. */
static bool name_codes_plausible(const uint8_t codes[8]){
    for(int i=0;i<8;i++){
        if(codes[i] != 0 && !_charset[codes[i]][0]) return false;
    }
    return true;
}

static void clear_candidates(pa_t *pa_state){
    for(int ch=0; ch<32; ch++){
        pa_ch_t *c = &pa_state->ch[ch];
        c->pa_n = 0;
        c->sk_mask = 0;
        c->name_n = 0;
    }
}

static void pa_candidate(pa_ch_t *c, int16_t inv_idx){
    if(inv_idx < 0) return;                       /* parity error: no evidence */
    if(c->pa_n == PA_HIST){ memmove(c->pa_hist, c->pa_hist + 1, (PA_HIST-1)*sizeof(c->pa_hist[0])); c->pa_n--; }
    c->pa_hist[c->pa_n++] = inv_idx;
    if(inv_idx == c->pa_idx) return;

    int win = c->pa_idx < 0 ? PA_WIN : PA_HIST, from = c->pa_n > win ? c->pa_n - win : 0;
    int same = 0, conf = 0;
    for(int i = from; i < c->pa_n; i++){
        if(c->pa_hist[i] == inv_idx) same++;
        else if(c->pa_hist[i] == c->pa_idx) conf++;
    }
    if(c->pa_idx < 0 ? same >= PA_CONFIRM : (same >= PA_CHANGE && same > conf)){
        if(c->pa_idx >= 0)
            SA_LOGF("[pa] attribute change confirmed: 0x%02X -> 0x%02X\n", c->pa_idx, inv_idx);
        c->pa_idx = inv_idx;
    }
}

static void name_candidate(pa_ch_t *c){
    const uint8_t *rx = c->sk_buf;
    if(!name_codes_plausible(rx)) return;         /* invalid code: no evidence */
    if(c->name_n == NAME_HIST){ memmove(c->name_hist, c->name_hist + 1, (NAME_HIST-1)*8); c->name_n--; }
    memcpy(c->name_hist[c->name_n++], rx, 8);
    if(c->name_valid && memcmp(rx, c->name, 8) == 0) return;

    int win = c->name_valid ? NAME_HIST : NAME_WIN, from = c->name_n > win ? c->name_n - win : 0;
    int same = 0, conf = 0;
    for(int i = from; i < c->name_n; i++){
        if(memcmp(c->name_hist[i], rx, 8) == 0) same++;
        else if(c->name_valid && memcmp(c->name_hist[i], c->name, 8) == 0) conf++;
    }
    if(!c->name_valid ? same >= NAME_CONFIRM : (same >= NAME_CHANGE && same > conf)){
        memcpy(c->name, rx, 8);
        c->name_valid = true;
    }
}

static void commit_sa_payload(pa_t *pa_state, uint8_t sa_idx)
{
    if(sa_idx < 56){
        int group  = sa_idx & 7;
        int baseCh = group * 4;

        /* Debug: print raw PA bytes for analysis */
        if (g_pa_debug_count < 16 && g_pa_log) {
            int16_t inv_idx0 = g_par_inv[pa_state->pay[0]], inv_idx1 = g_par_inv[pa_state->pay[1]];
            int16_t inv_idx2 = g_par_inv[pa_state->pay[2]], inv_idx3 = g_par_inv[pa_state->pay[3]];
            fprintf(stderr, "[PA] SA=%3d grp=%d ch=%2d-%2d pay=[%02X %02X %02X %02X] inv=[%d %d %d %d] "
                    "type=[%d %d %d %d] music=[%d %d %d %d] mode=[%d %d %d %d]\n",
                    sa_idx, group, baseCh, baseCh+3,
                    pa_state->pay[0], pa_state->pay[1], pa_state->pay[2], pa_state->pay[3],
                    inv_idx0, inv_idx1, inv_idx2, inv_idx3,
                    inv_idx0>=0?(inv_idx0>>4)&0xF:-1, inv_idx1>=0?(inv_idx1>>4)&0xF:-1, inv_idx2>=0?(inv_idx2>>4)&0xF:-1, inv_idx3>=0?(inv_idx3>>4)&0xF:-1,
                    inv_idx0>=0?(inv_idx0>>3)&1:-1, inv_idx1>=0?(inv_idx1>>3)&1:-1, inv_idx2>=0?(inv_idx2>>3)&1:-1, inv_idx3>=0?(inv_idx3>>3)&1:-1,
                    inv_idx0>=0?(inv_idx0>>1)&3:-1, inv_idx1>=0?(inv_idx1>>1)&3:-1, inv_idx2>=0?(inv_idx2>>1)&3:-1, inv_idx3>=0?(inv_idx3>>1)&3:-1);
            g_pa_debug_count++;
        }

        /* DSR PA encoding (from encoder dsr.c):
         * Encoder: pa_byte = _par[(type << 4) | (music << 3) | (mode << 1)]
         * Decoder: index = g_par_inv[pa_byte]  (-1 = parity error)
         *          type  = (index >> 4) & 0xF
         *          music = (index >> 3) & 1
         *          mode  = (index >> 1) & 3
         *
         * pay[0..3] = PA bytes for 4 individual channels (not stereo pairs!)
         * pay[4], pay[5] = DI/DII (not used) */
        for(int channel_idx = 0; channel_idx < 4; channel_idx++)
            pa_candidate(&pa_state->ch[baseCh + channel_idx], g_par_inv[pa_state->pay[channel_idx]]);

    }else if(sa_idx < 64){
        /* LB: Blank block – checked by the alignment monitor */
        return;

    }else{
        /* SK: octet bpos of the names of channels baseCh..baseCh+3 */
        int group  = sa_idx & 7;
        int baseCh = group * 4;

        int bpos = (sa_idx - 64) >> 3;
        for(int channel_idx=0;channel_idx<4;channel_idx++){
            pa_ch_t *c = &pa_state->ch[baseCh + channel_idx];
            c->sk_buf[bpos] = pa_state->pay[channel_idx];
            c->sk_mask |= (uint8_t)(1u << bpos);
            if(c->sk_mask == 0xFF){      /* all 8 octets received: one complete name */
                name_candidate(c);
                c->sk_mask = 0;
            }
        }
        /* pay[4], pay[5] = EI/EII (optional) */
    }
}

/* ---------- Block alignment ---------- */

/* Forget the alignment (confirmed data stays). clear: also drop the candidates, when
 * data committed before the problem was noticed may be misaligned. */
static void pa_realign(pa_t *pa_state, const char *why, bool clear){
    if(why && pa_state->aligned){
        pa_state->realigns++;
        SA_LOGF("[pa] %s - SA realign (station list kept)\n", why);
    }
    pa_state->aligned = false;
    pa_state->verified = false;
    pa_state->lb_seen = pa_state->lb_bad = pa_state->lb_bad_windows = 0;
    memset(pa_state->blank_mask, 0, sizeof(pa_state->blank_mask));
    memset(pa_state->seen_mask, 0, sizeof(pa_state->seen_mask));
    if(clear) clear_candidates(pa_state);
}

/* After each completed raw block while unaligned: once all 16 blocks were seen, the LB
 * block is the single one that is (almost) blank. Ambiguous (a mux with blank PA/SK
 * blocks) or no candidate: keep looking, the masks keep the latest cycle. */
static void try_align(pa_t *pa_state){
    int lb = -1;
    for(int b=0; b<16; b++){
        if(pa_state->seen_mask[b] != 0xFF) return;
        int blanks = popcount8(pa_state->blank_mask[b]);
        if(blanks >= BLOCK_LB_MIN){
            if(lb >= 0) return;          /* two candidates */
            lb = b;
        } else if(blanks > BLOCK_OTHER_MAX){
            return;                      /* unclear block */
        }
    }
    if(lb < 0) return;
    pa_state->aligned = true;
    pa_state->verified = false;
    pa_state->blk_off = (uint8_t)((7 - lb) & 15);
    pa_state->lb_seen = pa_state->lb_bad = pa_state->lb_bad_windows = 0;
    SA_LOGF("[pa] SA aligned: LB at raw block %d (offset %d blocks)\n", lb, pa_state->blk_off);
}

/* LB window check: slots 56..63 must be blank while aligned. Returns false if commits
 * must be held back. */
static bool lb_monitor(pa_t *pa_state, int sa_idx_adj, bool is_blank){
    if(sa_idx_adj >= 56 && sa_idx_adj <= 63){
        pa_state->lb_seen++;
        if(!is_blank) pa_state->lb_bad++;
        if(sa_idx_adj == 63){
            if(pa_state->lb_seen >= 4){
                if(2 * pa_state->lb_bad >= pa_state->lb_seen){
                    if(pa_state->lb_bad_windows < 255) pa_state->lb_bad_windows++;
                    clear_candidates(pa_state);   /* data since the last good window is suspect */
                } else {
                    pa_state->lb_bad_windows = 0;
                    pa_state->verified = true;
                }
            }
            pa_state->lb_seen = pa_state->lb_bad = 0;
            if(pa_state->lb_bad_windows >= LB_BAD_REALIGN){
                pa_realign(pa_state, "LB block not blank", true);
                return false;
            }
        }
    }
    return pa_state->lb_bad_windows == 0;
}

/* ---------- Public API ---------- */

pa_t* pa_new(void){
    setvbuf(stderr, NULL, _IONBF, 0);  /* Logging immediately visible */
    build_par_inverse();

    pa_set_log(0);                     /* Logs default off */
    pa_t *pa_state = (pa_t*)calloc(1, sizeof(*pa_state));
    if(!pa_state) return NULL;
    for(int ch=0; ch<32; ch++){
        pa_state->ch[ch].pa_idx  = -1;
        }
    return pa_state;
}

void pa_free(pa_t *pa_state){
    if(pa_state) free(pa_state);
}

void pa_reset(pa_t *pa_state){
    if(!pa_state) return;
    pa_realign(pa_state, NULL, false);
    pa_state->collecting = 0;
    pa_state->idx_valid = false;
}

/* ---------- Bit-Push per A-Frame ---------- */
void pa_push_a_ssb(pa_t *pa_state, uint8_t ssb,
                   bool will_locked, uint8_t sa_pos_mod64, uint16_t code_at15, uint8_t octet_pos)
{
    if(!pa_state) return;

    /* 1) Without Williard lock: don't collect anything. The SA index stops counting
     * while the transmitter goes on, so the block alignment is void. */
    if(!will_locked){
        pa_state->collecting = 0;
        pa_state->idx_valid = false;
        /* Candidates stay: every committed frame had a valid Williard word of its own,
         * so nothing garbled got in while the decoder was losing the lock. */
        if(pa_state->aligned) pa_realign(pa_state, "Williard lock lost", false);
        return;
    }

    /* 2) Continue ongoing 48-bit collection */
    if(pa_state->collecting){
        int bit_idx   = 48 - pa_state->collecting;      /* 0..47 */
        int byte_idx = bit_idx >> 3;                /* 0..5  */
        int bit_pos   = 7 - (bit_idx & 7);           /* MSB-first */
        if(ssb & 1u) pa_state->pay[byte_idx] |= (uint8_t)(1u << bit_pos);

        if (--pa_state->collecting == 0 && pa_state->pending_ok){
            const bool is_blank = sa_is_blank_payload(pa_state->pay);
            const int raw = pa_state->pending_sa_idx & 127;
            const int blk = raw >> 3, oct = raw & 7;

            if(!pa_state->aligned){
                uint8_t bit = (uint8_t)(1u << oct);
                pa_state->seen_mask[blk] |= bit;
                if(is_blank) pa_state->blank_mask[blk] |= bit;
                else         pa_state->blank_mask[blk] &= (uint8_t)~bit;
                if(oct == 7) try_align(pa_state);
            } else {
                const int adj = (raw + 8 * pa_state->blk_off) & 127;
                if(lb_monitor(pa_state, adj, is_blank))
                    commit_sa_payload(pa_state, (uint8_t)adj);
            }
        }
    }

    /* 3) Only at SA boundary (pos==15) start new 48-bit collection */
    if(sa_pos_mod64 != 15) return;

    /* Octet of the SA frame whose Williard word just ended: SYNC1 marks encoder octet 0.
     * Counted here rather than taken from the decoder (its will_octet_pos is already
     * advanced to the next frame, except on the acquire frame). A SYNC1 at another octet
     * twice in a row means the frame grid moved: the block count is void. A single one
     * is taken for a bit error. */
    (void)octet_pos;
    if(code_at15 == WILLIARD_CODE_SYNC1){
        if(!pa_state->idx_valid){
            pa_state->sa_idx_0_127 = 0;
            pa_state->idx_valid = true;
            pa_state->octet_err = 0;
        } else if(pa_state->sa_idx_0_127 & 7u){
            if(++pa_state->octet_err >= 2){
                pa_realign(pa_state, "Williard octet jump", true);
                pa_state->sa_idx_0_127 = 0;
                pa_state->octet_err = 0;
            }
        } else {
            pa_state->octet_err = 0;
        }
    }
    if(!pa_state->idx_valid) return;    /* wait for the first SYNC1 */

    if(code_at15 != WILLIARD_CODE_SYNC1 && code_at15 != WILLIARD_CODE_SYNC2){
        /* Invalid word at boundary - keep counting, the payload is discarded below.
         * Rate-limit: max 1 warning per 1000 frames */
        static uint64_t sa_edge_warn_count = 0;
        if((++sa_edge_warn_count % 1000) == 0){
            SA_LOGF("[sa-edge] invalid will=%04X -> continue collection (warn #%llu)\n",
                    code_at15, (unsigned long long)sa_edge_warn_count);
        }
    }

    /* Prepare NEW 48-bit collection (the pos==15 bit is NOT part of it). Its payload is
     * only used if the frame's own Williard word is the one expected at its octet: a
     * wrong word means bit errors or a slipped frame grid, either way the payload is
     * suspect (the decoder only drops its lock after 24 such words). */
    memset(pa_state->pay, 0, sizeof(pa_state->pay));
    pa_state->collecting     = 48;
    pa_state->pending_sa_idx = pa_state->sa_idx_0_127;
    pa_state->pending_ok     = code_at15 == ((pa_state->sa_idx_0_127 & 7u) ? WILLIARD_CODE_SYNC2
                                                                          : WILLIARD_CODE_SYNC1);

    /* next SA-Index */
    pa_state->sa_idx_0_127 = (uint8_t)((pa_state->sa_idx_0_127 + 1) & 127u);
}

/* ---------- Getter ---------- */

int pa_get_name_utf8(const pa_t *pa_state, int ch, char *out, int maxlen){
    if(!out || maxlen <= 0) return 0;
    out[0] = '\0';
    if(!pa_state || ch < 0 || ch >= 32) return 0;
    if(!pa_state->ch[ch].name_valid) return 0;  /* only confirmed names */
    return name_codes_to_utf8(pa_state->ch[ch].name, out, maxlen);
}

int pa_get_pa(const pa_t *pa_state, int ch, int *type, int *music, int *mode)
{
    if(!pa_state || ch < 0 || ch >= 32) return 0;
    int16_t idx = pa_state->ch[ch].pa_idx;
    if(idx < 0) return 0;
    if(type)  *type  = (idx >> 4) & 0xF;
    if(music) *music = (idx >> 3) & 1;
    if(mode)  *mode  = (idx >> 1) & 3;  /* mode 0-3: need 2 bits */
    return 1;
}

void pa_get_status(const pa_t *pa_state, pa_status_t *out){
    if(!out) return;
    memset(out, 0, sizeof(*out));
    if(!pa_state) return;
    out->aligned = pa_state->aligned && pa_state->verified && pa_state->lb_bad_windows == 0;
    for(int ch=0; ch<32; ch++){
        if(pa_state->ch[ch].name_valid) out->names++;
        if(pa_state->ch[ch].pa_idx >= 0) out->attrs++;
    }
    out->realigns = pa_state->realigns;
}

/* Debug/Status: "cycle locked" = block alignment found, "offset ready" = confirmed by a
 * blank LB window since. */
bool pa_sa_cycle_locked(const pa_t *pa_state){ return pa_state && pa_state->aligned; }
bool pa_is_sa_offset_ready(const pa_t *pa_state){
    return pa_state && pa_state->aligned && pa_state->verified;
}
