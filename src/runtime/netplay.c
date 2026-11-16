/*
 * netplay.c - the cabinet's one input layer (netplay.h): recording and
 * replay, the state hash, deterministic mode, and input lockstep between two
 * copies of the game over UDP on a LAN (the protocol itself: lockstep.c).
 *
 * Every reader of the cabinet comes through here: the JVS board's switch,
 * coin and analog reads (hle_sega.c) and the game hooks that look at a button
 * between reads (games' host.c). With no switch set each call is the host's
 * own (host_cabinet_input / host_cabinet_buttons), unchanged.
 *
 * An input frame ("tick") starts at each JVS switch read: the game polls its
 * board once a frame, on one thread, so the n-th read is the n-th frame on
 * any machine however fast it runs. Recording, replay, the hash and the
 * lockstep all count in ticks.
 */
#include "netplay.h"
#include "lockstep.h"
#include "game_profile.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define NP_SOCKETS 1
#endif

#ifndef LINDBERGH_RUNTIME_REV
#if __has_include("lindbergh_version.h")
#include "lindbergh_version.h"
#else
#define LINDBERGH_RUNTIME_REV "unknown"
#endif
#endif

extern unsigned g_frame;

/* ---- switches ---- */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int  g_det;                  /* deterministic mode */
static int  g_layer;                /* the layer takes over the input (REC, PLAY, NETPLAY) */
static unsigned g_hash_every, g_hash_detail;
static int  g_tick_log;
static FILE *g_rec, *g_play;
static int  g_play_exit, g_play_done;
static char g_net[256];
static uint32_t g_tick;             /* input frames so far */
static CabinetInput g_cur;          /* this frame's input, as the game is given it */

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static uint32_t now_us(void) { return (uint32_t)(uint64_t)(now_ms() * 1000.0); }

static int env_on(const char *k) { const char *v = getenv(k); return v && *v && *v != '0'; }

static void np_net_init(void);

static void init_once(void)
{
    const char *rec = getenv("LINDBERGH_INPUT_REC"), *play = getenv("LINDBERGH_INPUT_PLAY");
    const char *net = getenv("LINDBERGH_NETPLAY"), *h = getenv("LINDBERGH_STATE_HASH");
    if (net && *net) snprintf(g_net, sizeof g_net, "%s", net);
    g_layer = (rec && *rec) || (play && *play) || g_net[0];
    g_det = g_layer || env_on("LINDBERGH_DETERMINISTIC");
    { const char *d = getenv("LINDBERGH_DETERMINISTIC"); if (d && *d == '0') g_det = 0; }   /* =0: the fixes off, to measure them */
    if (h && *h) g_hash_every = (unsigned)strtoul(h, NULL, 10);
    g_hash_detail = (unsigned)env_on("LINDBERGH_STATE_HASH_DETAIL");
    g_tick_log = env_on("LINDBERGH_TICK_LOG");
    g_play_exit = env_on("LINDBERGH_INPUT_PLAY_EXIT");
    if (g_det) { extern int g_lg_det_math; g_lg_det_math = !(getenv("LINDBERGH_DET_LIBM") && *getenv("LINDBERGH_DET_LIBM") == '0'); }
    if (g_det) fprintf(stderr, "[det] deterministic mode on (guest clock by frame, own rand, own libm, separate runtime heap)\n");
}
static void np_init(void) { pthread_once(&g_once, init_once); }

int lg_deterministic(void) { np_init(); return g_det; }
uint32_t lg_input_tick(void) { return g_tick; }

/* ---- the guest's clock: 2007-01-01 12:00 UTC plus the frames at 60 Hz ---- */
#define DET_EPOCH_S 1167652800ull
uint64_t lg_det_time_us(void)
{
    return DET_EPOCH_S * 1000000ull + (uint64_t)g_tick * 1000000ull / 60ull;
}

/* ---- rand(): glibc's random_r TYPE_3 (x[i] = x[i-3] + x[i-31]) ---- */
static pthread_mutex_t g_rand_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_r[34 + 310 + 1]; static int g_rn = -1, g_rk;
static int32_t g_rtab[31]; static int g_rf, g_rr;
static void det_seed(unsigned seed)
{
    if (seed == 0) seed = 1;
    g_rtab[0] = (int32_t)seed;
    for (int i = 1; i < 31; i++) {          /* 16807 * x mod (2^31 - 1), Schrage's way, as glibc */
        long hi = g_rtab[i - 1] / 127773, lo = g_rtab[i - 1] % 127773;
        long w = 16807 * lo - 2836 * hi;
        if (w < 0) w += 2147483647;
        g_rtab[i] = (int32_t)w;
    }
    g_rf = 3; g_rr = 0;
    for (int i = 0; i < 310; i++) {         /* glibc discards the first 310 */
        g_rtab[g_rf] = (int32_t)((uint32_t)g_rtab[g_rf] + (uint32_t)g_rtab[g_rr]);
        if (++g_rf >= 31) g_rf = 0;
        if (++g_rr >= 31) g_rr = 0;
    }
    g_rn = 0; (void)g_r; (void)g_rk;
}
void lg_det_srand(unsigned seed)
{
    pthread_mutex_lock(&g_rand_lock);
    det_seed(seed);
    pthread_mutex_unlock(&g_rand_lock);
}
int lg_det_rand(void)
{
    pthread_mutex_lock(&g_rand_lock);
    if (g_rn < 0) det_seed(1);
    uint32_t v = (uint32_t)g_rtab[g_rf] + (uint32_t)g_rtab[g_rr];
    g_rtab[g_rf] = (int32_t)v;
    int r = (int)(v >> 1);
    if (++g_rf >= 31) g_rf = 0;
    if (++g_rr >= 31) g_rr = 0;
    pthread_mutex_unlock(&g_rand_lock);
    return r;
}

