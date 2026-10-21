/* Smooth motion: 60 fps sprite interpolation.

   The game updates the action at 16 Hz.  Every update it draws all sprites
   (with "compiled sprite" routines, which run in our interpreter) into the
   hidden one of two frame buffers and then flips the display to it.

   Capture: while a sprite routine runs we record every byte it writes into
   the frame buffer (address, old value, new value), grouped per buffer and
   per draw, with the sprite's identity (call site + sprite index) and
   position.

   Display: during the fields that follow a flip, before the frame is
   decoded we temporarily (1) undo the sprite pixels of the shown frame,
   revealing the background, and (2) redraw each sprite at a position
   interpolated between the previous and the current update.  After the field
   has been composed every byte is put back, so game memory is never
   changed.  Sprites without a match, sprites that jumped, and frames where
   the camera scrolled are shown exactly as drawn. */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <stdlib.h>
#include <string.h>

int g_smooth_motion;

#define SPR_BUFS   4
#define SPR_DRAWS  128
#define SPR_WRITES 49152
#define SPR_MAXJUMP 40

/* game data: playfield scroll position (drop_sprite subtracts it) */
#define SCROLL_X (-9364)
#define SCROLL_Y (-9368)

typedef struct { uint32_t addr; uint8_t oldv, newv; } SprWrite;

typedef struct {
    uint32_t ret;          /* call site of the sprite routine */
    int32_t idx;           /* sprite index (d7 at the call) */
    int32_t x, y;          /* position passed to the routine */
    int32_t w0, wn;        /* its writes: [w0, w0 + wn) */
} SprDraw;

typedef struct {
    int used, sealed, overflow;
    uint32_t buf, range, stride;
    int32_t scroll_x, scroll_y;
    int64_t shown_at;      /* field when it was first displayed */
    int ndraw, nw;
    SprDraw d[SPR_DRAWS];
    SprWrite w[SPR_WRITES];
} SprFrame;

static SprFrame fr[SPR_BUFS];
static int cur_disp = -1, prev_disp = -1;
static int capturing = -1;          /* frame being drawn into, or -1 */

/* patch bookkeeping for the field being composed */
static int composing, patched;
static int64_t field_now;
static SprWrite *undo;              /* addr + original byte */
static int nundo, capundo;

/* the buffer containing address a (the closest buffer start at or below it) */
static int find_buf(uint32_t a) {
    int best = -1;
    for (int i = 0; i < SPR_BUFS; i++)
        if (fr[i].used && a >= fr[i].buf && a < fr[i].buf + fr[i].range && (best < 0 || fr[i].buf > fr[best].buf))
            best = i;
    return best;
}

/* ---------------------------------------------------------------- capture */
int spr_capturing(void) { return capturing >= 0; }

void spr_call_begin(uint32_t ret, int32_t idx, int32_t x, int32_t y, uint32_t buf, uint32_t tab) {
    capturing = -1;
    if (!g_smooth_motion || buf < 0x1000 || tab < 0x1000) return;
    uint32_t stride = RL(tab + 4) - RL(tab);
    if (RL(tab) != 0 || stride < 64 || stride > 2048) return;   /* only linear frame buffers */
    int f = -1;
    for (int i = 0; i < SPR_BUFS; i++) if (fr[i].used && fr[i].buf == buf) f = i;
    if (f < 0) {
        for (int i = 0; i < SPR_BUFS; i++) if (!fr[i].used) { f = i; break; }
        if (f < 0) return;
        memset(&fr[f], 0, sizeof(SprFrame) - sizeof fr[f].w);
        fr[f].used = 1;
        fr[f].buf = buf;
        fr[f].stride = stride;
        fr[f].range = stride * 240;
    }
    SprFrame *F = &fr[f];
    if (F->sealed) {                /* the game starts a new picture in this buffer */
        F->sealed = 0;
        F->overflow = 0;
        F->ndraw = F->nw = 0;
        F->shown_at = 0;
    }
    if (F->ndraw == 0 && a5_expected) {
        F->scroll_x = (int32_t)RL(a5_expected + SCROLL_X);
        F->scroll_y = (int32_t)RL(a5_expected + SCROLL_Y);
    }
    if (F->ndraw >= SPR_DRAWS) { F->overflow = 1; return; }
    SprDraw *d = &F->d[F->ndraw++];
    d->ret = ret; d->idx = idx; d->x = x; d->y = y;
    d->w0 = F->nw; d->wn = 0;
    capturing = f;
}

void spr_call_end(void) { capturing = -1; }

