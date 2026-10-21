#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#endif

static char exe_dir[512] = ".";

static void init_exe_dir(void) {
#ifdef _WIN32
    char p[512];
    DWORD n = GetModuleFileNameA(NULL, p, sizeof p);
    if (n > 0 && n < sizeof p) {
        char *s = strrchr(p, '\\');
        if (s) { *s = 0; snprintf(exe_dir, sizeof exe_dir, "%s", p); }
    }
#endif
}

/* saves (NVRAM files), the log and debug dumps live in <exe dir>/saves */
static void setup_data_dir(void) {
    snprintf(g_data_dir, sizeof g_data_dir, "%s/saves", exe_dir);
#ifdef _WIN32
    _mkdir(g_data_dir);
#endif
}

/* Resolve a .cue to the .bin it references. */
static void cue_to_bin(const char *cue, char *out, size_t n) {
    snprintf(out, n, "%s", cue);
    FILE *f = fopen(cue, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!strncmp(line, "FILE", 4) && q1 && q2) {
            *q2 = 0;
            char dir[512];
            snprintf(dir, sizeof dir, "%s", cue);
            char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
            if (bs > sl) sl = bs;
            if (sl) { sl[1] = 0; snprintf(out, n, "%s%s", dir, q1 + 1); }
            else snprintf(out, n, "%s", q1 + 1);
            break;
        }
    }
    if (f) fclose(f);
}

/* Is this a raw 2352-byte-sector CD-i image? (disc label "CD-I " at sector 16) */
static int is_cdi_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[8] = {0};
    int ok = fseek(f, 16L * 2352 + 24 + 1, SEEK_SET) == 0 && fread(buf, 1, 5, f) == 5 && !memcmp(buf, "CD-I ", 5);
    fclose(f);
    return ok;
}

static int scan_dir_for_image(const char *dir, char *out, size_t n) {
#ifdef _WIN32
    char pat[600];
    snprintf(pat, sizeof pat, "%s\\*.bin", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char p[800];
        snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        if (is_cdi_image(p)) { FindClose(h); snprintf(out, n, "%s", p); return 0; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    (void)dir; (void)out; (void)n;
#endif
    return -1;
}

static int find_image(const char *arg, char *out, size_t n) {
    if (arg) {
        size_t L = strlen(arg);
        if (L > 4 && !_stricmp(arg + L - 4, ".cue")) cue_to_bin(arg, out, n);
        else snprintf(out, n, "%s", arg);
        return is_cdi_image(out) ? 0 : -1;
    }
    /* look for rom/*.bin next to the working directory or the executable, up to 4 levels up */
    const char *bases[2] = {".", exe_dir};
    for (int b = 0; b < 2; b++) {
        char dir[600];
        snprintf(dir, sizeof dir, "%s", bases[b]);
        for (int up = 0; up <= 4; up++) {
            char romdir[700];
            snprintf(romdir, sizeof romdir, "%s\\rom", dir);
            if (!scan_dir_for_image(romdir, out, n)) return 0;
            if (!scan_dir_for_image(dir, out, n)) return 0;
            size_t L = strlen(dir);
            if (L + 3 < sizeof dir) strcat(dir, "\\..");
        }
    }
    return -1;
}

