/* Entry point: load the disc, set up the HLE OS and run the recompiled game. */
#include "recomp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "find_image.inc"

int log_level = 1;
static FILE *logfp;
extern int g_headless, g_dump_frames, g_max_frames;

void logf_(int lvl, const char *fmt, ...) {
    (void)lvl;
    extern int g_ra_active;
    if (g_ra_active) return;
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
    if (logfp) { fprintf(logfp, "%s\n", buf); fflush(logfp); }
}

void fatal(const char *fmt, ...) {
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logf_(0, "FATAL: %s", buf);
    logf_(0, "  pc=%06x d=%08x %08x %08x %08x %08x %08x %08x %08x", m68k.pc,
          D(0), D(1), D(2), D(3), D(4), D(5), D(6), D(7));
    logf_(0, "  a=%08x %08x %08x %08x %08x %08x %08x %08x",
          A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7));
    /* crude 68k stack backtrace: print code addresses found on the stack */
    for (int i = 0; i < 64; i++) {
        uint32_t v = RL(A(7) + i * 4);
        if (v >= recomp_code_lo && v < recomp_code_hi)
            logf_(0, "  stack+%03x: %06x (%s)", i * 4, v, func_name(v));
    }
    {
        char rp[600];
        snprintf(rp, sizeof rp, "%s/ramdump.bin", g_data_dir);
        FILE *rf = fopen(rp, "wb");
        if (rf) { fwrite(RAM, 1, MEM_SIZE, rf); fclose(rf); }
    }
#ifdef _WIN32
    if (!g_headless) {
        MessageBoxA(NULL, buf, "Recomp fatal error", MB_ICONERROR);
    }
#endif
    exit(1);
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    const char *img_arg = NULL;
    int no_update = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--no-update")) no_update = 1;
        else if (!strcmp(argv[i], "--portable")) { /* always portable: accepted for shortcuts */ }
        else if (!strcmp(argv[i], "--trace")) log_level = 2;
        else if (!strcmp(argv[i], "--trace3")) log_level = 3;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) g_dump_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) { extern int g_dump_from; g_dump_from = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--aspect") || !strcmp(argv[i], "--filter") || !strcmp(argv[i], "--size") ||
                 !strcmp(argv[i], "--runahead") || !strcmp(argv[i], "--smooth")) {
            extern void host_video_option(const char *, const char *);
            if (i + 1 < argc) { host_video_option(argv[i], argv[i + 1]); i++; }
        }
        else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "--windowed") || !strcmp(argv[i], "--gdi")) {
            extern void host_video_option(const char *, const char *);
            host_video_option(argv[i], NULL);
        }
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) {
            extern void savestate_load_at_start(int);
            savestate_load_at_start(atoi(argv[++i]));
        }
        else if (!strcmp(argv[i], "--interp")) { extern int g_interp_all; g_interp_all = 1; }
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) { extern int g_fuzz; g_fuzz = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { extern void host_wav_open(const char *); host_wav_open(argv[++i]); }
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) { extern void host_set_script(const char *); host_set_script(argv[++i]); }
        else img_arg = argv[i];
    }
    init_exe_dir();
    setup_data_dir();
    {
        char lp[600];
        snprintf(lp, sizeof lp, "%s/recomp.log", g_data_dir);
        logfp = fopen(lp, "w");
    }
    portable_init(exe_dir);
    LOG("Link: The Faces of Evil recomp v%s", portable_version());
    if (!g_headless && !no_update && !g_max_frames) portable_check_update();

    /* disc image: command line, then linkfoe.ini, then a rom\ folder nearby,
       then ask the player */
    char img[1024], ini_rom[1024];
    int found = 0;
    if (img_arg) found = !find_image(img_arg, img, sizeof img);
    else {
        if (!portable_rom_path(ini_rom, sizeof ini_rom)) found = !find_image(ini_rom, img, sizeof img);
        if (!found) found = !find_image(NULL, img, sizeof img);
    }
    while (!found && !img_arg && !g_headless) {
        char pick[1024];
        if (portable_pick_rom(pick, sizeof pick)) break;
        if (!find_image(pick, img, sizeof img)) { portable_save_rom(pick); found = 1; break; }
#ifdef _WIN32
        MessageBoxA(NULL, "That file is not a CD-i disc image (.cue or raw .bin).", "Link: The Faces of Evil",
                    MB_ICONWARNING);
#endif
    }
    if (!found) {
        fatal("Could not find the Link: The Faces of Evil disc image.\n\n"
              "Run the game again and choose the .cue or .bin file, put it in a 'rom' folder next to the program, "
              "or pass its path as an argument.");
    }
    LOG("disc image: %s", img);
    if (disc_open(img)) fatal("failed to open disc image %s", img);

    RAM = (uint8_t *)calloc(MEM_SIZE + 16, 1);
    cpu_init();
    os9_init();
    host_init();
    video_init();
    audio_init();
    cdfm_init();
    sched_init();

    const DiscFile *df = disc_find("cdi_link");
    if (!df) fatal("cdi_link not found on disc");
    uint8_t *mod; uint32_t len;
    disc_read_file(df, &mod, &len);
    os9_load_main_module(mod, len);
    free(mod);
    sched_init();

    cpu_run();
    return 0;
}
