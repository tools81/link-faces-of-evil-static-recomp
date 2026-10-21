/* CD File Manager HLE: file reads and real-time play (SS_Play). */
#include "recomp.h"
#include "runtime.h"
#include "state.h"
#include <stdlib.h>
#include <string.h>

#define E_EOF   0xD3
#define E_BPNUM 0xC9
#define E_DEVBSY 0xFA
#define E_UNKSVC 0xD0

typedef struct {
    int used;
    const DiscFile *f;
    uint32_t pos;          /* byte position (2048 per sector) */
} CdHandle;

static CdHandle hs[16];

/* active play */
static struct {
    int active;
    int h;
    uint32_t pcb;
    uint32_t sector;       /* index within file */
    uint32_t nsec;
    int pid;               /* process that started the play */
} play;

void cdfm_init(void) { memset(hs, 0, sizeof hs); memset(&play, 0, sizeof play); }

int cdfm_open(const char *name, int mode) {
    (void)mode;
    const DiscFile *f = disc_find(name);
    if (!f) { LOG("cdfm: file '%s' not found", name); return -1; }
    for (int i = 0; i < 16; i++) {
        if (!hs[i].used) {
            hs[i].used = 1; hs[i].f = f; hs[i].pos = 0;
            return i;
        }
    }
    return -1;
}

void cdfm_close(int h) {
    if (play.active && play.h == h) play.active = 0;
    hs[h].used = 0;
}

uint32_t cdfm_getpos(int h) { return hs[h].pos; }
uint32_t cdfm_size(int h) { return hs[h].f->size; }
int cdfm_seek(int h, uint32_t pos) { hs[h].pos = pos; return 0; }
int cdfm_busy(void) { return play.active; }

int cdfm_read(int h, uint32_t buf, uint32_t n, uint32_t *got) {
    CdHandle *c = &hs[h];
    const DiscFile *f = c->f;
    if (c->pos >= f->size) return E_EOF;
    if (n > f->size - c->pos) n = f->size - c->pos;
    uint8_t raw[2352];
    uint32_t done = 0;
    while (done < n) {
        uint32_t sec = c->pos / 2048, off = c->pos % 2048;
        disc_read_raw(f->lsn + sec, raw);
        uint32_t k = 2048 - off;
        if (k > n - done) k = n - done;
        memcpy(RAM + ((buf + done) & MEM_MASK), raw + 24 + off, k);
        done += k;
        c->pos += k;
    }
    *got = done;
    return 0;
}

/* ---------------------------------------------------------------- play */
static void play_finish(int err) {
    uint32_t pcb = play.pcb;
    uint16_t st = RW(pcb);
    st |= 0x0100;
    if (err) { st |= 0x8000; }
    WW(pcb, st);
    uint16_t sig = RW(pcb + 2);
    if (err) WW(pcb + 2, (uint32_t)err);
    play.active = 0;
    TRACE("play finished (err %d), sig %u", err, sig);
    if (sig) sig_send_pid(play.pid, sig);
}

static void deliver_to_pcl(uint32_t cil, int chan, const uint8_t *raw, uint32_t size, uint8_t submode, uint8_t coding) {
    uint32_t pcl = RL(cil + chan * 4);
    if (!pcl) return;
    uint8_t ctrl = RB(pcl);
    if (ctrl & 1) {
        LOG("play overrun on PCL %06x (chan %d)", pcl, chan);
        play_finish(0xF4);
        return;
    }
    uint32_t buf = RL(pcl + 10);
    uint32_t bufsz = RL(pcl + 14);
    uint32_t cnt = RL(pcl + 24);
    WB(pcl + 2, submode);
    WB(pcl + 3, coding);
    if (buf) {
        memcpy(RAM + ((buf + cnt) & MEM_MASK), raw + 24, size);
        cnt += size;
        WL(pcl + 24, cnt);
    }
    /* buffer full? (size counted in sectors) */
    uint32_t nsec = size ? cnt / size : 0;
    if (!buf || nsec >= bufsz) {
        WB(pcl, ctrl | 1);
        WL(cil + chan * 4, RL(pcl + 6));     /* CIL -> next PCL */
        uint16_t sig = RW(pcl + 4);
        TRACE("PCL %06x full (%u sectors), sig %u", pcl, nsec, sig);
        if (sig) sig_send_pid(play.pid, sig);
    }
}