/* ---- settings that change the simulation ---- */
static struct { char name[48]; long value; } g_set[48];
static int g_nset;
static void (*g_set_report[4])(void);
static int g_nreport;
void netplay_setting(const char *name, long value)
{
    for (int i = 0; i < g_nset; i++) if (!strcmp(g_set[i].name, name)) { g_set[i].value = value; return; }
    if (g_nset == 48) return;
    snprintf(g_set[g_nset].name, sizeof g_set[g_nset].name, "%s", name);
    g_set[g_nset].value = value;
    g_nset++;
}
void netplay_settings_source(void (*report)(void)) { if (g_nreport < 4) g_set_report[g_nreport++] = report; }
static int set_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void settings_string(char *out, size_t cap)
{
    for (int i = 0; i < g_nreport; i++) g_set_report[i]();
    netplay_setting("pace_fps", game_profile()->pace_fps);
    qsort(g_set, (size_t)g_nset, sizeof g_set[0], set_cmp);
    size_t n = 0; out[0] = 0;
    for (int i = 0; i < g_nset && n < cap; i++) n += (size_t)snprintf(out + n, cap - n, "%s=%ld;", g_set[i].name, g_set[i].value);
}

/* ---- the game's executable ---- */
static uint8_t g_elf_sha[32]; static int g_elf_known;
void netplay_set_elf(const void *img, unsigned long len)
{
    np_init();
    if (!g_layer) return;                /* only a recording or a session needs it */
    ls_sha256(img, (size_t)len, g_elf_sha);
    g_elf_known = 1;
}
static void hex(const uint8_t *p, int n, char *out) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", p[i]); }

/* ---- the state hash: guest memory that holds the game's state ----
 * By default the image's writable segments (.data, .bss); the game's profile
 * (state_hash) or LINDBERGH_STATE_HASH_REGIONS="lo-hi,...,!lo-hi" names its
 * own list, "!" ranges taken out (timers, host-side buffers). */
typedef struct { uint32_t lo, hi; } Range;
static Range g_rw[8]; static int g_nrw;
static Range g_inc[32], g_exc[64]; static int g_ninc = -1, g_nexc;
void netplay_add_image_rw(uint32_t lo, uint32_t hi) { if (g_nrw < 8) g_rw[g_nrw++] = (Range){ lo, hi }; }
static void parse_ranges(const char *v)
{
    while (v && *v) {
        int ex = *v == '!'; if (ex) v++;
        char *e; unsigned long lo = strtoul(v, &e, 16), hi = lo;
        if (*e == '-') hi = strtoul(e + 1, &e, 16);
        if (hi > lo) {
            if (ex && g_nexc < 64) g_exc[g_nexc++] = (Range){ (uint32_t)lo, (uint32_t)hi };
            else if (!ex && g_ninc < 32) g_inc[g_ninc++] = (Range){ (uint32_t)lo, (uint32_t)hi };
        }
        v = strchr(e, ',');
        if (v) v++;
    }
}
static void hash_regions(void)
{
    if (g_ninc >= 0) return;
    g_ninc = 0;
    const char *v = getenv("LINDBERGH_STATE_HASH_REGIONS");
    if (!(v && *v)) v = game_profile()->state_hash;
    parse_ranges(v);
    if (!g_ninc) for (int i = 0; i < g_nrw; i++) g_inc[g_ninc++] = g_rw[i];
    char d[1024]; size_t n = 0; d[0] = 0;
    for (int i = 0; i < g_ninc && n < sizeof d; i++) n += (size_t)snprintf(d + n, sizeof d - n, " %#x-%#x", g_inc[i].lo, g_inc[i].hi);
    for (int i = 0; i < g_nexc && n < sizeof d; i++) n += (size_t)snprintf(d + n, sizeof d - n, " !%#x-%#x", g_exc[i].lo, g_exc[i].hi);
    fprintf(stderr, "[hash] state hash over%s\n", d);
}
/* The words of a range, except the runtime's own: a guest mutex or condition
 * holds the host object's pointer behind hle_pthread.c's "LGJ1" magic (a
 * different address in every run) - the magic is hashed, the 12 bytes after
 * it are not. */
#define GUEST_OBJ_MAGIC 0x4C474A31u
static uint64_t hash_words(uint64_t h, uint32_t lo, uint32_t hi)
{
    uint32_t a = lo;
    while (a < hi) {
        uint32_t end = a;
        while (end + 4 <= hi && rd32(end) != GUEST_OBJ_MAGIC) end += 4;
        if (end + 4 > hi) end = hi;
        if (end > a) h = ls_hash64(h, G2H(a), end - a);
        if (end >= hi) break;
        h = ls_hash64(h, G2H(end), 4);       /* the magic */
        a = end + 16;
    }
    return h;
}

