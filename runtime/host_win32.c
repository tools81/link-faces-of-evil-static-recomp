/* Win32 host platform: window, input, audio, timing. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "render.h"
#include "state.h"

static int fullscreen;
static void save_settings(void);

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

static HWND hwnd;
static LARGE_INTEGER qpf, qp0;
static int keys[256];
int g_headless;
static int win_scale = 3;

static WINDOWPLACEMENT saved_wp = {sizeof(WINDOWPLACEMENT)};

static void toggle_fullscreen(void) {
    DWORD style = GetWindowLongA(hwnd, GWL_STYLE);
    if (!fullscreen) {
        MONITORINFO mi = {sizeof mi};
        if (GetWindowPlacement(hwnd, &saved_wp) &&
            GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            ShowCursor(FALSE);
        }
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &saved_wp);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ShowCursor(TRUE);
    }
    fullscreen = !fullscreen;
    save_settings();
}

/* ---------------------------------------------------------------- video settings */
static int use_gpu = 1, start_fullscreen, win_w, win_h, runahead_from_cli, smooth_from_cli;
static int gpu_ok;

#ifdef HD_BUILD
#define EDITION " HD"
static void settings_path(char *out, size_t n) { snprintf(out, n, "%s/settings_hd.ini", g_data_dir); }
#else
#define EDITION ""
static void settings_path(char *out, size_t n) { snprintf(out, n, "%s/settings.ini", g_data_dir); }
#endif

static void load_settings(void) {
    char p[600];
    settings_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "aspect=%d", &v) == 1) render_settings.aspect = v % RENDER_ASPECTS;
        if (sscanf(line, "filter=%d", &v) == 1) render_settings.filter = v % RENDER_FILTERS;
        if (sscanf(line, "fullscreen=%d", &v) == 1) start_fullscreen = v;
        if (sscanf(line, "runahead=%d", &v) == 1 && !runahead_from_cli) g_runahead = v < 0 ? 0 : v > 6 ? 6 : v;
        if (sscanf(line, "smooth=%d", &v) == 1 && !smooth_from_cli) g_smooth_motion = v != 0;
    }
    fclose(f);
}

static void save_settings(void) {
    char p[600];
    settings_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "aspect=%d\nfilter=%d\nfullscreen=%d\nrunahead=%d\nsmooth=%d\n", render_settings.aspect,
            render_settings.filter, fullscreen, g_runahead, g_smooth_motion);
    fclose(f);
}

static void update_title(void) {
    char t[256];
    if (gpu_ok)
        snprintf(t, sizeof t, "Link: The Faces of Evil" EDITION "  [%s, %s]  F9 aspect, F10 filter, F11 fullscreen",
                 render_aspect_name(render_settings.aspect), render_filter_name(render_settings.filter));
    else
        snprintf(t, sizeof t, "Link: The Faces of Evil  (GDI)");
    SetWindowTextA(hwnd, t);
}

static void settings_changed(void) {
    update_title();
    save_settings();
}

