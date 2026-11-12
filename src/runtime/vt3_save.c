/*
 * vt3_save.c - Virtua Tennis 3's save file (one card record per character).
 * See vt3_save.h for the format and the safety rules.
 */
#include "vt3_save.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#define fsync _commit
#define fileno _fileno
#else
#include <unistd.h>
#endif

#define HDR 32
#define ENT (24 + VT3_SAVE_REC)
static const char MAGIC[8] = { 'V', 'T', '3', 'S', 'A', 'V', 'E', 0 };
static const char GAME[8] = { 'V', 'T', '3', 'L', 'H', 'D', 0, 0 };

static struct { int character; uint64_t when; uint8_t rec[VT3_SAVE_REC]; } g_ent[VT3_SAVE_MAX];
static int g_n;
static int g_nowrite;                    /* an unreadable file could not be moved aside: never written over */
static char g_path[512];

uint32_t vt3_save_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* the file's entries into memory; NULL on success, else what is wrong */
static const char *parse(const char *path, int *missing)
{
    *missing = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { *missing = errno == ENOENT; return *missing ? "no file" : strerror(errno); }
    uint8_t *b = malloc(HDR + VT3_SAVE_MAX * ENT + 1);
    size_t n = b ? fread(b, 1, HDR + VT3_SAVE_MAX * ENT + 1, f) : 0;
    fclose(f);
    const char *why = NULL;
    if (!b) return "out of memory";
    if (n < HDR || memcmp(b, MAGIC, 8)) why = "not a save file (magic)";
    else if (get32(b + 8) != 1) why = "unknown version";
    else if (get32(b + 12) != VT3_SAVE_REC) why = "wrong record size";
    else if (get32(b + 16) > VT3_SAVE_MAX || n != HDR + (size_t)get32(b + 16) * ENT) why = "truncated or wrong length";
    else if (vt3_save_crc32(b + HDR, n - HDR) != get32(b + 20)) why = "checksum";
    else if (memcmp(b + 24, GAME, 8)) why = "another game's save";
    if (!why) {
        const int cnt = (int)get32(b + 16);
        for (int i = 0; i < cnt && !why; i++) {
            const uint8_t *e = b + HDR + (size_t)i * ENT;
            if (vt3_save_crc32(e + 24, VT3_SAVE_REC) != get32(e + 16)) why = "record checksum";
        }
        if (!why) {
            g_n = cnt;
            for (int i = 0; i < cnt; i++) {
                const uint8_t *e = b + HDR + (size_t)i * ENT;
                g_ent[i].character = (int)get32(e);
                g_ent[i].when = (uint64_t)get32(e + 8) | (uint64_t)get32(e + 12) << 32;
                memcpy(g_ent[i].rec, e + 24, VT3_SAVE_REC);
            }
        }
    }
    free(b);
    return why;
}

const char *vt3_save_path(void) { return g_path; }
int vt3_save_count(void) { return g_n; }

int vt3_save_open(const char *path)
{
    if (!path) { const char *e = getenv("LINDBERGH_VT3_SAVE_FILE"); path = e && *e ? e : "vt3_save.bin"; }
    snprintf(g_path, sizeof g_path, "%s", path);
    g_n = 0; g_nowrite = 0;
    int missing = 0;
    const char *why = parse(g_path, &missing);
    if (!why) { fprintf(stderr, "[save] %s: %d record(s) loaded\n", g_path, g_n); return VT3_SAVE_LOADED; }
    char bak[540];
    snprintf(bak, sizeof bak, "%s.bak", g_path);
    int bmissing = 0;
    const char *bwhy = parse(bak, &bmissing);
    if (missing) {      /* no file: a backup left by a write cut short, or by a file moved aside, else nothing yet */
        if (!bwhy) { fprintf(stderr, "[save] %s missing: %d record(s) loaded from the backup %s\n", g_path, g_n, bak); return VT3_SAVE_FROM_BAK; }
        g_n = 0;
        fprintf(stderr, "[save] %s: no save yet - a fresh one is made after the first match\n", g_path);
        return VT3_SAVE_FRESH;
    }
    /* the bad file is moved aside either way: never written over */
    char aside[560];
    snprintf(aside, sizeof aside, "%s.unreadable-%ld", g_path, (long)time(NULL));
    int moved = rename(g_path, aside) == 0;
    if (!moved) g_nowrite = 1;
    fprintf(stderr, "[save] %s unreadable (%s)%s%s\n", g_path, why, moved ? " - kept as " : " - could not move it aside", moved ? aside : "");
    if (!bwhy) { fprintf(stderr, "[save] %s: %d record(s) loaded from the backup\n", bak, g_n); return VT3_SAVE_FROM_BAK; }
    g_n = 0;
    fprintf(stderr, "[save] backup %s %s - starting a fresh save\n", bak, bmissing ? "missing" : bwhy);
    return VT3_SAVE_BAD_FRESH;
}

const uint8_t *vt3_save_find(int character)
{
    for (int i = 0; i < g_n; i++) if (g_ent[i].character == character) return g_ent[i].rec;
    return NULL;
}

int vt3_save_store(int character, const uint8_t rec[VT3_SAVE_REC])
{
    if (!g_path[0]) vt3_save_open(NULL);
    int k = -1;
    for (int i = 0; i < g_n; i++) if (g_ent[i].character == character) k = i;
    if (k < 0) {
        if (g_n >= VT3_SAVE_MAX) { fprintf(stderr, "[save] no room for character %d\n", character); return -1; }
        k = g_n++;
        g_ent[k].character = character;
    }
    memcpy(g_ent[k].rec, rec, VT3_SAVE_REC);
    g_ent[k].when = (uint64_t)time(NULL);
    if (g_nowrite) { fprintf(stderr, "[save] not written: %s is unreadable and could not be moved aside\n", g_path); return -1; }
    /* the whole file */
    const size_t n = HDR + (size_t)g_n * ENT;
    uint8_t *b = calloc(1, n);
    if (!b) return -1;
    memcpy(b, MAGIC, 8);
    put32(b + 8, 1); put32(b + 12, VT3_SAVE_REC); put32(b + 16, (uint32_t)g_n);
    memcpy(b + 24, GAME, 8);
    for (int i = 0; i < g_n; i++) {
        uint8_t *e = b + HDR + (size_t)i * ENT;
        put32(e, (uint32_t)g_ent[i].character);
        put32(e + 8, (uint32_t)g_ent[i].when); put32(e + 12, (uint32_t)(g_ent[i].when >> 32));
        put32(e + 16, vt3_save_crc32(g_ent[i].rec, VT3_SAVE_REC));
        memcpy(e + 24, g_ent[i].rec, VT3_SAVE_REC);
    }
    put32(b + 20, vt3_save_crc32(b + HDR, n - HDR));
    char tmp[540], bak[540];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    snprintf(bak, sizeof bak, "%s.bak", g_path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(b, 1, n, f) == n;
    if (f) { ok = fflush(f) == 0 && ok; ok = fsync(fileno(f)) == 0 && ok; ok = fclose(f) == 0 && ok; }
    free(b);
    if (!ok) { fprintf(stderr, "[save] could not write %s (%s)\n", tmp, strerror(errno)); remove(tmp); return -1; }
    FILE *old = fopen(g_path, "rb");
    if (old) { fclose(old); remove(bak); if (rename(g_path, bak) != 0) fprintf(stderr, "[save] could not keep %s as %s\n", g_path, bak); }
    if (rename(tmp, g_path) != 0) { fprintf(stderr, "[save] could not put %s in place (%s)\n", g_path, strerror(errno)); return -1; }
    return 0;
}