/* one range less the excluded ones, fed to the hash in order */
static uint64_t hash_range(uint64_t h, uint32_t lo, uint32_t hi)
{
    while (lo < hi) {
        uint32_t end = hi, skip_to = 0;
        for (int i = 0; i < g_nexc; i++) {
            if (g_exc[i].hi <= lo || g_exc[i].lo >= end) continue;
            if (g_exc[i].lo <= lo) { if (g_exc[i].hi > skip_to) skip_to = g_exc[i].hi; }
            else end = g_exc[i].lo;
        }
        if (skip_to) { lo = skip_to; continue; }
        h = hash_words(h, lo, end);
        lo = end;
    }
    return h;
}
static uint64_t state_hash(uint32_t tick)
{
    hash_regions();
    uint64_t h = LS_HASH_SEED;
    char *det = NULL; size_t dn = 0, dcap = 0;
    if (g_hash_detail) { dcap = 64 * 1024; det = (char *)malloc(dcap); if (det) det[0] = 0; }
    for (int i = 0; i < g_ninc; i++) {
        if (!det) { h = hash_range(h, g_inc[i].lo, g_inc[i].hi); continue; }
        for (uint32_t c = g_inc[i].lo; c < g_inc[i].hi; ) {     /* 4 KB chunks for the detail line */
            uint32_t e = (c | 0xFFFu) + 1; if (e > g_inc[i].hi) e = g_inc[i].hi;
            uint64_t ch = hash_range(LS_HASH_SEED, c, e);
            h = hash_range(h, c, e);
            if (dn + 32 < dcap) dn += (size_t)snprintf(det + dn, dcap - dn, " %08x:%016llx", c, (unsigned long long)ch);
            c = e;
        }
    }
    if (det) { fprintf(stderr, "[hashd] frame %u:%s\n", tick, det); free(det); }
    {   /* LINDBERGH_STATE_DUMP=<file>@<frame>[,<frame>...]: the hashed memory raw, for a byte diff
         * (<file>.<frame>: per range, lo and hi as u32 then the bytes) */
        const char *v = getenv("LINDBERGH_STATE_DUMP"), *at = v ? strchr(v, '@') : NULL;
        for (const char *p = at; p && *p; p = strchr(p + 1, ',')) {
            if ((uint32_t)strtoul(p + 1, NULL, 10) != tick) continue;
            char path[600]; snprintf(path, sizeof path, "%.*s.%u", (int)(at - v), v, tick);
            FILE *f = fopen(path, "wb");
            for (int i = 0; f && i < g_ninc; i++) {
                fwrite(&g_inc[i].lo, 4, 1, f); fwrite(&g_inc[i].hi, 4, 1, f);
                fwrite(G2H(g_inc[i].lo), 1, g_inc[i].hi - g_inc[i].lo, f);
            }
            if (f) { fclose(f); fprintf(stderr, "[hash] frame %u: state dumped to %s\n", tick, path); }
        }
    }
    return h;
}

/* ---- canonical input: the gun in the board's 0..65535, screen 65535 ---- */
static int gun_units(int v, int size) { if (v < 0) return -1; long u = (long)v * 65535 / (size > 0 ? size : 1); return (int)(u > 65535 ? 65535 : u); }
static void canonical(CabinetInput *c)
{
    c->gun_x = gun_units(c->gun_x, c->screen_w); c->gun_y = gun_units(c->gun_y, c->screen_h);
    if (c->gun2) { c->gun2_x = gun_units(c->gun2_x, c->screen_w); c->gun2_y = gun_units(c->gun2_y, c->screen_h); }
    else c->gun2_x = c->gun2_y = 0;
    c->screen_w = c->screen_h = 65535;
}

/* ---- recording and replay ----
 * A text header ("LGINPUT1", then name=value lines, then "---"), then 32
 * bytes a frame, little-endian: tick, buttons, coins[2] (u16), gun x/y, gun 2
 * x/y (i32), gun 2 on (u8), 3 spare. */