/* command-line overrides: --aspect N, --filter N, --fullscreen, --windowed, --gdi, --size WxH */
void host_video_option(const char *opt, const char *val) {
    if (!strcmp(opt, "--aspect")) render_settings.aspect = atoi(val) % RENDER_ASPECTS;
    else if (!strcmp(opt, "--filter")) render_settings.filter = atoi(val) % RENDER_FILTERS;
    else if (!strcmp(opt, "--fullscreen")) start_fullscreen = 1;
    else if (!strcmp(opt, "--windowed")) start_fullscreen = 0;
    else if (!strcmp(opt, "--gdi")) use_gpu = 0;
    else if (!strcmp(opt, "--runahead") && val) { g_runahead = atoi(val); runahead_from_cli = 1; }
    else if (!strcmp(opt, "--smooth") && val) { g_smooth_motion = atoi(val) != 0; smooth_from_cli = 1; }
    else if (!strcmp(opt, "--size") && val) sscanf(val, "%dx%d", &win_w, &win_h);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: input.quit = 1; return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        keys[w & 255] = 1;
        if (w == VK_ESCAPE) input.quit = 1;
        if (w == VK_F2) savestate_request(0);
        if (w == VK_F3) savestate_cycle_slot();
        if (w == VK_F4) savestate_request(1);
        if (w == VK_F5) cheat_toggle_invincible();
        if (w == VK_F6) cheat_add_rubies(100);
        if (w == VK_F7) {
            g_runahead = (g_runahead + 1) % 7;
            osd_message(g_runahead ? "RUN-AHEAD %d" : "RUN-AHEAD OFF", g_runahead);
            settings_changed();
        }
        if (w == VK_F8) {
            g_smooth_motion = !g_smooth_motion;
            osd_message("SMOOTH MOTION %s", g_smooth_motion ? "ON" : "OFF");
            settings_changed();
        }
        if (w == VK_F9) { render_settings.aspect = (render_settings.aspect + 1) % RENDER_ASPECTS; settings_changed(); }
        if (w == VK_F10) { render_settings.filter = (render_settings.filter + 1) % RENDER_FILTERS; settings_changed(); }
        if (w == VK_F11 || (w == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) {
            toggle_fullscreen();
            keys[VK_RETURN] = 0;
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    case WM_SYSCHAR:
        return 0;   /* no menu beep on Alt+Enter */
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYUP: case WM_SYSKEYUP:
        keys[w & 255] = 0;
        return 0;
    case WM_KILLFOCUS:
        memset(keys, 0, sizeof keys);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int host_init(void) {
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qp0);
    timeBeginPeriod(1);
    if (g_headless) return 0;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LinkFoERecomp";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    SetProcessDPIAware();   /* render at true pixel resolution on high-DPI / 4K displays */
    int save_aspect = render_settings.aspect, save_filter = render_settings.filter;
    int cli_fs = start_fullscreen;
    if (!runahead_from_cli) g_runahead = 3;   /* windowed default: hides most of the game's input delay */
#ifdef HD_BUILD
    if (!smooth_from_cli) g_smooth_motion = 1;   /* HD edition: 60 fps sprite motion by default */
#endif
    load_settings();
    /* command-line options win over the settings file */
    if (save_aspect) render_settings.aspect = save_aspect;
    if (save_filter) render_settings.filter = save_filter;
    if (cli_fs) start_fullscreen = 1;
    int cw = win_w > 0 ? win_w : 320 * win_scale, chh = win_h > 0 ? win_h : 240 * win_scale;
    if (win_w <= 0) {
        /* default: a 16:9 window filling most of the screen height */
        int sh = GetSystemMetrics(SM_CYSCREEN);
        chh = sh * 3 / 4;
        cw = chh * 16 / 9;
    }
    RECT r = {0, 0, cw, chh};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA(wc.lpszClassName, "Link: The Faces of Evil",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return -1;
    if (use_gpu) gpu_ok = render_init(hwnd) == 0;
    if (start_fullscreen) toggle_fullscreen();
    update_title();
    return 0;
}

double host_time(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - qp0.QuadPart) / (double)qpf.QuadPart;
}

void host_sleep_until(double t) {
    for (;;) {
        double now = host_time();
        if (now >= t) return;
        double d = t - now;
        if (d > 0.002) Sleep((DWORD)((d - 0.001) * 1000));
    }
}

int g_turbo;
int g_fuzz;
static double next_frame_time = -1;
static unsigned long long frame_counter, poll_counter;
int g_dump_frames;
int g_dump_from;      /* first field to dump */      /* write frames to BMP files (debug) */
int g_max_frames;       /* exit after N fields (debug) */

static void dump_bmp(const uint32_t *fb, int w, int h, unsigned long long n) {
    char name[256];
    snprintf(name, sizeof name, "%s/frame_%06llu.bmp", g_data_dir, n);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    BITMAPFILEHEADER bf = {0};
    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32;
    bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bi; bf.bfSize = bf.bfOffBits + w * h * 4;
    fwrite(&bf, sizeof bf, 1, f); fwrite(&bi, sizeof bi, 1, f);
    fwrite(fb, 4, (size_t)w * h, f);
    fclose(f);
}

void host_present(const uint32_t *fb, int w, int h) {
    frame_counter++;
    if (g_dump_frames && frame_counter >= (unsigned long long)g_dump_from && frame_counter % (unsigned)g_dump_frames == 0) dump_bmp(fb, w, h, frame_counter);
    if (g_max_frames && frame_counter >= (unsigned long long)g_max_frames) {
        LOG("reached %d fields, exiting", g_max_frames);
        exit(0);
    }
    if (!g_headless && hwnd && gpu_ok && render_present(fb, w, h) != 0) {
        LOG("GPU present failed; falling back to GDI");
        gpu_ok = 0;
        update_title();
    }
    if (!g_headless && hwnd && !gpu_ok) {
        HDC dc = GetDC(hwnd);
        RECT r;
        GetClientRect(hwnd, &r);
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        /* letterbox the full 384x240 (768 double-res) picture to 4:3 */
        int cw = r.right, ch = r.bottom;
        int dw = cw, dh = cw * 3 / 4;
        if (dh > ch) { dh = ch; dw = ch * 4 / 3; }
        int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (dy > 0) { RECT a = {0, 0, cw, dy}, b = {0, dy + dh, cw, ch}; FillRect(dc, &a, black); FillRect(dc, &b, black); }
        if (dx > 0) { RECT a = {0, 0, dx, ch}, b = {dx + dw, 0, cw, ch}; FillRect(dc, &a, black); FillRect(dc, &b, black); }
        StretchDIBits(dc, dx, dy, dw, dh, 0, 0, w, h, fb, &bi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);
    }
    /* pacing */
    if (g_turbo || g_headless) return;
    double now = host_time();
    if (next_frame_time < 0 || now - next_frame_time > 0.25) next_frame_time = now;
    next_frame_time += 1.0 / FIELD_RATE;
    host_sleep_until(next_frame_time);
}

/* scripted input for automated testing: --script "field:key:dur,field:key:dur"
   keys: 1 = button 1, 2 = button 2, L R U D = directions */
typedef struct { long field; char key; int dur; } ScriptEv;
static ScriptEv script[256];
static int nscript;
void host_set_script(const char *s) {
    while (*s && nscript < 256) {
        ScriptEv *e = &script[nscript];
        e->field = strtol(s, (char **)&s, 10);
        if (*s == ':') s++;
        e->key = *s ? *s++ : '1';
        e->dur = 6;
        if (*s == ':') { s++; e->dur = (int)strtol(s, (char **)&s, 10); }
        nscript++;
        if (*s == ',') s++; else break;
    }
}

/* ---------------------------------------------------------------- XInput (loaded dynamically) */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XIGamepad;
typedef struct { DWORD dwPacketNumber; XIGamepad Gamepad; } XIState;
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XIState *);
static XInputGetStateFn xinput_get;
static int xinput_tried;

static void xinput_load(void) {
    xinput_tried = 1;
    const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !xinput_get; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m) xinput_get = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
    }
}