void cdfm_sector_tick(void) {
    if (!play.active) return;
    if (play.sector >= play.nsec) { play_finish(0); return; }
    CdHandle *c = &hs[play.h];
    uint8_t raw[2352];
    disc_read_raw(c->f->lsn + play.sector, raw);
    play.sector++;
    c->pos = play.sector * 2048;
    uint32_t pcb = play.pcb;
    uint8_t fnum = raw[16], chan = raw[17] & 31, submode = raw[18], coding = raw[19];
    if (log_level >= 3) TRACE("  sector %u fn %u ch %u sm %02x ci %02x", play.sector - 1, fnum, chan, submode, coding);
    if (fnum != c->f->fnum) return;
    uint32_t chanmask = RL(pcb + 8);
    int is_v = submode & 2, is_a = submode & 4, is_d = submode & 8;
    if (chanmask & (1u << chan)) {
        uint16_t st = (uint16_t)(RW(pcb) & 0x0100);
        st |= submode & 1 ? 0x01 : 0;
        st |= is_v ? 0x02 : 0;
        st |= is_a ? 0x04 : 0;
        st |= is_d ? 0x08 : 0;
        st |= submode & 0x10 ? 0x10 : 0;
        st |= submode & 0x20 ? 0x20 : 0;
        st |= submode & 0x40 ? 0x40 : 0;
        st |= submode & 0x80 ? 0x80 : 0;
        WW(pcb, st);
        uint32_t size = (submode & 0x20) ? (is_a ? 2304 : 2324) : 2048;
        uint16_t achan = RW(pcb + 12);
        if (!(is_v || is_a || is_d)) {
            /* empty sector: only its EOR/trigger/EOF bits matter */
        } else if (is_a && chan < 16 && (achan & (1u << chan))) {
            audio_feed_sector(raw);
        } else {
            uint32_t cil = is_v ? RL(pcb + 14) : is_a ? RL(pcb + 18) : RL(pcb + 22);
            if (cil && !(is_a && chan >= 16)) deliver_to_pcl(cil, chan, raw, size, submode, coding);
            if (!play.active) return;
        }
        int ev = submode & 0x11;      /* EOR or trigger */
        if (submode & 1) {
            uint32_t rec = RL(pcb + 4);
            if (rec) { rec--; WL(pcb + 4, rec); }
            if (rec == 0) { play_finish(0); return; }
        }
        if (ev) {
            uint16_t sig = RW(pcb + 2);
            if (sig) sig_send_pid(play.pid, sig);
        }
        if (RL(pcb + 4) == 0) { play_finish(0); return; }
    }
    if (submode & 0x80) { play_finish(0); return; }
}

int cdfm_setstat(int h, uint32_t code) {
    switch (code) {
    case 0x2D: { /* SS_Play */
        if (play.active) {
            char bt[256]; int k = 0;
            for (int i = 0; i < 40 && k < 200; i++) {
                uint32_t v = RL(A(7) + i * 2);
                if (v >= 0x100048 && v < 0x11e000 && !(v & 1)) k += sprintf(bt + k, " %06x", v);
            }
            LOG("SS_Play while busy (old pcb %06x %s, new pcb %06x %s):%s", play.pcb, hs[play.h].f->name,
                A(0), hs[h].f->name, bt);
        }
        play.active = 1;
        play.h = h;
        play.pcb = A(0);
        play.pid = os9_cur_pid();
        play.sector = hs[h].pos / 2048;
        play.nsec = (hs[h].f->size + 2047) / 2048;
        WW(play.pcb, 0);
        TRACE("SS_Play %s pcb %06x from sector %u chan %08x achan %04x rec %u",
              hs[h].f->name, play.pcb, play.sector, RL(play.pcb + 8), RW(play.pcb + 12), RL(play.pcb + 4));
        if (log_level >= 2) {
            const char *nm[3] = {"video", "audio", "data"};
            for (int t = 0; t < 3; t++) {
                uint32_t cil = RL(play.pcb + 14 + t * 4);
                if (!cil) continue;
                for (int c = 0; c < (t == 1 ? 16 : 32); c++) {
                    uint32_t pcl = RL(cil + c * 4);
                    if (pcl) TRACE("  %s cil %06x chan %d -> pcl %06x ctrl %02x buf %06x sz %u cnt %u nxt %06x sig %u",
                                   nm[t], cil, c, pcl, RB(pcl), RL(pcl + 10), RL(pcl + 14), RL(pcl + 24), RL(pcl + 6), RW(pcl + 4));
                }
            }
        }
        return 0;
    }
    case 0x31: /* SS_Abort */
        if (play.active) {
            TRACE("SS_Abort");
            play.active = 0;
        }
        return 0;
    case 0x30: /* SS_Seek (async) */
        hs[h].pos = D(2);
        return 0;
    case 0x33: /* SS_Pause */
    case 0x36: /* SS_Stop */
    case 0x37: /* SS_Cont */
    case 0x38: /* SS_Disable */
    case 0x39: /* SS_Enable */
        LOG("cdfm setstat %x ignored", code);
        return 0;
    }
    if (code >= 0x3B && code <= 0x3D) return audio_setstat(code);
    LOG("cdfm: unknown setstat %x", code);
    return E_UNKSVC;
}

int cdfm_getstat(int h, uint32_t code) {
    (void)h;
    if (code >= 0x3B && code <= 0x3D) return audio_getstat(code);
    LOG("cdfm: unknown getstat %x", code);
    return E_UNKSVC;
}

void cdfm_state_io(SBuf *b) {
    /* disc file pointers are stored as indexes so states survive a restart */
    for (int i = 0; i < 16; i++) {
        int fi = (!b->load && hs[i].used) ? (int)(hs[i].f - disc_file(0)) : -1;
        SIO(b, hs[i].used);
        SIO(b, hs[i].pos);
        SIO(b, fi);
        if (b->load) hs[i].f = fi >= 0 ? disc_file(fi) : NULL;
    }
    SIO(b, play);
}