#define REC_MAGIC "LGINPUT1"
typedef struct { uint32_t tick, buttons; uint16_t coins[2]; int32_t gx, gy, g2x, g2y; uint8_t gun2, pad[3]; } RecFrame;
static char g_play_hdr[2048];
static void header_lines(char *out, size_t cap)
{
    char sha[65] = "unknown", set[1024];
    if (g_elf_known) hex(g_elf_sha, 32, sha);
    settings_string(set, sizeof set);
    snprintf(out, cap, "elf=%s\nruntime=%s\nsettings=%s\ndeterministic=%d\n", sha, LINDBERGH_RUNTIME_REV, set, g_det);
}
static void rec_open(void)
{
    const char *path = getenv("LINDBERGH_INPUT_REC");
    if (path && *path) {
        g_rec = fopen(path, "wb");
        if (!g_rec) fprintf(stderr, "[rec] cannot write %s: %s\n", path, strerror(errno));
        else {
            char h[2048]; header_lines(h, sizeof h);
            fprintf(g_rec, "%s\n%s---\n", REC_MAGIC, h);
            fprintf(stderr, "[rec] recording the cabinet's input to %s\n", path);
        }
    }
    path = getenv("LINDBERGH_INPUT_PLAY");
    if (path && *path) {
        g_play = fopen(path, "rb");
        if (!g_play) { fprintf(stderr, "[rec] cannot read %s: %s\n", path, strerror(errno)); return; }
        char line[1100]; size_t n = 0;
        if (!fgets(line, sizeof line, g_play) || strncmp(line, REC_MAGIC, 8)) {
            fprintf(stderr, "[rec] %s is not an input recording\n", path); fclose(g_play); g_play = NULL; return;
        }
        while (fgets(line, sizeof line, g_play) && strcmp(line, "---\n")) {
            size_t l = strlen(line);
            if (n + l < sizeof g_play_hdr) { memcpy(g_play_hdr + n, line, l); n += l; g_play_hdr[n] = 0; }
        }
        char mine[2048]; header_lines(mine, sizeof mine);
        if (strcmp(mine, g_play_hdr)) {
            fprintf(stderr, "[rec] WARNING: %s was recorded with a different game, runtime or settings - the replay may go its own way\n"
                            "[rec]   recorded:\n%s[rec]   here:\n%s", path, g_play_hdr, mine);
            if (env_on("LINDBERGH_INPUT_PLAY_STRICT")) { fprintf(stderr, "[rec] LINDBERGH_INPUT_PLAY_STRICT: stopping\n"); exit(3); }
        }
        fprintf(stderr, "[rec] playing the cabinet's input back from %s\n", path);
    }
}
static void rec_write(uint32_t tick, const CabinetInput *c)
{
    RecFrame f = { tick, c->buttons, { (uint16_t)c->coins[0], (uint16_t)c->coins[1] }, c->gun_x, c->gun_y, c->gun2_x, c->gun2_y, (uint8_t)c->gun2, { 0, 0, 0 } };
    fwrite(&f, sizeof f, 1, g_rec);
    if (tick % 60 == 0) fflush(g_rec);
}
static int play_read(uint32_t tick, CabinetInput *c)
{
    RecFrame f;
    if (g_play_done || fread(&f, sizeof f, 1, g_play) != 1) {
        if (!g_play_done) {
            g_play_done = 1;
            fprintf(stderr, "[rec] the recording ends at frame %u%s\n", tick - 1, g_play_exit ? ": stopping (LINDBERGH_INPUT_PLAY_EXIT)" : ", nothing pressed from here");
            if (g_play_exit) { fflush(stderr); _exit(0); }
        }
        return 0;
    }
    if (f.tick != tick && g_tick_log) fprintf(stderr, "[rec] frame %u: the recording says %u\n", tick, f.tick);
    c->buttons = f.buttons; c->coins[0] = f.coins[0]; c->coins[1] = f.coins[1];
    c->gun_x = f.gx; c->gun_y = f.gy; c->gun2_x = f.g2x; c->gun2_y = f.g2y; c->gun2 = f.gun2;
    c->screen_w = c->screen_h = 65535;
    return 1;
}