/* inventory button: the game opens the subscreen when button 2 is pressed
   while holding down, so a press plays "down, then down + button 2" (each
   phase spans at least one of the game's 16 Hz input ticks) */
static int inventory_key, inventory_fields;
#define INVENTORY_MACRO_FIELDS 16
#define INVENTORY_BUTTON_FIELDS 10   /* last fields of the macro also press button 2 */

static void gamepad_poll(void) {
    if (!xinput_tried) xinput_load();
    if (!xinput_get) return;
    for (DWORD pad = 0; pad < 4; pad++) {
        XIState st;
        if (xinput_get(pad, &st) != 0) continue;
        XIGamepad *g = &st.Gamepad;
        int lx = g->sThumbLX, ly = g->sThumbLY;
        const int dead = 8000;
        if (lx > dead || lx < -dead) input.dx += (lx > 0 ? lx - dead : lx + dead) * 10 / (32767 - dead);
        if (ly > dead || ly < -dead) input.dy -= (ly > 0 ? ly - dead : ly + dead) * 10 / (32767 - dead);
        if (g->wButtons & 0x0001) input.dy -= 6;   /* dpad up */
        if (g->wButtons & 0x0002) input.dy += 6;   /* dpad down */
        if (g->wButtons & 0x0004) input.dx -= 6;   /* dpad left */
        if (g->wButtons & 0x0008) input.dx += 6;   /* dpad right */
        if (g->wButtons & 0x1000) input.b1 = 1;    /* A */
        if (g->wButtons & 0x2000) input.b2 = 1;    /* B */
        if (g->wButtons & 0x4000) { input.b1 = 1; input.b2 = 1; }  /* X: both buttons */
        if (g->wButtons & 0x0010) { input.b1 = 1; input.b2 = 1; }  /* Start: both buttons */
        if (g->wButtons & (0x8000 | 0x0020)) inventory_key = 1;   /* Y / Back: inventory */
        break;
    }
}

