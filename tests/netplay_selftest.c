/*
 * netplay_selftest.c - the lockstep protocol (src/runtime/lockstep.c) without
 * a network or a game: two sessions joined by a simulated link with delay,
 * loss and reordering. Built and run by test_netplay.py.
 */
#include "lockstep.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_sha(void)
{
    uint8_t h[32];
    ls_sha256("abc", 3, h);
    static const uint8_t want[4] = { 0xba, 0x78, 0x16, 0xbf };
    CHECK(!memcmp(h, want, 4));
    ls_sha256("", 0, h);
    CHECK(h[0] == 0xe3 && h[1] == 0xb0);
}

static LsInput in_of(uint32_t b) { LsInput i; memset(&i, 0, sizeof i); i.buttons = b; return i; }

static void test_delay_assignment(void)
{
    LsSession s; ls_init(&s, 1, 7, 1, 2);
    LsInput o;
    CHECK(ls_mine(&s, 1, &o) && o.buttons == 0);
    CHECK(ls_mine(&s, 2, &o) && o.buttons == 0);
    CHECK(!ls_mine(&s, 3, &o));
    CHECK(ls_local(&s, 1, &(LsInput){ .buttons = 0x10 }) == 3);
    CHECK(ls_local(&s, 2, &(LsInput){ .buttons = 0x20 }) == 4);
    /* up to 4 from frame 3: frames 5 and 6 repeat frame 4's input, sample 3 -> 7 */
    ls_schedule_delay(&s, 3, 4);
    CHECK(ls_local(&s, 3, &(LsInput){ .buttons = 0x30 }) == 7);
    CHECK(ls_mine(&s, 5, &o) && o.buttons == 0x20);
    CHECK(ls_mine(&s, 6, &o) && o.buttons == 0x20);
    /* down to 1 from frame 4: samples 4, 5 -> 5, 6 are taken already; 6 -> 7 too; 7 -> 8 */
    ls_schedule_delay(&s, 4, 1);
    CHECK(ls_local(&s, 4, &(LsInput){ .buttons = 0x40 }) == 0);
    CHECK(ls_local(&s, 5, &(LsInput){ .buttons = 0x50 }) == 0);
    CHECK(ls_local(&s, 6, &(LsInput){ .buttons = 0x60 }) == 0);
    CHECK(ls_local(&s, 7, &(LsInput){ .buttons = 0x70 }) == 8);
    CHECK(ls_delay(&s, 3) == 4 && ls_delay(&s, 10) == 1 && ls_delay(&s, 2) == 2);
}

/* ---- a simulated link: packets with a release time, loss and reordering ---- */
typedef struct { uint8_t b[LS_PACKET_MAX]; size_t n; uint32_t at; int to; } Pkt;
static Pkt q[4096]; static int nq;
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void send_pkt(const uint8_t *b, size_t n, uint32_t now, int to, int delay_us, int jitter_us, int loss_pct, int reorder_pct)
{
    if ((int)(rnd() % 100) < loss_pct) return;
    if (nq == 4096) return;
    int d = delay_us + (jitter_us ? (int)(rnd() % (2 * jitter_us + 1)) - jitter_us : 0);
    if ((int)(rnd() % 100) < reorder_pct) d += delay_us + jitter_us + 3000;
    if (d < 0) d = 0;
    memcpy(q[nq].b, b, n); q[nq].n = n; q[nq].at = now + (uint32_t)d; q[nq].to = to; nq++;
}
static void deliver(LsSession *side[2], uint32_t now)
{
    for (int i = 0; i < nq; ) {
        if ((int32_t)(now - q[i].at) >= 0) {
            ls_receive(side[q[i].to], q[i].b, q[i].n, now);
            q[i] = q[--nq];
        } else i++;
    }
}

/* Two copies play `frames` frames, each frame 16.7 ms, a copy stalling when
 * the other's input for the frame is not in. Returns the frames both played;
 * *stalls counts the 1 ms steps spent waiting. Both copies must see the same
 * merged input every frame. */
static int play(int frames, int delay_us, int jitter_us, int loss, int reorder, int adapt, unsigned *stalls, unsigned *final_delay)
{
    static LsSession A, B; LsSession *side[2] = { &A, &B };
    ls_init(&A, 1, 99, 1, 1); ls_init(&B, 0, 99, 1, 1);
    nq = 0;
    uint32_t tick[2] = { 1, 1 }, next[2] = { 0, 500 }, now = 0;
    uint32_t merged[2][4096]; memset(merged, 0, sizeof merged);
    int sampled[2] = { 0, 0 };
    *stalls = 0;
    while ((tick[0] <= (uint32_t)frames || tick[1] <= (uint32_t)frames) && now < 600000000u) {
        for (int k = 0; k < 2; k++) {
            if (tick[k] > (uint32_t)frames || (int32_t)(now - next[k]) < 0) continue;
            LsSession *s = side[k];
            if (!sampled[k]) {                      /* this frame's own input, once */
                LsInput in = in_of(k ? (tick[k] * 7u) << 8 : tick[k] * 3u);
                ls_local(s, tick[k], &in);
                if (adapt) ls_adapt(s, tick[k]);
                sampled[k] = 1;
                uint8_t b[LS_PACKET_MAX]; size_t n = ls_write_input(s, b, sizeof b, now, tick[k]);
                send_pkt(b, n, now, !k, delay_us, jitter_us, loss, reorder);
            }
            LsInput mine, theirs, m;
            if (!ls_mine(s, tick[k], &mine) || !ls_theirs(s, tick[k], &theirs)) {
                (*stalls)++;
                if (now % 4000 == 0) {              /* resend while waiting */
                    uint8_t b[LS_PACKET_MAX]; size_t n = ls_write_input(s, b, sizeof b, now, tick[k]);
                    send_pkt(b, n, now, !k, delay_us, jitter_us, loss, reorder);
                }
                continue;
            }
            if (k == 0) ls_merge(&mine, &theirs, &m); else ls_merge(&theirs, &mine, &m);
            if (tick[k] < 4096) merged[k][tick[k]] = m.buttons;
            if (tick[k] % 60 == 0) ls_note_hash(s, tick[k], m.buttons * 0x9E3779B97F4A7C15ull);
            tick[k]++; sampled[k] = 0;
            next[k] += 16683;
        }
        now += 1000;
        deliver(side, now);
    }
    int same = 1;
    for (int f = 1; f <= frames && f < 4096; f++) if (merged[0][f] != merged[1][f]) same = 0;
    CHECK(same);
    CHECK(A.desync_tick == 0 && B.desync_tick == 0);
    CHECK(A.hashes_compared > 0 || frames < 120);
    *final_delay = ls_delay(&A, tick[0] - 1);
    CHECK(ls_delay(&B, tick[1] - 1) == *final_delay);
    return (int)(tick[0] < tick[1] ? tick[0] : tick[1]) - 1;
}

