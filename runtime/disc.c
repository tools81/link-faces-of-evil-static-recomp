/* CD-i disc image access (raw 2352-byte Mode 2 sectors, Green Book filesystem). */
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static FILE *img;
static DiscFile files[64];
static int nfiles;

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

void disc_read_raw(uint32_t lsn, uint8_t *buf) {
    _fseeki64(img, (long long)lsn * 2352, SEEK_SET);
    if (fread(buf, 1, 2352, img) != 2352) memset(buf, 0, 2352);
}

static void parse_dir(uint32_t lsn, uint32_t size) {
    uint8_t raw[2352];
    uint32_t blocks = (size + 2047) / 2048;
    for (uint32_t b = 0; b < blocks; b++) {
        disc_read_raw(lsn + b, raw);
        const uint8_t *d = raw + 24;
        int p = 0;
        while (p < 2048) {
            int ln = d[p];
            if (!ln) break;
            const uint8_t *rec = d + p;
            uint32_t start = be32(rec + 6), fsize = be32(rec + 14);
            int nlen = rec[32];
            char name[32] = {0};
            memcpy(name, rec + 33, nlen < 31 ? nlen : 31);
            int q = 33 + nlen;
            if (q & 1) q++;
            uint16_t attr = q + 6 <= ln ? be16(rec + q + 4) : 0;
            uint8_t fnum = q + 9 <= ln ? rec[q + 8] : 0;
            if (nlen == 1 && (name[0] == 0 || name[0] == 1)) { p += ln; continue; }
            if (attr & 0x8000) {
                parse_dir(start, fsize);
            } else if (nfiles < 64) {
                DiscFile *f = &files[nfiles++];
                snprintf(f->name, sizeof f->name, "%s", name);
                f->lsn = start; f->size = fsize; f->fnum = fnum; f->attr = attr;
            }
            p += ln;
        }
    }
}

int disc_open(const char *path) {
    img = fopen(path, "rb");
    if (!img) return -1;
    uint8_t raw[2352];
    disc_read_raw(16, raw);
    if (memcmp(raw + 24 + 1, "CD-I ", 5)) { LOG("not a CD-i disc"); return -1; }
    uint32_t pt_lsn = be32(raw + 24 + 148);
    disc_read_raw(pt_lsn, raw);
    uint32_t root = be32(raw + 24 + 2);
    disc_read_raw(root, raw);
    uint32_t rsize = be32(raw + 24 + 14);
    parse_dir(root, rsize);
    for (int i = 0; i < nfiles; i++)
        LOG("disc: %-14s lsn %6u size %9u fnum %u", files[i].name, files[i].lsn, files[i].size, files[i].fnum);
    return 0;
}

int disc_num_files(void) { return nfiles; }
const DiscFile *disc_file(int i) { return &files[i]; }

const DiscFile *disc_find(const char *name) {
    for (int i = 0; i < nfiles; i++)
        if (!_stricmp(files[i].name, name)) return &files[i];
    return NULL;
}

int disc_read_file(const DiscFile *f, uint8_t **out, uint32_t *len) {
    uint32_t n = (f->size + 2047) / 2048;
    uint8_t *buf = (uint8_t *)malloc((size_t)n * 2048 + 1);
    uint8_t raw[2352];
    for (uint32_t i = 0; i < n; i++) {
        disc_read_raw(f->lsn + i, raw);
        memcpy(buf + i * 2048, raw + 24, 2048);
    }
    *out = buf;
    *len = f->size;
    return 0;
}
