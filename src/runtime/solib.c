/*
 * solib.c - dlopen and dlsym for the libraries tools/recomp/solib.py lifted.
 *
 * Their code was lifted to C at recompile time and linked for fixed addresses
 * in the guest window. Their bytes are not in the app: at start-up
 * solib_prepare() reads the player's own copy of each library (the path the
 * recompiler read it from, relative to the game's executable), checks its
 * size and SHA-256 against the ones recorded then, lays out its PT_LOAD
 * segments and applies the relocations recorded then (offset, type, resolved
 * address), and checks the result against the image the recompiler
 * relocated. Loading one is then a copy and running its constructors. All of
 * them load together on the first dlopen of any: they were linked against
 * each other.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

#include "lindbergh_rt.h"

#if __has_include("recomp_solib.h")
#include "recomp_solib.h"
#include "solib.h"

/* ---- SHA-256 (FIPS 180-4) ---- */
typedef struct { uint32_t h[8]; uint64_t n; unsigned char buf[64]; size_t fill; } Sha256;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha256 *s, const unsigned char *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_hex(const unsigned char *p, size_t n, char out[65])
{
    Sha256 s = { { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }, 0, {0}, 0 };
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha_block(&s, p + i);
    unsigned char last[128] = {0};
    size_t rest = n - i;
    memcpy(last, p + i, rest);
    last[rest] = 0x80;
    size_t blocks = rest + 9 > 64 ? 2 : 1;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) last[blocks * 64 - 1 - k] = (unsigned char)(bits >> (8 * k));
    for (size_t k = 0; k < blocks; k++) sha_block(&s, last + 64 * k);
    for (int k = 0; k < 8; k++) snprintf(out + 8 * k, 9, "%08x", s.h[k]);
}

/* ---- reading the player's libraries ---- */
static unsigned char *g_ready[8];     /* each library's relocated image, until it is loaded */
static char g_why[1024];
static int g_loaded;

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }

const char *solib_error(void) { return g_why; }

/* A REL relocation as solib.py's apply_reloc() did it (A: what the image holds there). */
static int apply_relocs(unsigned char *img, const SolibImage *im)
{
    for (int i = 0; i < im->reloc_count; i++) {
        const SolibReloc *r = &im->relocs[i];
        if (r->off > im->size - 4) return -1;
        uint32_t A = le32(img + r->off), v;
        switch (r->type) {
        case 8:  v = im->base + A; break;                      /* R_386_RELATIVE */
        case 1:  v = r->sym + A; break;                        /* R_386_32 */
        case 2:  v = r->sym + A - (im->base + r->off); break;  /* R_386_PC32 */
        case 6: case 7: v = r->sym; break;                     /* R_386_GLOB_DAT, R_386_JMP_SLOT */
        default: return -1;
        }
        img[r->off] = (unsigned char)v; img[r->off + 1] = (unsigned char)(v >> 8);
        img[r->off + 2] = (unsigned char)(v >> 16); img[r->off + 3] = (unsigned char)(v >> 24);
    }
    return 0;
}

/* The loaded image: each PT_LOAD's file bytes at its p_vaddr, the rest zero. */
static unsigned char *lay_out(const unsigned char *f, size_t n, uint32_t size)
{
    if (n < 52 || memcmp(f, "\x7f" "ELF\x01\x01", 6)) return NULL;
    uint32_t phoff = le32(f + 28);
    unsigned phentsize = le16(f + 42), phnum = le16(f + 44);
    if (phentsize < 32 || phoff > n || (size_t)phnum * phentsize > n - phoff) return NULL;
    unsigned char *img = calloc(1, size);
    if (!img) return NULL;
    for (unsigned i = 0; i < phnum; i++) {
        const unsigned char *ph = f + phoff + (size_t)i * phentsize;
        if (le32(ph) != 1) continue;                            /* PT_LOAD */
        uint32_t off = le32(ph + 4), va = le32(ph + 8), filesz = le32(ph + 16);
        if (off > n || filesz > n - off || va > size || filesz > size - va) { free(img); return NULL; }
        memcpy(img + va, f + off, filesz);
    }
    return img;
}

static int fail(const SolibImage *im, const char *where, const char *fmt, const char *arg)
{
    char what[256];
    snprintf(what, sizeof what, fmt, arg);
    snprintf(g_why, sizeof g_why,
             "the game's own %s is needed at %s (%u bytes, SHA-256 %.16s...), but %s. "
             "Copy it there from your own copy of the game (it is not part of this app).",
             im->name, where, im->file_size, im->file_sha256, what);
    fprintf(stderr, "[solib] ERROR: %s\n", g_why);
    return -1;
}