void host_poll(void) {
    poll_counter++;   /* scripted / fuzz input is timed by real fields */
    if (!g_headless) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    int speed = keys[VK_SHIFT] ? 12 : 6;
    input.dx = (keys[VK_RIGHT] ? speed : 0) - (keys[VK_LEFT] ? speed : 0);
    input.dy = (keys[VK_DOWN] ? speed : 0) - (keys[VK_UP] ? speed : 0);
    input.b1 = keys['Z'] || keys[VK_SPACE] || keys['A'] || keys['C'] || keys[VK_RETURN];
    input.b2 = keys['X'] || keys[VK_CONTROL] || keys['S'] || keys['C'] || keys[VK_RETURN];
    input.b3 = 0;
    inventory_key = keys['I'] || keys['V'];
    if (!g_headless) gamepad_poll();
    g_turbo = keys[VK_TAB];
    if (g_fuzz) {
        /* random input held for random durations (robustness testing) */
        static int hold, fdx, fdy, fb1, fb2;
        static uint32_t st;
        if (!st) st = (uint32_t)g_fuzz * 2654435761u + 1;
        if (--hold <= 0) {
            st = st * 1103515245u + 12345u; uint32_t r = st >> 8;
            hold = 2 + (int)(r % 40);
            int dir = (int)((r >> 6) % 9);
            fdx = (dir % 3 - 1) * 6; fdy = (dir / 3 - 1) * 6;
            fb1 = ((r >> 10) % 4) == 0; fb2 = ((r >> 13) % 6) == 0;
        }
        input.dx = fdx; input.dy = fdy; input.b1 = fb1; input.b2 = fb2;
    }
    for (int i = 0; i < nscript; i++) {
        ScriptEv *e = &script[i];
        if ((long long)poll_counter >= e->field && (long long)poll_counter < e->field + e->dur) {
            switch (e->key) {
            case '1': input.b1 = 1; break;
            case '2': input.b2 = 1; break;
            case 'L': input.dx = -6; break;
            case 'R': input.dx = 6; break;
            case 'U': input.dy = -6; break;
            case 'D': input.dy = 6; break;
            case 'I': if ((long long)poll_counter == e->field) cheat_toggle_invincible(); break;
            case 'M': if ((long long)poll_counter == e->field) cheat_add_rubies(100); break;
            case 'B': inventory_key = 1; break;
            case 'S': if ((long long)poll_counter == e->field) savestate_request(0); break;
            case 'O': if ((long long)poll_counter == e->field) savestate_request(1); break;
            }
        }
    }
    {
        static int was_down;
        if (inventory_key && !was_down && inventory_fields == 0) inventory_fields = INVENTORY_MACRO_FIELDS;
        was_down = inventory_key;
        if (inventory_fields > 0) {
            inventory_fields--;
            input.b1 = 0;
            input.b2 = inventory_fields < INVENTORY_BUTTON_FIELDS;
            input.dx = 0;
            input.dy = 6;
        }
    }
    if (input.quit) {
        LOG("quit requested");
        exit(0);
    }
}

/* ---------------------------------------------------------------- audio */
#define NBUF 12
#define BUF_FRAMES 735          /* one field of audio per buffer */
#define AUDIO_MAX_QUEUED 4      /* about 65 ms queued: keeps sound effects responsive */
static HWAVEOUT wo;
static WAVEHDR hdrs[NBUF];
static int16_t bufs[NBUF][BUF_FRAMES * 2];
static int cur_buf, cur_fill;

void host_audio_init(int rate) {
    if (g_headless) return;
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        LOG("waveOutOpen failed; audio disabled");
        wo = NULL;
        return;
    }
    for (int i = 0; i < NBUF; i++) {
        hdrs[i].lpData = (LPSTR)bufs[i];
        hdrs[i].dwBufferLength = BUF_FRAMES * 4;
        hdrs[i].dwFlags = WHDR_DONE;
    }
}

int host_audio_queued(void) {
    int n = 0;
    for (int i = 0; i < NBUF; i++) if (!(hdrs[i].dwFlags & WHDR_DONE)) n++;
    return n;
}

static FILE *wavf;
static long wav_frames;
void host_wav_open(const char *path) {
    wavf = fopen(path, "wb");
    if (wavf) { uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, wavf); }
}
static void wav_close(void) {
    if (!wavf) return;
    uint32_t data = (uint32_t)wav_frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); *(uint32_t *)(h + 4) = 36 + data; memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16; *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 2;
    *(uint32_t *)(h + 24) = 44100; *(uint32_t *)(h + 28) = 44100 * 4; *(uint16_t *)(h + 32) = 4;
    *(uint16_t *)(h + 34) = 16; memcpy(h + 36, "data", 4); *(uint32_t *)(h + 40) = data;
    fseek(wavf, 0, SEEK_SET); fwrite(h, 1, 44, wavf); fclose(wavf); wavf = NULL;
}

void host_audio_push(const int16_t *s, int frames) {
    if (wavf) {
        fwrite(s, 4, (size_t)frames, wavf);
        wav_frames += frames;
        static int reg;
        if (!reg) { reg = 1; atexit(wav_close); }
    }
    if (!wo) return;
    while (frames > 0) {
        WAVEHDR *h = &hdrs[cur_buf];
        if (!(h->dwFlags & WHDR_DONE)) return;   /* all buffers busy: drop */
        if (cur_fill == 0 && host_audio_queued() >= AUDIO_MAX_QUEUED) return;   /* too much latency: drop */
        int k = BUF_FRAMES - cur_fill;
        if (k > frames) k = frames;
        memcpy(bufs[cur_buf] + cur_fill * 2, s, (size_t)k * 4);
        cur_fill += k; s += k * 2; frames -= k;
        if (cur_fill == BUF_FRAMES) {
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
            h->dwFlags = 0;
            h->dwBufferLength = BUF_FRAMES * 4;
            waveOutPrepareHeader(wo, h, sizeof *h);
            waveOutWrite(wo, h, sizeof *h);
            cur_buf = (cur_buf + 1) % NBUF;
            cur_fill = 0;
        }
    }
}
