/* Machine snapshots (save states) and run-ahead. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *p;
    size_t n, cap, rd;
    int load;           /* 0 = saving into the buffer, 1 = loading from it */
} SBuf;

/* serialize a block of memory in either direction */
void sio(SBuf *b, void *v, size_t n);
#define SIO(b, var) sio((b), &(var), sizeof(var))

/* per-module state (implemented next to each module's statics) */
void os9_state_io(SBuf *b);
void cdfm_state_io(SBuf *b);
void video_state_io(SBuf *b);
void audio_state_io(SBuf *b);
void interp_state_io(SBuf *b);
void spr_state_io(SBuf *b);

/* ---- smooth motion (sprinterp.c) */
extern int g_smooth_motion;
void spr_call_begin(uint32_t ret, int32_t idx, int32_t x, int32_t y, uint32_t buf, uint32_t tab);
void spr_call_end(void);
int  spr_capturing(void);
void spr_write(uint32_t a, int n, uint32_t v);
void spr_vsr(uint32_t vsr);
void spr_field_begin(int64_t field, int compose);
void spr_field_end(void);
uint32_t mem_high_water(void);
const char *os9_state_unsafe(void);
void os9_close_host_files(void);

/* ---- user save states (F2 save, F4 load, F3 slot) */
void savestate_request(int load);       /* performed at the next safe point */
void savestate_cycle_slot(void);
void savestate_service(void);           /* called by the dispatcher at top level */
int  savestate_slot(void);

void state_save(SBuf *b);
void state_load(SBuf *b);

/* ---- run-ahead */
extern int g_runahead;          /* fields to run ahead (0 = off) */
extern int g_ra_active;         /* currently emulating ahead (silent) */
extern int g_cpu_depth;         /* nesting of cpu_run_until (child processes) */
extern int g_sched_nested;      /* inside a blocking syscall's event loop */

int  runahead_field_mode(void); /* called by video_field: see video.c */
void runahead_present_done(void);
void runahead_abort(const char *why);
void runahead_top_level(void);  /* called by the dispatcher loop after events */
void runahead_tick_real_field(void);