static void test_links(void)
{
    unsigned stalls, d;
    CHECK(play(600, 200, 0, 0, 0, 1, &stalls, &d) == 600);
    printf("clean: stalls %u ms, delay %u\n", stalls, d);
    CHECK(d == 1);
    CHECK(play(1200, 20000, 5000, 0, 0, 1, &stalls, &d) == 1200);
    printf("20 ms / 5 ms jitter: stalls %u ms, delay %u\n", stalls, d);
    CHECK(d >= 2 && d <= 3);
    CHECK(play(1200, 60000, 0, 0, 0, 1, &stalls, &d) == 1200);
    printf("60 ms: stalls %u ms, delay %u\n", stalls, d);
    CHECK(d >= 4 && d <= 5);
    CHECK(play(1200, 2000, 500, 10, 0, 1, &stalls, &d) == 1200);
    printf("10%% loss: stalls %u ms, delay %u\n", stalls, d);
    CHECK(play(1200, 3000, 1000, 2, 20, 1, &stalls, &d) == 1200);
    printf("2%% loss, 20%% reordered: stalls %u ms, delay %u\n", stalls, d);
}

static void test_hello(void)
{
    LsHello a, b; memset(&a, 0, sizeof a);
    a.proto = LS_PROTO; memset(a.elf_sha, 0xab, 32); snprintf(a.runtime, sizeof a.runtime, "abc1234");
    snprintf(a.settings, sizeof a.settings, "replays=1;cutaways=1;immersive=0;");
    a.want_delay = 2;
    uint8_t buf[LS_PACKET_MAX];
    size_t n = ls_write_hello(buf, sizeof buf, &a);
    CHECK(n > 0 && ls_type(buf, n) == LS_HELLO);
    CHECK(ls_read_hello(buf, n, &b));
    CHECK(!memcmp(&a.elf_sha, &b.elf_sha, 32) && !strcmp(a.settings, b.settings) && b.want_delay == 2);
    char why[512];
    ls_compare_hello(&a, &b, why, sizeof why);
    CHECK(why[0] == 0);
    snprintf(b.settings, sizeof b.settings, "replays=0;cutaways=1;immersive=0;");
    b.elf_sha[0] = 1;
    ls_compare_hello(&a, &b, why, sizeof why);
    CHECK(strstr(why, "replays=1 here, 0 there") && strstr(why, "executable"));
    CHECK(!ls_read_hello(buf, n - 3, &b));                 /* cut short */
    n = ls_write_refuse(buf, sizeof buf, "nope");
    CHECK(ls_read_refuse(buf, n, why, sizeof why) && !strcmp(why, "nope"));
    uint32_t nonce, start; unsigned d;
    n = ls_write_welcome(buf, sizeof buf, 42, 3, 1);
    CHECK(ls_read_welcome(buf, n, &nonce, &d, &start) && nonce == 42 && d == 3 && start == 1);
}

static void test_desync_and_bye(void)
{
    LsSession a, b; ls_init(&a, 1, 5, 1, 1); ls_init(&b, 0, 5, 1, 1);
    uint8_t buf[LS_PACKET_MAX]; size_t n;
    ls_note_hash(&a, 60, 111); ls_note_hash(&b, 60, 111);
    ls_note_hash(&a, 120, 222); ls_note_hash(&b, 120, 223);
    n = ls_write_input(&a, buf, sizeof buf, 1000, 120); CHECK(ls_receive(&b, buf, n, 2000));
    CHECK(b.desync_tick == 120);
    n = ls_write_input(&b, buf, sizeof buf, 3000, 120); CHECK(ls_receive(&a, buf, n, 4000));
    CHECK(a.desync_tick == 120);
    /* the other session's packets are not taken */
    LsSession c; ls_init(&c, 0, 6, 1, 1);
    CHECK(!ls_receive(&c, buf, n, 5000));
    n = ls_write_bye(buf, sizeof buf, 5, LS_BYE_DESYNC);
    CHECK(ls_receive(&a, buf, n, 6000) && a.bye == LS_BYE_DESYNC);
}

int main(void)
{
    test_sha();
    test_delay_assignment();
    test_hello();
    test_desync_and_bye();
    test_links();
    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    printf("netplay selftest: all passed\n");
    return 0;
}