/* called by the interpreter before it writes memory */
void spr_write(uint32_t a, int n, uint32_t v) {
    SprFrame *F = &fr[capturing];
    for (int k = 0; k < n; k++) {
        uint32_t x = (a + k) & MEM_MASK;
        if (x < F->buf || x >= F->buf + F->range) continue;
        if (F->nw >= SPR_WRITES) { F->overflow = 1; return; }
        SprWrite *w = &F->w[F->nw++];
        w->addr = x;
        w->oldv = RAM[x];
        w->newv = (uint8_t)(v >> (8 * (n - 1 - k)));
        F->d[F->ndraw - 1].wn++;
    }
}

/* ---------------------------------------------------------------- display */
static void set_byte(uint32_t a, uint8_t v) {
    if (nundo >= capundo) {
        capundo = capundo ? capundo * 2 : 65536;
        undo = (SprWrite *)realloc(undo, sizeof(SprWrite) * (size_t)capundo);
    }
    undo[nundo].addr = a;
    undo[nundo].oldv = RAM[a];
    nundo++;
    RAM[a] = v;
}

static int ilerp(int a, int b, double t) {
    double v = a + (b - a) * t;
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

static void apply_patch(int c) {
    SprFrame *C = &fr[c];
    patched = 1;           /* evaluated once per field */
    if (prev_disp < 0 || prev_disp == c || C->overflow) return;
    SprFrame *P = &fr[prev_disp];
    if (!P->used || P->overflow || P->stride != C->stride) return;
    if (P->scroll_x != C->scroll_x || P->scroll_y != C->scroll_y) return;   /* camera moved */
    int64_t period = C->shown_at - P->shown_at;
    if (period < 2) period = 2;
    if (period > 8) period = 8;
    double t = (double)(field_now - C->shown_at + 1) / (double)period;
    if (t >= 1.0) return;
    /* 1: remove this frame's sprites (reverse order restores the background) */
    for (int i = C->nw - 1; i >= 0; i--) set_byte(C->w[i].addr, C->w[i].oldv);
    /* 2: draw them again at interpolated positions */
    int used[SPR_DRAWS] = {0};
    for (int i = 0; i < C->ndraw; i++) {
        SprDraw *d = &C->d[i];
        int dx = 0, dy = 0;
        for (int j = 0; j < P->ndraw; j++) {
            SprDraw *p = &P->d[j];
            if (used[j] || p->ret != d->ret || p->idx != d->idx) continue;
            used[j] = 1;
            if (abs(d->x - p->x) <= SPR_MAXJUMP && abs(d->y - p->y) <= SPR_MAXJUMP) {
                dx = ilerp(p->x, d->x, t) - d->x;
                dy = ilerp(p->y, d->y, t) - d->y;
            }
            break;
        }
        int32_t shift = dy * (int32_t)C->stride + dx;
        for (int k = 0; k < d->wn; k++) {
            SprWrite *w = &C->w[d->w0 + k];
            uint32_t a = w->addr + (uint32_t)shift;
            if (a < C->buf || a >= C->buf + C->range) continue;
            set_byte(a, w->newv);
        }
    }
    patched = 1;
}

/* the decoder starts a line at address vsr */
void spr_vsr(uint32_t vsr) {
    if (!g_smooth_motion) return;
    int f = find_buf(vsr);
    if (f < 0) return;
    SprFrame *F = &fr[f];
    if (f != cur_disp || !F->sealed) {
        /* a flip: the previous picture is the one to interpolate from; a buffer
           redrawn while on screen (single-buffered screens) has no usable one */
        prev_disp = f != cur_disp ? cur_disp : -1;
        cur_disp = f;
        F->sealed = 1;
        F->shown_at = field_now;
    }
    if (composing && !patched) apply_patch(f);
}

void spr_field_begin(int64_t field, int compose) {
    field_now = field;
    composing = compose && g_smooth_motion;
    patched = 0;
    nundo = 0;
}

void spr_field_end(void) {
    for (int i = nundo - 1; i >= 0; i--) RAM[undo[i].addr] = undo[i].oldv;
    nundo = 0;
    composing = patched = 0;
}

/* ---------------------------------------------------------------- save state */
void spr_state_io(SBuf *b) {
    SIO(b, cur_disp);
    SIO(b, prev_disp);
    for (int i = 0; i < SPR_BUFS; i++) {
        SprFrame *F = &fr[i];
        sio(b, F, offsetof(SprFrame, w));
        if (F->used) sio(b, F->w, sizeof(SprWrite) * (size_t)F->nw);
    }
}
