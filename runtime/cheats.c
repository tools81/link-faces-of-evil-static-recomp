/* Cheats (F5 invincibility, F6 +100 rubies) and a small on-screen message.

   Game state used (offsets from the game's A5 data pointer):
     -10140.l  damage taken by sprite 0 (Link), in half hearts
     -10164.b  "Link is dead" flag
      -7599.b  Link touched deadly terrain (pit, water, lava)
     -11120.w  rubies
   Invincibility hooks two game functions (see HOOKS in tools/gen.py):
     hero_hit(damage)  - all enemy / projectile damage to Link
     kill_sprite(n)    - sprite deaths; n == 0 is Link */
#include "recomp.h"
#include "runtime.h"
#include <stdarg.h>
#include <string.h>

#define LINK_DAMAGE  (-10140)
#define LINK_DEAD    (-10164)
#define LINK_TERRAIN (-7599)
#define RUBIES       (-11120)
#define RUBIES_MAX   999

static int invincible;

static int game_ready(void) { return a5_expected != 0 && A(5) == a5_expected; }

/* return to the hooked function's caller (as its rts would) */
static uint32_t skip_function(void) {
    uint32_t ra = RL(A(7));
    A(7) += 4;
    return ra;
}

uint32_t hook_hero_hit(void) {
    if (!invincible) return 0;
    return skip_function();
}

uint32_t hook_kill_sprite(void) {
    if (!invincible || RL(A(7) + 4) != 0) return 0;
    WL(a5_expected + LINK_DAMAGE, 0);
    WB(a5_expected + LINK_DEAD, 0);
    WB(a5_expected + LINK_TERRAIN, 0);
    return skip_function();
}

/* ---------------------------------------------------------------- OSD */
static char osd_text[64];
static int osd_fields;

void osd_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(osd_text, sizeof osd_text, fmt, ap);
    va_end(ap);
    osd_fields = 150;   /* about 2.5 seconds */
    LOG("%s", osd_text);
}

/* 5x7 font: rows top to bottom, bit 4 = leftmost column */
static const uint8_t *glyph(char c) {
    static const uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31},
        {31, 2, 4, 2, 1, 17, 14}, {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14},
        {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8}, {14, 17, 17, 14, 17, 17, 14},
        {14, 17, 17, 15, 1, 2, 12}};
    static const uint8_t letters[26][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
        {28, 18, 17, 17, 17, 18, 28}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
        {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
        {7, 2, 2, 2, 2, 18, 12}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
        {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
        {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
        {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4}, {17, 17, 17, 17, 17, 17, 14},
        {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
        {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31}};
    static const uint8_t plus[7] = {0, 4, 4, 31, 4, 4, 0};
    static const uint8_t colon[7] = {0, 12, 12, 0, 12, 12, 0};
    static const uint8_t blank[7] = {0};
    if (c >= '0' && c <= '9') return digits[c - '0'];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c == '+') return plus;
    if (c == ':') return colon;
    return blank;
}

/* draw the message onto the 768x240 frame (pixels are 2:1 wide) */
void osd_draw(uint32_t *fb, int w, int h) {
    if (osd_fields <= 0) return;
    osd_fields--;
    const int sx = 4, sy = 2;            /* scale: double-res pixels are half width */
    int n = (int)strlen(osd_text);
    int tw = n * 6 * sx, th = 7 * sy;
    int x0 = 40, y0 = 20;
    for (int y = y0 - 4; y < y0 + th + 4 && y < h; y++)
        for (int x = x0 - 8; x < x0 + tw + 8 && x < w; x++) {
            uint32_t p = fb[y * w + x];
            fb[y * w + x] = (p >> 2) & 0x3F3F3F;   /* darkened backdrop */
        }
    for (int i = 0; i < n; i++) {
        const uint8_t *g = glyph(osd_text[i]);
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (16 >> c))) continue;
                for (int yy = 0; yy < sy; yy++)
                    for (int xx = 0; xx < sx; xx++) {
                        int x = x0 + (i * 6 + c) * sx + xx, y = y0 + r * sy + yy;
                        if (x < w && y < h) fb[y * w + x] = 0xFFE860;
                    }
            }
    }
}

/* ---------------------------------------------------------------- hotkeys */
void cheat_toggle_invincible(void) {
    invincible = !invincible;
    osd_message("INVINCIBLE %s", invincible ? "ON" : "OFF");
}

void cheat_add_rubies(int n) {
    if (!game_ready()) { osd_message("NOT IN GAME"); return; }
    uint32_t a = a5_expected + RUBIES;
    uint32_t r = RW(a) + (uint32_t)n;
    if (r > RUBIES_MAX) r = RUBIES_MAX;
    WW(a, r);
    osd_message("RUBIES: %u", r);
}
