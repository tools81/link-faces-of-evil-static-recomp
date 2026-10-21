/* Save states and run-ahead.

   Run-ahead (input-lag reduction, as popularised by RetroArch): after each
   real field the whole machine is snapshotted, the emulation runs a few
   fields ahead silently with the current input, the last of those fields is
   shown, and the snapshot is restored.  The game's own reaction delay (its
   16 Hz update tick plus drawing) is therefore hidden from the player.
   Audio is produced only by the real timeline.

   Everything is deterministic, so a restore puts the machine back exactly;
   tools: compare a --runahead N headless run with a normal run (the audio
   must be bit-identical). */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

void sio(SBuf *b, void *v, size_t n) {
    if (b->load) {
        if (b->rd + n > b->n) fatal("save state truncated");
        memcpy(v, b->p + b->rd, n);
        b->rd += n;
    } else {
        if (b->n + n > b->cap) {
            size_t c = b->cap ? b->cap : 1 << 20;
            while (c < b->n + n) c *= 2;
            b->p = (uint8_t *)realloc(b->p, c);
            b->cap = c;
        }
        memcpy(b->p + b->n, v, n);
        b->n += n;
    }
}

static void machine_io(SBuf *b) {
    SIO(b, m68k);
    SIO(b, a5_expected);
    /* RAM: everything below the heap's high-water mark (modules, data,
       stacks, display memory, sound maps) */
    uint32_t top = mem_high_water() + 0x10000;
    if (top > MEM_SIZE) top = MEM_SIZE;
    SIO(b, top);
    sio(b, RAM, top);
    os9_state_io(b);
    cdfm_state_io(b);
    video_state_io(b);
    audio_state_io(b);
    interp_state_io(b);
    spr_state_io(b);
}

void state_save(SBuf *b) {
    b->load = 0;
    b->n = 0;
    machine_io(b);
}

void state_load(SBuf *b) {
    b->load = 1;
    b->rd = 0;
    machine_io(b);
}

/* ---------------------------------------------------------------- run-ahead */
int g_runahead;
int g_ra_active;
int g_cpu_depth;
int g_sched_nested;

static int ra_left;           /* fields still to emulate ahead */
static int ra_pending;        /* a real field ended and wants run-ahead */
static int ra_presented;
static int ra_suspend;        /* real fields to wait before trying again */
static jmp_buf ra_jmp;
static SBuf snap;

/* Decide how video_field handles this field:
     0  real field, normal: compose and present
     1  real field, deferred: run-ahead will present
     2  run-ahead, intermediate: no picture
     3  run-ahead, last field: compose and present, then stop */
int runahead_field_mode(void) {
    if (g_ra_active) return ra_left <= 1 ? 3 : 2;
    if (g_runahead > 0 && !ra_suspend && g_cpu_depth == 1 && !g_sched_nested) {
        ra_pending = 1;
        return 1;
    }
    return 0;
}

void runahead_tick_real_field(void) {
    if (ra_suspend > 0) ra_suspend--;
}

/* called by video_field after each run-ahead field */
void runahead_present_done(void) {
    if (--ra_left > 0) return;
    ra_presented = 1;
    longjmp(ra_jmp, 1);
}

/* something with side effects outside the machine (NVRAM files, exiting)
   happened while running ahead: abandon this run-ahead and pause it for a
   moment; the real timeline will do it for real */
void runahead_abort(const char *why) {
    if (!g_ra_active) return;
    (void)why;
    ra_suspend = 120;
    ra_presented = 0;
    longjmp(ra_jmp, 1);
}

void runahead_top_level(void) {
    if (!ra_pending) return;
    ra_pending = 0;
    host_poll();          /* freshest input: used by the frames emulated ahead and the next real field */
    state_save(&snap);
    int depth = g_cpu_depth, nested = g_sched_nested;
    ra_left = g_runahead;
    ra_presented = 0;
    g_ra_active = 1;
    if (setjmp(ra_jmp) == 0) {
        static volatile int never;
        cpu_run_until(&never);
    }
    g_ra_active = 0;
    g_cpu_depth = depth;
    g_sched_nested = nested;
    state_load(&snap);
    if (!ra_presented)
        host_present(video_fb, video_w, video_h);   /* show the previous picture again */
}