static int prepare_one(int i, const char *elf_path)
{
    const SolibImage *im = &g_solib_images[i];
    char where[1024], cwd[768];
    const char *dir = getenv("LINDBERGH_SOLIB_DIR");
    if (dir && *dir) snprintf(where, sizeof where, "%s/%s", dir, im->name);
    else {
        const char *slash = strrchr(elf_path, '/');
        int dl = slash ? (int)(slash - elf_path) : 0;
        if (slash) snprintf(where, sizeof where, "%.*s/%s", dl, elf_path, im->path);
        else if (getcwd(cwd, sizeof cwd)) snprintf(where, sizeof where, "%s/%s", cwd, im->path);
        else snprintf(where, sizeof where, "%s", im->path);
    }
    FILE *f = fopen(where, "rb");
    if (!f) return fail(im, where, "it cannot be opened (%s)", strerror(errno));
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char got[64];
    if (n != (long)im->file_size) {
        fclose(f);
        snprintf(got, sizeof got, "%ld", n);
        return fail(im, where, "that file is %s bytes - a different version", got);
    }
    unsigned char *raw = malloc((size_t)n);
    if (!raw || fread(raw, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(raw);
        return fail(im, where, "it cannot be read (%s)", "short read");
    }
    fclose(f);
    char hex[65];
    sha256_hex(raw, (size_t)n, hex);
    if (strcmp(hex, im->file_sha256)) { free(raw); return fail(im, where, "that file's SHA-256 is %s - a different version or damaged", hex); }
    unsigned char *img = lay_out(raw, (size_t)n, im->size);
    free(raw);
    if (!img) return fail(im, where, "%s", "its program headers do not fit (not the expected ELF)");
    if (apply_relocs(img, im)) { free(img); return fail(im, where, "%s", "a relocation does not fit"); }
    sha256_hex(img, im->size, hex);
    if (strcmp(hex, im->image_sha256)) {     /* a runtime bug, not the player's file */
        free(img);
        return fail(im, where, "the image relocated here (%s) differs from the recompiler's - a bug in solib.c", hex);
    }
    g_ready[i] = img;
    fprintf(stderr, "[solib] %s read from %s (%u bytes, SHA-256 verified), %d relocations applied\n",
            im->name, where, im->file_size, im->reloc_count);
    return 0;
}

int solib_prepare(const char *elf_path)
{
    g_why[0] = 0;
    if (g_solib_count > (int)(sizeof g_ready / sizeof g_ready[0])) {
        snprintf(g_why, sizeof g_why, "too many libraries (%d)", g_solib_count);
        return -1;
    }
    for (int i = 0; i < g_solib_count; i++)
        if (!g_ready[i] && prepare_one(i, elf_path ? elf_path : "")) return -1;
    return 0;
}

static void load_all(CPU *c)
{
    if (g_loaded) return;
    g_loaded = 1;
    memset(G2H(SOLIB_DATA_BASE), 0, SOLIB_DATA_SIZE);
#ifdef SOLIB_DATA
    {   /* data imports: what the runtime has for them, else zero */
#define FILL(name, addr) { uint32_t v; if (hle_libc_data_import(name, &v)) wr32(addr, v); \
                           else fprintf(stderr, "[solib] data import %s left zero\n", name); }
        SOLIB_DATA(FILL)
#undef FILL
    }
#endif
    for (int i = 0; i < g_solib_count; i++) {
        const SolibImage *im = &g_solib_images[i];
        if (!g_ready[i]) {   /* solib_prepare() failed or was never called: nothing to run */
            fprintf(stderr, "[solib] FATAL: %s was not read at start-up%s%s\n", im->name,
                    g_why[0] ? ": " : "", g_why);
            exit(1);
        }
        memcpy(G2H(im->base), g_ready[i], im->size);
        free(g_ready[i]);
        g_ready[i] = NULL;
        fprintf(stderr, "[solib] %s loaded at %#010x (%u KB)\n", im->name, im->base, im->size >> 10);
    }
    /* constructors, dependencies first (the order solib.py was given) */
    for (int i = 0; i < g_solib_count; i++) {
        const SolibImage *im = &g_solib_images[i];
        if (im->init) guest_call(c, im->init, NULL, 0);
        for (int k = 0; k < im->init_count; k++) {
            uint32_t fn = rd32(im->init_array + 4u * (uint32_t)k);
            if (fn && fn != 0xFFFFFFFFu) guest_call(c, fn, NULL, 0);
        }
    }
}