/* ================= netplay: the session over UDP ================= */
enum { NP_OFF, NP_HOSTING, NP_JOINING, NP_RUN, NP_ENDED };
static struct {
    int      phase, host, fd;
    struct sockaddr_storage peer; socklen_t peer_len; int have_peer;
    pthread_t thread; int thread_on, stop;
    pthread_mutex_t lock; pthread_cond_t cond;
    LsSession s;
    LsHello hello;
    char     refused[512];
    uint32_t start, nonce; unsigned delay;
    double   last_send_ms, timeout_ms, wait_ms;
    /* stats */
    unsigned stalls, stall_frames; double stall_ms, stall_max_ms;
    unsigned delay_changes;
    char     end_why[160];
} N = { .fd = -1, .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

int netplay_active(void) { return N.phase == NP_RUN; }
/* A frame waited for the other player: the host's frame pacer starts its
 * 60 Hz grid again from now (host_sdl.c pace), or the side that waited would
 * wait the same few ms every frame from then on (seen after a 350 ms stall:
 * every later frame waited ~15 ms, the two grids a frame apart). */
static volatile int g_stalled;
int netplay_take_stalled(void) { int s = g_stalled; g_stalled = 0; return s; }

/* LINDBERGH_NETSIM: our own sends delayed, jittered, lost, reordered */
static struct {
    int on; double delay, jitter, loss, reorder, stall_at, stall_len;
    uint32_t rng;
    struct { uint8_t b[LS_PACKET_MAX]; size_t n; double due; } q[512]; int nq;
    double t0;
} S;
static uint32_t sim_rnd(void) { S.rng ^= S.rng << 13; S.rng ^= S.rng >> 17; S.rng ^= S.rng << 5; return S.rng; }
static double sim_unit(void) { return (sim_rnd() & 0xFFFFFF) / (double)0x1000000; }

#ifdef NP_SOCKETS
static void raw_send(const uint8_t *b, size_t n)
{
    if (N.fd < 0 || !N.have_peer) return;
    sendto(N.fd, b, n, 0, (struct sockaddr *)&N.peer, N.peer_len);
}
/* called with N.lock held */
static void net_send(const uint8_t *b, size_t n)
{
    if (!n) return;
    N.last_send_ms = now_ms();
    if (!S.on) { raw_send(b, n); return; }
    double now = N.last_send_ms;
    if (sim_unit() * 100.0 < S.loss) return;
    double d = S.delay + (S.jitter > 0 ? (sim_unit() * 2 - 1) * S.jitter : 0);
    if (sim_unit() * 100.0 < S.reorder) d += S.delay + S.jitter + 4;
    if (d < 0) d = 0;
    double due = now + d;
    /* a stall (LINDBERGH_NETSIM_STALL=at_ms,len_ms after the start): held until it ends, as Wi-Fi power save holds them */
    if (S.stall_len > 0 && now - S.t0 >= S.stall_at && now - S.t0 < S.stall_at + S.stall_len) due = S.t0 + S.stall_at + S.stall_len + d;
    if (S.nq == 512) return;
    memcpy(S.q[S.nq].b, b, n); S.q[S.nq].n = n; S.q[S.nq].due = due; S.nq++;
}
static double sim_flush(void)        /* with N.lock held; ms until the next one is due */
{
    double now = now_ms(), next = 1e9;
    for (int i = 0; i < S.nq; ) {
        if (S.q[i].due <= now) { raw_send(S.q[i].b, S.q[i].n); S.q[i] = S.q[--S.nq]; }
        else { if (S.q[i].due - now < next) next = S.q[i].due - now; i++; }
    }
    return next;
}

static void send_input_locked(void)
{
    uint8_t b[LS_PACKET_MAX];
    size_t n = ls_write_input(&N.s, b, sizeof b, now_us(), g_tick);
    net_send(b, n);
}

static void end_session_locked(int bye_reason, const char *why)
{
    if (N.phase != NP_RUN && N.phase != NP_HOSTING && N.phase != NP_JOINING) return;
    if (bye_reason && N.phase == NP_RUN) {
        uint8_t b[64]; size_t n = ls_write_bye(b, sizeof b, N.s.nonce, bye_reason);
        for (int i = 0; i < 3; i++) raw_send(b, n);    /* no simulator for the goodbye: it should get there */
    }
    snprintf(N.end_why, sizeof N.end_why, "%s", why);
    N.phase = NP_ENDED;
    pthread_cond_broadcast(&N.cond);
}

/* a packet in, with N.lock held */
static void handle_packet(const uint8_t *b, size_t n, const struct sockaddr_storage *from, socklen_t flen)
{
    int type = ls_type(b, n);
    if (!type) return;
    if (N.phase == NP_HOSTING && type == LS_HELLO) {
        LsHello theirs; char why[512];
        if (!ls_read_hello(b, n, &theirs)) return;
        ls_compare_hello(&N.hello, &theirs, why, sizeof why);
        memcpy(&N.peer, from, flen); N.peer_len = flen; N.have_peer = 1;
        uint8_t out[LS_PACKET_MAX]; size_t on;
        if (why[0]) {
            fprintf(stderr, "[net] refused a player: %s\n", why);
            on = ls_write_refuse(out, sizeof out, why);
            raw_send(out, on);
            N.have_peer = 0;
            return;
        }
        on = ls_write_welcome(out, sizeof out, N.nonce, N.delay, N.start);
        raw_send(out, on);
        ls_init(&N.s, 1, N.nonce, N.start, N.delay);
        N.phase = NP_RUN;
        char addr[64] = "?";
        if (from->ss_family == AF_INET) inet_ntop(AF_INET, &((const struct sockaddr_in *)from)->sin_addr, addr, sizeof addr);
        fprintf(stderr, "[net] player 2 joined from %s: lockstep from frame %u, input delay %u\n", addr, N.start, N.delay);
        pthread_cond_broadcast(&N.cond);
        return;
    }
    if (N.phase == NP_RUN && N.host && type == LS_HELLO) {     /* our WELCOME was lost: again */
        uint8_t out[LS_PACKET_MAX]; size_t on = ls_write_welcome(out, sizeof out, N.nonce, N.delay, N.start);
        raw_send(out, on);
        return;
    }
    if (N.phase == NP_JOINING && type == LS_WELCOME) {
        uint32_t nonce, start; unsigned d;
        if (!ls_read_welcome(b, n, &nonce, &d, &start)) return;
        N.nonce = nonce; N.delay = d; N.start = start;
        ls_init(&N.s, 0, nonce, start, d);
        N.phase = NP_RUN;
        fprintf(stderr, "[net] joined as player 2: lockstep from frame %u, input delay %u\n", start, d);
        pthread_cond_broadcast(&N.cond);
        return;
    }
    if (N.phase == NP_JOINING && type == LS_REFUSE) {
        ls_read_refuse(b, n, N.refused, sizeof N.refused);
        fprintf(stderr, "[net] the host refused us: %s\n", N.refused);
        end_session_locked(0, "refused by the host");
        return;
    }
    if (N.phase == NP_RUN && (type == LS_INPUT || type == LS_BYE)) {
        if (ls_receive(&N.s, b, n, now_us())) {
            if (N.s.bye) {
                const char *w = N.s.bye == LS_BYE_DESYNC ? "the other side found the games out of sync"
                              : N.s.bye == LS_BYE_TIMEOUT ? "the other side lost us" : "the other player left";
                fprintf(stderr, "[net] frame %u: %s - playing on alone\n", g_tick, w);
                end_session_locked(0, w);
            }
            pthread_cond_broadcast(&N.cond);
        }
    }
}

static void *net_thread(void *arg)
{
    (void)arg;
    double last_hello = 0;
    while (!N.stop) {
        pthread_mutex_lock(&N.lock);
        double next = S.on ? sim_flush() : 1e9;
        if (N.phase == NP_JOINING && now_ms() - last_hello > 250) {
            uint8_t b[LS_PACKET_MAX]; size_t n = ls_write_hello(b, sizeof b, &N.hello);
            raw_send(b, n);                  /* the handshake goes straight out */
            last_hello = now_ms();
        }
        /* a long frame on our side (loading): keep the peer told */
        if (N.phase == NP_RUN && now_ms() - N.last_send_ms > 50) send_input_locked();
        int phase = N.phase;
        pthread_mutex_unlock(&N.lock);
        if (phase == NP_ENDED || phase == NP_OFF) break;
        int timeout = next < 5 ? (int)next : 5;
        struct pollfd p = { N.fd, POLLIN, 0 };
        if (poll(&p, 1, timeout) > 0 && (p.revents & POLLIN)) {
            uint8_t b[2048];
            for (;;) {
                struct sockaddr_storage from; socklen_t fl = sizeof from;
                ssize_t n = recvfrom(N.fd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
                if (n <= 0) break;
                pthread_mutex_lock(&N.lock);
                handle_packet(b, (size_t)n, &from, fl);
                pthread_mutex_unlock(&N.lock);
            }
        }
    }
    pthread_mutex_lock(&N.lock);
    if (S.on) { double t = now_ms(); while (S.nq && now_ms() - t < 200) { sim_flush(); } }
    pthread_mutex_unlock(&N.lock);
    return NULL;
}

static int env_num(const char *k, int def) { const char *v = getenv(k); return v && *v ? atoi(v) : def; }

static void np_net_init(void)
{
    if (!g_net[0]) return;
    char spec[256]; snprintf(spec, sizeof spec, "%s", g_net);
    int port = 47100;
    char host[128] = "";
    if (!strncmp(spec, "host", 4)) {
        N.host = 1;
        if (spec[4] == ':') port = atoi(spec + 5);
    } else if (!strncmp(spec, "join:", 5)) {
        N.host = 0;
        char *a = spec + 5, *c = strrchr(a, ':');
        if (c) { *c = 0; port = atoi(c + 1); }
        snprintf(host, sizeof host, "%s", a);
    } else {
        fprintf(stderr, "[net] LINDBERGH_NETPLAY=%s: want host[:port] or join:<ip>[:port] - netplay off\n", g_net);
        return;
    }
    N.timeout_ms = env_num("LINDBERGH_NETPLAY_TIMEOUT", 10000);
    N.wait_ms = env_num("LINDBERGH_NETPLAY_WAIT", 120) * 1000.0;
    N.delay = (unsigned)env_num("LINDBERGH_NETPLAY_DELAY", 2);
    N.start = 1;
    const char *sim = getenv("LINDBERGH_NETSIM");
    if (sim && *sim) {
        double v[4] = { 0, 0, 0, 0 };
        sscanf(sim, "%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3]);
        S.on = 1; S.delay = v[0]; S.jitter = v[1]; S.loss = v[2]; S.reorder = v[3];
        const char *st = getenv("LINDBERGH_NETSIM_STALL");
        if (st && *st) sscanf(st, "%lf,%lf", &S.stall_at, &S.stall_len);
        S.rng = (uint32_t)env_num("LINDBERGH_NETSIM_SEED", 0);
        if (!S.rng) S.rng = (uint32_t)getpid() * 2654435761u;
        if (!S.rng) S.rng = 1;
        fprintf(stderr, "[net] network simulator: %.0f ms delay, %.0f ms jitter, %.1f%% loss, %.1f%% reordered%s\n",
                S.delay, S.jitter, S.loss, S.reorder, S.stall_len > 0 ? ", one stall" : "");
    }
    N.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (N.fd < 0) { fprintf(stderr, "[net] no UDP socket: %s - netplay off\n", strerror(errno)); return; }
    fcntl(N.fd, F_SETFL, fcntl(N.fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in me; memset(&me, 0, sizeof me);
    me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port = htons((uint16_t)(N.host ? port : 0));
    if (bind(N.fd, (struct sockaddr *)&me, sizeof me) < 0) {
        fprintf(stderr, "[net] cannot listen on UDP port %d: %s - netplay off\n", port, strerror(errno));
        close(N.fd); N.fd = -1; return;
    }
    /* the handshake */
    N.hello.proto = (uint32_t)env_num("LINDBERGH_NETPLAY_TEST_PROTO", LS_PROTO);
    memcpy(N.hello.elf_sha, g_elf_sha, 32);
    snprintf(N.hello.runtime, sizeof N.hello.runtime, "%s", LINDBERGH_RUNTIME_REV);
    settings_string(N.hello.settings, sizeof N.hello.settings);
    N.hello.want_delay = (uint8_t)N.delay;
    if (N.host) {
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
        N.nonce = (uint32_t)(t.tv_nsec ^ (t.tv_sec << 12) ^ (uint32_t)getpid());
        if (!N.nonce) N.nonce = 1;
        N.phase = NP_HOSTING;
        fprintf(stderr, "[net] hosting on UDP port %d: waiting up to %.0f s for player 2 (settings %s)\n", port, N.wait_ms / 1000, N.hello.settings);
    } else {
        struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        char ps[16]; snprintf(ps, sizeof ps, "%d", port);
        if (getaddrinfo(host, ps, &hints, &res) || !res) {
            fprintf(stderr, "[net] cannot find the host '%s' - netplay off\n", host);
            close(N.fd); N.fd = -1; return;
        }
        memcpy(&N.peer, res->ai_addr, res->ai_addrlen); N.peer_len = (socklen_t)res->ai_addrlen; N.have_peer = 1;
        freeaddrinfo(res);
        N.phase = NP_JOINING;
        fprintf(stderr, "[net] joining %s:%d as player 2 (settings %s)\n", host, port, N.hello.settings);
    }
    if (pthread_create(&N.thread, NULL, net_thread, NULL) == 0) N.thread_on = 1;
    /* wait for the other side here, before the first frame: both start together */
    double t0 = now_ms();
    pthread_mutex_lock(&N.lock);
    while (N.phase == NP_HOSTING || N.phase == NP_JOINING) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&N.cond, &N.lock, &ts);
        if (now_ms() - t0 > N.wait_ms && (N.phase == NP_HOSTING || N.phase == NP_JOINING)) {
            fprintf(stderr, "[net] %s in %.0f s - playing alone\n", N.host ? "nobody joined" : "no answer from the host", N.wait_ms / 1000);
            end_session_locked(0, "nobody came");
        }
    }
    S.t0 = now_ms();
    if (N.phase == NP_RUN) fprintf(stderr, "[net] session %08x running after %.0f ms\n", N.nonce, now_ms() - t0);
    pthread_mutex_unlock(&N.lock);
}

static void side_input(const CabinetInput *c, LsInput *o, int host)
{
    const unsigned p2 = CAB_P2_START | CAB_P2_TRIGGER | CAB_P2_RELOAD | CAB_P2_ACTION | CAB_P2_FIREMODE |
                        CAB_P2_UP | CAB_P2_DOWN | CAB_P2_LEFT | CAB_P2_RIGHT | CAB_P2_BUTTON2 | CAB_P2_BUTTON3;
    const unsigned gun = CAB_P1_TRIGGER | CAB_P1_RELOAD | CAB_P1_ACTION | CAB_P1_FIREMODE;
    const unsigned joy = CAB_P1_UP | CAB_P1_DOWN | CAB_P1_LEFT | CAB_P1_RIGHT | CAB_P1_BUTTON2 | CAB_P1_BUTTON3;
    unsigned b = c->buttons;
    if (host) o->buttons = b & ~p2;
    else o->buttons = (b & CAB_P1_START ? CAB_P2_START : 0) | (b & gun) << CAB_P2_SHIFT | (b & joy) << CAB_P2_JOY_SHIFT | (b & p2);
    o->coins[0] = (uint16_t)c->coins[0]; o->coins[1] = (uint16_t)c->coins[1];
    o->gx = c->gun_x; o->gy = c->gun_y; o->gun = 1;
}

static void net_log_stats(void)
{
    LsSession *s = &N.s;
    fprintf(stderr, "[net] frame %u: delay %u, rtt %.1f ms (min %.1f, max %.1f, jitter %.1f), stalls %u (%u frames, %.0f ms, longest %.0f ms), "
                    "sent %u, received %u, repeated inputs %u, hashes compared %u\n",
            g_tick, ls_delay(s, g_tick), s->srtt_ms, s->rtt_samples ? s->rtt_min_ms : 0.0, s->rtt_max_ms, s->rttvar_ms,
            N.stalls, N.stall_frames, N.stall_ms, N.stall_max_ms, s->sent, s->received, s->dup_inputs, s->hashes_compared);
}

/* one frame of lockstep: my input out, both sides' in (the merged input in *c) */
static void net_tick(CabinetInput *c)
{
    pthread_mutex_lock(&N.lock);
    if (N.phase != NP_RUN) { pthread_mutex_unlock(&N.lock); return; }
    LsInput mine;
    side_input(c, &mine, N.host);
    unsigned d_before = ls_delay(&N.s, g_tick);
    if (N.host && ls_adapt(&N.s, g_tick)) {
        N.delay_changes++;
        fprintf(stderr, "[net] frame %u: input delay %u -> %u from frame %u (rtt %.1f ms, jitter %.1f)\n",
                g_tick, d_before, N.s.sched_d, N.s.sched_at, N.s.srtt_ms, N.s.rttvar_ms);
    }
    if (!N.host && ls_delay(&N.s, g_tick) != d_before) N.delay_changes++;
    ls_local(&N.s, g_tick, &mine);
    send_input_locked();
    LsInput a, b;
    double t0 = now_ms(), last = t0;
    int stalled = 0;
    while (N.phase == NP_RUN && !(ls_mine(&N.s, g_tick, &a) && ls_theirs(&N.s, g_tick, &b))) {
        stalled = 1;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&N.cond, &N.lock, &ts);
        double now = now_ms();
        if (now - last >= 8) { send_input_locked(); last = now; }
        if (now - t0 > N.timeout_ms) {
            fprintf(stderr, "[net] frame %u: nothing from the other player for %.0f ms - session ended, playing on alone\n", g_tick, now - t0);
            end_session_locked(LS_BYE_TIMEOUT, "the other player stopped answering");
        }
    }
    if (stalled) {
        double ms = now_ms() - t0;
        N.stalls++; N.stall_ms += ms; if (ms > N.stall_max_ms) N.stall_max_ms = ms;
        if (ms > 2) { N.stall_frames++; g_stalled = 1; }
        if (ms > 100) fprintf(stderr, "[net] frame %u: waited %.0f ms for the other player\n", g_tick, ms);
    }
    if (N.phase == NP_RUN) {
        LsInput m;
        if (N.host) ls_merge(&a, &b, &m); else ls_merge(&b, &a, &m);
        const LsInput *p2 = N.host ? &b : &a;
        c->buttons = m.buttons;
        c->coins[0] = m.coins[0]; c->coins[1] = m.coins[1];
        c->gun_x = m.gx; c->gun_y = m.gy;
        c->gun2 = p2->gx >= 0; c->gun2_x = p2->gx; c->gun2_y = p2->gy;
        c->screen_w = c->screen_h = 65535;
    }
    if (g_tick % 600 == 0 && N.phase == NP_RUN) net_log_stats();
    pthread_mutex_unlock(&N.lock);
}

static void net_hash(uint32_t tick, uint64_t h)
{
    pthread_mutex_lock(&N.lock);
    if (N.phase == NP_RUN) {
        ls_note_hash(&N.s, tick, h);
        if (N.s.desync_tick) {
            fprintf(stderr, "[net] DESYNC: the two games differ at frame %u (hash compared every %u frames) - session ended, playing on alone\n",
                    N.s.desync_tick, g_hash_every);
            net_log_stats();
            end_session_locked(LS_BYE_DESYNC, "out of sync");
        }
    }
    pthread_mutex_unlock(&N.lock);
}
static void net_check_desync(void)
{
    pthread_mutex_lock(&N.lock);
    if (N.phase == NP_RUN && N.s.desync_tick) {
        fprintf(stderr, "[net] DESYNC: the two games differ at frame %u - session ended, playing on alone\n", N.s.desync_tick);
        net_log_stats();
        end_session_locked(LS_BYE_DESYNC, "out of sync");
    }
    pthread_mutex_unlock(&N.lock);
}
#else
static void net_tick(CabinetInput *c) { (void)c; }
static void net_hash(uint32_t t, uint64_t h) { (void)t; (void)h; }
static void net_check_desync(void) {}
static void np_net_init(void) { if (g_net[0]) fprintf(stderr, "[net] netplay is not built for this platform\n"); }
#endif

/* ================= the cabinet ================= */
static void first_tick(void)
{
    rec_open();
    np_net_init();
#ifdef NP_SOCKETS
    if (g_net[0] && N.phase == NP_RUN && !g_hash_every) g_hash_every = (unsigned)env_num("LINDBERGH_NETPLAY_HASH_EVERY", 30);
    if (N.phase == NP_RUN) atexit(net_log_stats);
#endif
}

static void (*g_barrier)(void);
void netplay_frame_barrier(void (*fn)(void)) { g_barrier = fn; }
static uint64_t (*g_shash)(char *, unsigned);
void netplay_semantic_hash(uint64_t (*fn)(char *what, unsigned cap)) { g_shash = fn; }

void cab_switch_read(CabinetInput *out)
{
    np_init();
    if (!g_layer && !g_hash_every && !g_tick_log && !g_det) { host_cabinet_input(out); return; }
    if (g_tick == 0) first_tick();
    g_tick++;
    if (g_tick_log && (g_tick < 5 || g_tick % 600 == 0))
        fprintf(stderr, "[tick] input frame %u at display frame %u (thread %lu)\n", g_tick, g_frame, (unsigned long)(uintptr_t)pthread_self());
    if (g_det && g_barrier) g_barrier();
    /* the state at the start of this frame, before its input */
    if (g_hash_every && g_tick % g_hash_every == 0) {
        uint64_t h = state_hash(g_tick), sh = 0;
        fprintf(stderr, "[hash] frame %u: %016llx\n", g_tick, (unsigned long long)h);
        if (g_shash) {
            char what[512] = "";
            sh = g_shash(what, sizeof what);
            fprintf(stderr, "[shash] frame %u: %016llx %s\n", g_tick, (unsigned long long)sh, what);
        }
        if (g_net[0]) {
            const char *m = getenv("LINDBERGH_NETPLAY_HASH");
            net_hash(g_tick, g_shash && !(m && !strcmp(m, "mem")) ? sh : h);
        }
    }
    if (!g_layer) { host_cabinet_input(out); return; }
    CabinetInput c;
    host_cabinet_input(&c);
    canonical(&c);
    if (g_play) { if (!play_read(g_tick, &c)) { memset(&c, 0, sizeof c); c.screen_w = c.screen_h = 65535; } }
    else if (g_net[0]) { net_tick(&c); net_check_desync(); }
    if (g_rec) rec_write(g_tick, &c);
    g_cur = c;
    *out = c;
}

void cab_input(CabinetInput *out)
{
    np_init();
    if (!g_layer) { host_cabinet_input(out); return; }
    if (!g_cur.screen_w) g_cur.screen_w = g_cur.screen_h = 65535;
    *out = g_cur;
}

unsigned cab_buttons(void)
{
    np_init();
    if (!g_layer) return host_cabinet_buttons();
    return g_cur.buttons;
}