/* ---------------------------------------------------------------- save states */
#define STATE_MAGIC   "LFOE-SS1"
#define STATE_VERSION 1

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t layout;      /* identifies the build: runtime layout + recompiled code */
    uint32_t size;        /* payload bytes */
    uint32_t checksum;    /* FNV-1a of the payload */
} StateHeader;

static int ss_slot = 1;
static int ss_request;    /* 0 none, 1 save, 2 load */

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t layout_id(void) {
    uint32_t v[5] = {STATE_VERSION, (uint32_t)sizeof(M68K), (uint32_t)recomp_num_entries,
                     recomp_code_lo, recomp_code_hi};
    return fnv1a((const uint8_t *)v, sizeof v);
}

static void slot_path(char *out, size_t n, int slot) {
    snprintf(out, n, "%s/state_%d.lfs", g_data_dir, slot);
}

int savestate_slot(void) { return ss_slot; }

void savestate_cycle_slot(void) {
    ss_slot = ss_slot % 10 + 1;
    char p[600];
    slot_path(p, sizeof p, ss_slot);
    FILE *f = fopen(p, "rb");
    osd_message("STATE SLOT %d%s", ss_slot, f ? "" : " EMPTY");
    if (f) fclose(f);
}

void savestate_request(int load) { ss_request = load ? 2 : 1; }

/* --load-state N: load slot N as soon as the game reaches a safe point */
void savestate_load_at_start(int slot) {
    if (slot < 1 || slot > 10) return;
    ss_slot = slot;
    ss_request = 2;
}

static void do_save(void) {
    const char *why = os9_state_unsafe();
    if (why) { osd_message("CANNOT SAVE: %s", why); return; }
    SBuf b = {0};
    state_save(&b);
    StateHeader h;
    memcpy(h.magic, STATE_MAGIC, 8);
    h.version = STATE_VERSION;
    h.layout = layout_id();
    h.size = (uint32_t)b.n;
    h.checksum = fnv1a(b.p, b.n);
    char p[600];
    slot_path(p, sizeof p, ss_slot);
    FILE *f = fopen(p, "wb");
    int ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(b.p, 1, b.n, f) == b.n;
    if (f) fclose(f);
    free(b.p);
    osd_message(ok ? "STATE %d SAVED" : "STATE %d: WRITE FAILED", ss_slot);
}

static void do_load(void) {
    char p[600];
    slot_path(p, sizeof p, ss_slot);
    FILE *f = fopen(p, "rb");
    if (!f) { osd_message("STATE %d EMPTY", ss_slot); return; }
    StateHeader h;
    SBuf b = {0};
    int ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, STATE_MAGIC, 8);
    if (ok && (h.version != STATE_VERSION || h.layout != layout_id())) {
        fclose(f);
        osd_message("STATE %d IS FROM ANOTHER VERSION", ss_slot);
        return;
    }
    if (ok) {
        b.p = (uint8_t *)malloc(h.size ? h.size : 1);
        b.n = b.cap = h.size;
        ok = fread(b.p, 1, h.size, f) == h.size && fnv1a(b.p, b.n) == h.checksum;
    }
    fclose(f);
    if (!ok) { free(b.p); osd_message("STATE %d IS DAMAGED", ss_slot); return; }
    os9_close_host_files();
    state_load(&b);
    free(b.p);
    osd_message("STATE %d LOADED", ss_slot);
}

/* save/load only between frames of the main program: never inside a
   blocking system call, a child process or run-ahead */
void savestate_service(void) {
    if (!ss_request || g_ra_active || g_cpu_depth != 1 || g_sched_nested) return;
    int r = ss_request;
    ss_request = 0;
    if (r == 1) do_save();
    else do_load();
}