int solib_dlopen(CPU *c, const char *path)
{
    if (!path) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; i < g_solib_count; i++)
        if (!strcmp(g_solib_images[i].name, base)) { load_all(c); return 1; }
    return 0;
}

uint32_t solib_dlsym(const char *name)
{
    if (!g_loaded || !name) return 0;
    return solib_export(name);
}

uint32_t solib_export(const char *name)
{
    if (!name) return 0;
#define EXP(n, a) if (!strcmp(name, n)) return a;
    SOLIB_EXPORTS(EXP)
#undef EXP
    return 0;
}

/* A main executable may link a lifted library through its PLT, without
 * ever calling dlopen. HLE has already consumed the caller's return slot;
 * restore one for the lifted callee, keeping its original arguments. */
int solib_call(CPU *c, const char *name)
{
    uint32_t fn = solib_export(name);
    if (!fn) return 0;
    CPU saved = *c;
    load_all(c);
    *c = saved;
    /* LINDBERGH_SOLIB_TRACE=1: each call into a lifted library, its first
     * six stack words, a string's start where one points to text, and what
     * it returned in eax */
    static int trace = -1;
    if (trace < 0) { const char *v = getenv("LINDBERGH_SOLIB_TRACE"); trace = v && *v == '1'; }
    uint32_t at = c->esp;
    if (trace) {
        fprintf(stderr, "[solib] -> %s(", name);
        for (int i = 0; i < 6; i++) {
            uint32_t a = rd32(at + 4u * (uint32_t)i);
            fprintf(stderr, "%s%#x", i ? ", " : "", a);
            if ((a >= 0x08048000u && a < 0x0A000000u) || (a >= 0x20000000u && a < 0xB4400000u)) {
                const unsigned char *s = (const unsigned char *)G2H(a);
                if (s[0] >= ' ' && s[0] < 0x7F && s[1] >= ' ' && s[1] < 0x7F)
                    fprintf(stderr, " \"%.24s\"", (const char *)s);
            }
        }
        fprintf(stderr, ")\n");
        if (!strcmp(name, "cgCreateProgram")) {      /* its compiler options, a NULL-ended list */
            uint32_t args = rd32(at + 20);
            for (int i = 0; args && i < 80; i++) {
                uint32_t s = rd32(args + 4u * (uint32_t)i);
                fprintf(stderr, "[solib]    args[%d] = %#x%s%s%s\n", i, s, s ? " \"" : "",
                        s ? (const char *)G2H(s) : "", s ? "\"" : "");
                if (!s) break;
            }
        }
    }
    push32(c, 0xDEADBEEFu);
    dispatch(c, fn);
    if (trace) fprintf(stderr, "[solib] <- %s = %#x\n", name, c->eax);
    if (trace && !strcmp(name, "cgCreateProgram") && !c->eax) {   /* what the compiler said */
        CPU k = *c;
        uint32_t ctx = rd32(at), fn = solib_export("cgGetLastListing");
        uint32_t s = fn ? guest_call(&k, fn, &ctx, 1) : 0;
        fprintf(stderr, "[solib]    profile %#x, listing:\n%s\n", rd32(at + 12), s ? (const char *)G2H(s) : "(none)");
        uint32_t src = rd32(at + 8);
        if (src) fprintf(stderr, "[solib]    source:\n%.3000s\n", (const char *)G2H(src));
    }
    if (trace && !strcmp(name, "cgCreateContext")) {   /* is the profile table there? */
        CPU k = *c;
        uint32_t s = guest_strdup_static("vp40");
        uint32_t gp = solib_export("cgGetProfile"), gs = solib_export("cgGetProfileString");
        uint32_t id = gp ? guest_call(&k, gp, &s, 1) : 0;
        uint32_t vp = 0x1b59, str = gs ? guest_call(&k, gs, &vp, 1) : 0;
        fprintf(stderr, "[solib]    cgGetProfile(\"vp40\") = %#x, cgGetProfileString(0x1b59) = %#x %s\n",
                id, str, str ? (const char *)G2H(str) : "(null)");
    }
    return 1;
}
#else
int solib_prepare(const char *elf_path) { (void)elf_path; return 0; }
const char *solib_error(void) { return ""; }
int solib_dlopen(CPU *c, const char *path) { (void)c; (void)path; return 0; }
uint32_t solib_dlsym(const char *name) { (void)name; return 0; }
uint32_t solib_export(const char *name) { (void)name; return 0; }
int solib_call(CPU *c, const char *name) { (void)c; (void)name; return 0; }
#endif
