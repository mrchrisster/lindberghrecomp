/*
 * lockstep.c - the network input layer's protocol (lockstep.h).
 *
 * No sockets, no clock, no game: byte buffers in and out, times passed in.
 */
#include "lockstep.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define FRAME_MS 16.6833333

/* ---- SHA-256 (FIPS 180-4), for the game's executable ---- */
typedef struct { uint32_t h[8]; } ShaState;
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
static void sha_block(ShaState *s, const uint8_t *p)
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
void ls_sha256(const void *data, size_t n, uint8_t out[32])
{
    const uint8_t *p = (const uint8_t *)data;
    ShaState s = { { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 } };
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha_block(&s, p + i);
    uint8_t last[128] = {0};
    size_t rest = n - i;
    memcpy(last, p + i, rest);
    last[rest] = 0x80;
    size_t blocks = rest + 9 > 64 ? 2 : 1;
    uint64_t bits = (uint64_t)n * 8;
    for (int k = 0; k < 8; k++) last[blocks * 64 - 1 - k] = (uint8_t)(bits >> (8 * k));
    for (size_t k = 0; k < blocks; k++) sha_block(&s, last + 64 * k);
    for (int k = 0; k < 8; k++) {
        out[4 * k] = (uint8_t)(s.h[k] >> 24); out[4 * k + 1] = (uint8_t)(s.h[k] >> 16);
        out[4 * k + 2] = (uint8_t)(s.h[k] >> 8); out[4 * k + 3] = (uint8_t)s.h[k];
    }
}

/* FNV-1a over 64-bit words (bytes for the tail): fast enough for a few MB a
 * frame, and order-sensitive, which is all a desync check needs. */
uint64_t ls_hash64(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w; memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/* ---- byte writer / reader ---- */
typedef struct { uint8_t *p; size_t n, cap; int bad; } W;
static void w8(W *w, uint32_t v)  { if (w->n + 1 > w->cap) { w->bad = 1; return; } w->p[w->n++] = (uint8_t)v; }
static void w16(W *w, uint32_t v) { w8(w, v); w8(w, v >> 8); }
static void w32(W *w, uint32_t v) { w16(w, v); w16(w, v >> 16); }
static void w64(W *w, uint64_t v) { w32(w, (uint32_t)v); w32(w, (uint32_t)(v >> 32)); }
static void wbytes(W *w, const void *s, size_t n) { if (w->n + n > w->cap) { w->bad = 1; return; } memcpy(w->p + w->n, s, n); w->n += n; }
typedef struct { const uint8_t *p; size_t n, at; int bad; } R;
static uint32_t r8(R *r)  { if (r->at + 1 > r->n) { r->bad = 1; return 0; } return r->p[r->at++]; }
static uint32_t r16(R *r) { uint32_t a = r8(r); return a | r8(r) << 8; }
static uint32_t r32(R *r) { uint32_t a = r16(r); return a | r16(r) << 16; }
static uint64_t r64(R *r) { uint64_t a = r32(r); return a | (uint64_t)r32(r) << 32; }
static void rbytes(R *r, void *d, size_t n) { if (r->at + n > r->n) { r->bad = 1; memset(d, 0, n); return; } memcpy(d, r->p + r->at, n); r->at += n; }

static void header(W *w, int type)
{
    w32(w, LS_MAGIC); w8(w, LS_PROTO); w8(w, (uint32_t)type); w16(w, 0);   /* length filled in by finish */
}
static size_t finish(W *w)
{
    if (w->bad || w->n < 8) return 0;
    w->p[6] = (uint8_t)(w->n - 8); w->p[7] = (uint8_t)((w->n - 8) >> 8);
    return w->n;
}
/* a reader past our header, or bad if it is not ours */
static R open_packet(const uint8_t *buf, size_t len, int type)
{
    R r = { buf, len, 0, 0 };
    if (r32(&r) != LS_MAGIC) r.bad = 1;
    r8(&r);                              /* the sender's protocol: checked in the HELLO */
    if ((int)r8(&r) != type) r.bad = 1;
    uint32_t n = r16(&r);
    if (r.bad || n + 8 > len) r.bad = 1;
    else r.n = n + 8;
    return r;
}

int ls_type(const uint8_t *buf, size_t len)
{
    if (len < 8) return 0;
    R r = { buf, len, 0, 0 };
    if (r32(&r) != LS_MAGIC) return 0;
    r8(&r);
    return (int)r8(&r);
}

/* ---- session ---- */
#define SLOT(t) ((t) & (LS_WINDOW - 1u))

void ls_init(LsSession *s, int host, uint32_t nonce, uint32_t start, unsigned delay)
{
    memset(s, 0, sizeof *s);
    s->host = host;
    s->nonce = nonce;
    if (delay < 1) delay = 1;
    if (delay > LS_MAX_DELAY) delay = LS_MAX_DELAY;
    s->delay_at[0] = 0; s->delay_d[0] = (uint8_t)delay; s->ndelay = 1;
    s->rtt_min_ms = 1e9;
    /* frames start .. start + delay - 1 have no sample behind them: both sides
     * play them with nothing pressed */
    LsInput zero; memset(&zero, 0, sizeof zero);
    for (uint32_t f = start; f < start + delay; f++) { s->mine[SLOT(f)] = zero; s->mine_tick[SLOT(f)] = f; }
    s->assigned = start + delay - 1;
    s->have = start - 1;
    s->peer_ack = start - 1;
}

unsigned ls_delay(const LsSession *s, uint32_t tick)
{
    unsigned d = s->delay_d[0];
    for (unsigned i = 0; i < s->ndelay; i++) if (tick >= s->delay_at[i]) d = s->delay_d[i];
    return d;
}

static void add_delay(LsSession *s, uint32_t at, unsigned d)
{
    for (unsigned i = 0; i < s->ndelay; i++) if (s->delay_at[i] == at) { s->delay_d[i] = (uint8_t)d; return; }
    if (s->ndelay == 8) {                /* keep the newest: the oldest is in the past */
        memmove(s->delay_at, s->delay_at + 1, 7 * sizeof s->delay_at[0]);
        memmove(s->delay_d, s->delay_d + 1, 7);
        s->ndelay = 7;
    }
    unsigned i = s->ndelay++;
    while (i && s->delay_at[i - 1] > at) { s->delay_at[i] = s->delay_at[i - 1]; s->delay_d[i] = s->delay_d[i - 1]; i--; }
    s->delay_at[i] = at; s->delay_d[i] = (uint8_t)d;
}

uint32_t ls_local(LsSession *s, uint32_t tick, const LsInput *in)
{
    uint32_t target = tick + ls_delay(s, tick);
    if (target <= s->assigned) return 0;             /* the delay went down: this sample is dropped */
    LsInput prev = s->mine[SLOT(s->assigned)];
    if (s->mine_tick[SLOT(s->assigned)] != s->assigned) memset(&prev, 0, sizeof prev);
    for (uint32_t f = s->assigned + 1; f < target; f++) {   /* the delay went up: the gap holds the last input */
        s->mine[SLOT(f)] = prev; s->mine_tick[SLOT(f)] = f;
    }
    s->mine[SLOT(target)] = *in;
    s->mine_tick[SLOT(target)] = target;
    s->assigned = target;
    return target;
}

int ls_mine(const LsSession *s, uint32_t tick, LsInput *out)
{
    if (s->mine_tick[SLOT(tick)] != tick) return 0;
    if (out) *out = s->mine[SLOT(tick)];
    return 1;
}
int ls_theirs(const LsSession *s, uint32_t tick, LsInput *out)
{
    if (s->theirs_tick[SLOT(tick)] != tick) return 0;
    if (out) *out = s->theirs[SLOT(tick)];
    return 1;
}

void ls_merge(const LsInput *p1, const LsInput *p2, LsInput *out)
{
    LsInput m;
    m.buttons = p1->buttons | p2->buttons;
    m.coins[0] = (uint16_t)(p1->coins[0] + p2->coins[0]);
    m.coins[1] = (uint16_t)(p1->coins[1] + p2->coins[1]);
    m.gx = p1->gx; m.gy = p1->gy; m.gun = p1->gun;
    *out = m;
}

/* ---- packets ---- */
size_t ls_write_hello(uint8_t *buf, size_t cap, const LsHello *h)
{
    W w = { buf, 0, cap, 0 };
    header(&w, LS_HELLO);
    w32(&w, h->proto);
    wbytes(&w, h->elf_sha, 32);
    wbytes(&w, h->runtime, sizeof h->runtime);
    w8(&w, h->want_delay);
    size_t n = strnlen(h->settings, sizeof h->settings - 1);
    w16(&w, (uint32_t)n);
    wbytes(&w, h->settings, n);
    return finish(&w);
}
int ls_read_hello(const uint8_t *buf, size_t len, LsHello *h)
{
    R r = open_packet(buf, len, LS_HELLO);
    memset(h, 0, sizeof *h);
    h->proto = r32(&r);
    rbytes(&r, h->elf_sha, 32);
    rbytes(&r, h->runtime, sizeof h->runtime);
    h->runtime[sizeof h->runtime - 1] = 0;
    h->want_delay = (uint8_t)r8(&r);
    size_t n = r16(&r);
    if (n > sizeof h->settings - 1) { r.bad = 1; n = 0; }
    rbytes(&r, h->settings, n);
    h->settings[n] = 0;
    return !r.bad;
}
size_t ls_write_welcome(uint8_t *buf, size_t cap, uint32_t nonce, unsigned delay, uint32_t start)
{
    W w = { buf, 0, cap, 0 };
    header(&w, LS_WELCOME);
    w32(&w, nonce); w8(&w, delay); w32(&w, start);
    return finish(&w);
}
int ls_read_welcome(const uint8_t *buf, size_t len, uint32_t *nonce, unsigned *delay, uint32_t *start)
{
    R r = open_packet(buf, len, LS_WELCOME);
    *nonce = r32(&r); *delay = r8(&r); *start = r32(&r);
    return !r.bad;
}
size_t ls_write_refuse(uint8_t *buf, size_t cap, const char *why)
{
    W w = { buf, 0, cap, 0 };
    header(&w, LS_REFUSE);
    size_t n = strlen(why); if (n > 400) n = 400;
    w16(&w, (uint32_t)n); wbytes(&w, why, n);
    return finish(&w);
}
int ls_read_refuse(const uint8_t *buf, size_t len, char *why, size_t cap)
{
    R r = open_packet(buf, len, LS_REFUSE);
    size_t n = r16(&r);
    char tmp[401];
    if (n > 400) { r.bad = 1; n = 0; }
    rbytes(&r, tmp, n); tmp[n] = 0;
    snprintf(why, cap, "%s", tmp);
    return !r.bad;
}
size_t ls_write_query(uint8_t *buf, size_t cap)
{
    W w = { buf, 0, cap, 0 };
    header(&w, LS_QUERY);
    w32(&w, LS_PROTO);
    return finish(&w);
}
size_t ls_write_settings(uint8_t *buf, size_t cap, const char *name, const char *lines)
{
    W w = { buf, 0, cap, 0 };
    header(&w, LS_SETTINGS);
    w32(&w, LS_PROTO);
    size_t n = strlen(name); if (n > 40) n = 40;
    w8(&w, (uint32_t)n); wbytes(&w, name, n);
    n = strlen(lines); if (n > 420) n = 420;
    w16(&w, (uint32_t)n); wbytes(&w, lines, n);
    return finish(&w);
}
int ls_read_settings(const uint8_t *buf, size_t len, char *name, size_t ncap, char *lines, size_t lcap)
{
    R r = open_packet(buf, len, LS_SETTINGS);
    if (r32(&r) != LS_PROTO) return 0;
    char tmp[421];
    size_t n = r8(&r);
    if (n > 40) { r.bad = 1; n = 0; }
    rbytes(&r, tmp, n); tmp[n] = 0;
    snprintf(name, ncap, "%s", tmp);
    n = r16(&r);
    if (n > 420) { r.bad = 1; n = 0; }
    rbytes(&r, tmp, n); tmp[n] = 0;
    snprintf(lines, lcap, "%s", tmp);
    return !r.bad;
}
size_t ls_write_bye(uint8_t *buf, size_t cap, uint32_t nonce, int reason)
{
    W w = { buf, 0, cap, 0 };
    header(&w, LS_BYE);
    w32(&w, nonce); w8(&w, (uint32_t)reason);
    return finish(&w);
}

static void w_input(W *w, const LsInput *in)
{
    w32(w, in->buttons); w16(w, in->coins[0]); w16(w, in->coins[1]);
    w32(w, (uint32_t)in->gx); w32(w, (uint32_t)in->gy); w8(w, in->gun);
}
static void r_input(R *r, LsInput *in)
{
    in->buttons = r32(r); in->coins[0] = (uint16_t)r16(r); in->coins[1] = (uint16_t)r16(r);
    in->gx = (int32_t)r32(r); in->gy = (int32_t)r32(r); in->gun = (uint8_t)r8(r);
}

size_t ls_write_input(LsSession *s, uint8_t *buf, size_t cap, uint32_t now_us, uint32_t my_tick)
{
    W w = { buf, 0, cap, 0 };
    header(&w, LS_INPUT);
    w32(&w, s->nonce);
    w32(&w, now_us ? now_us : 1);
    w32(&w, s->peer_send_us);
    w32(&w, s->peer_send_us ? now_us - s->peer_send_at_us : 0);
    w32(&w, s->have);
    w32(&w, my_tick);
    w32(&w, s->host ? s->sched_at : 0); w8(&w, s->host ? s->sched_d : 0);
    w32(&w, s->host ? 0 : s->peer_sched_seen);
    uint32_t first = s->peer_ack + 1, n = s->assigned >= first ? s->assigned - first + 1 : 0;
    if (n > LS_REDUNDANT) n = LS_REDUNDANT;
    w32(&w, first); w8(&w, n);
    for (uint32_t i = 0; i < n; i++) {
        LsInput in; if (!ls_mine(s, first + i, &in)) memset(&in, 0, sizeof in);
        w_input(&w, &in);
    }
    unsigned nh = s->n_my_hash < LS_HASHES ? s->n_my_hash : LS_HASHES;
    w8(&w, nh);
    for (unsigned i = 0; i < nh; i++) {
        const LsHash *h = &s->my_hash[(s->n_my_hash - 1 - i) % (LS_HASHES * 4)];
        w32(&w, h->tick); w64(&w, h->hash);
    }
    size_t len = finish(&w);
    if (len) s->sent++;
    return len;
}

static void check_hash(LsSession *s, uint32_t tick, uint64_t mine, uint64_t theirs)
{
    s->hashes_compared++;
    if (mine != theirs && (!s->desync_tick || tick < s->desync_tick)) s->desync_tick = tick;
}
static const LsHash *find_my_hash(const LsSession *s, uint32_t tick)
{
    unsigned n = s->n_my_hash < LS_HASHES * 4 ? s->n_my_hash : LS_HASHES * 4;
    for (unsigned i = 0; i < n; i++) {
        const LsHash *h = &s->my_hash[(s->n_my_hash - 1 - i) % (LS_HASHES * 4)];
        if (h->tick == tick) return h;
    }
    return NULL;
}

void ls_note_hash(LsSession *s, uint32_t tick, uint64_t hash)
{
    s->my_hash[s->n_my_hash % (LS_HASHES * 4)] = (LsHash){ tick, hash };
    s->n_my_hash++;
    for (unsigned i = 0; i < s->n_peer_hash && i < LS_HASHES; i++)
        if (s->peer_hash[i].tick == tick) check_hash(s, tick, hash, s->peer_hash[i].hash);
}

int ls_receive(LsSession *s, const uint8_t *buf, size_t len, uint32_t now_us)
{
    int type = ls_type(buf, len);
    if (type == LS_BYE) {
        R r = open_packet(buf, len, LS_BYE);
        uint32_t nonce = r32(&r); int reason = (int)r8(&r);
        if (r.bad || nonce != s->nonce) { s->bad++; return 0; }
        s->bye = reason ? reason : LS_BYE_QUIT;
        return 1;
    }
    if (type != LS_INPUT) { s->bad++; return 0; }
    R r = open_packet(buf, len, LS_INPUT);
    uint32_t nonce = r32(&r), send_us = r32(&r), echo_us = r32(&r), hold_us = r32(&r);
    uint32_t ack = r32(&r), peer_tick = r32(&r);
    uint32_t sched_at = r32(&r); unsigned sched_d = r8(&r); uint32_t sched_seen = r32(&r);
    uint32_t first = r32(&r); unsigned n = r8(&r);
    LsInput in[LS_REDUNDANT];
    if (n > LS_REDUNDANT) { s->bad++; return 0; }
    for (unsigned i = 0; i < n; i++) r_input(&r, &in[i]);
    unsigned nh = r8(&r);
    LsHash hs[LS_HASHES];
    if (nh > LS_HASHES) { s->bad++; return 0; }
    for (unsigned i = 0; i < nh; i++) { hs[i].tick = r32(&r); hs[i].hash = r64(&r); }
    if (r.bad || nonce != s->nonce) { s->bad++; return 0; }
    s->received++;

    /* round trip: my clock came back after the peer held it hold_us */
    if (echo_us) {
        double rtt = (double)(uint32_t)(now_us - echo_us - hold_us) / 1000.0;
        if (rtt >= 0 && rtt < 5000) {
            if (!s->rtt_samples) { s->srtt_ms = rtt; s->rttvar_ms = rtt / 2; }
            else {
                s->rttvar_ms = 0.75 * s->rttvar_ms + 0.25 * fabs(s->srtt_ms - rtt);
                s->srtt_ms = 0.875 * s->srtt_ms + 0.125 * rtt;
            }
            if (rtt < s->rtt_min_ms) s->rtt_min_ms = rtt;
            if (rtt > s->rtt_max_ms) s->rtt_max_ms = rtt;
            s->rtt_samples++;
        }
    }
    /* the newest clock of the peer's I have seen, for echoing back */
    if ((int32_t)(send_us - s->peer_send_us) > 0 || !s->peer_send_us) { s->peer_send_us = send_us; s->peer_send_at_us = now_us; }
    if ((int32_t)(ack - s->peer_ack) > 0) s->peer_ack = ack;
    if ((int32_t)(peer_tick - s->peer_tick) > 0) s->peer_tick = peer_tick;

    for (unsigned i = 0; i < n; i++) {
        uint32_t f = first + i;
        if (f <= s->have || s->theirs_tick[SLOT(f)] == f) { s->dup_inputs++; continue; }
        if (f > s->have + LS_WINDOW - 1) { s->late_inputs++; continue; }   /* cannot be: they wait for me */
        s->theirs[SLOT(f)] = in[i]; s->theirs_tick[SLOT(f)] = f;
    }
    while (s->theirs_tick[SLOT(s->have + 1)] == s->have + 1) s->have++;

    if (!s->host && sched_at && sched_d) {           /* the host's delay schedule */
        if (s->peer_sched_seen != sched_at) add_delay(s, sched_at, sched_d);
        s->peer_sched_seen = sched_at;
    }
    if (s->host && sched_seen) s->peer_sched_seen = sched_seen;

    for (unsigned i = 0; i < nh; i++) {
        int seen = 0;
        for (unsigned k = 0; k < s->n_peer_hash && k < LS_HASHES; k++) if (s->peer_hash[k].tick == hs[i].tick) seen = 1;
        if (seen) continue;
        s->peer_hash[s->n_peer_hash % LS_HASHES] = hs[i];
        s->n_peer_hash++;
        const LsHash *m = find_my_hash(s, hs[i].tick);
        if (m) check_hash(s, hs[i].tick, m->hash, hs[i].hash);
    }
    return 1;
}

void ls_compare_hello(const LsHello *a, const LsHello *b, char *why, size_t cap)
{
    why[0] = 0;
    size_t n = 0;
#define ADD(...) do { if (n < cap) n += (size_t)snprintf(why + n, cap - n, __VA_ARGS__); } while (0)
    if (a->proto != b->proto) ADD("protocol %u here, %u there; ", a->proto, b->proto);
    if (memcmp(a->elf_sha, b->elf_sha, 32)) {
        ADD("a different game executable (SHA-256 %02x%02x%02x%02x... here, %02x%02x%02x%02x... there); ",
            a->elf_sha[0], a->elf_sha[1], a->elf_sha[2], a->elf_sha[3], b->elf_sha[0], b->elf_sha[1], b->elf_sha[2], b->elf_sha[3]);
    }
    if (strncmp(a->runtime, b->runtime, sizeof a->runtime)) ADD("runtime %s here, %s there; ", a->runtime, b->runtime);
    if (strcmp(a->settings, b->settings)) {
        /* name the settings that differ: "k=v;" items of one missing from the other */
        char ta[sizeof a->settings], tb[sizeof b->settings];
        snprintf(ta, sizeof ta, "%s", a->settings); snprintf(tb, sizeof tb, "%s", b->settings);
        ADD("settings differ:");
        for (char *it = ta, *e; it && *it; it = e) {
            e = strchr(it, ';'); if (e) *e++ = 0;
            char key[96]; snprintf(key, sizeof key, "%s", it);
            char *eq = strchr(key, '='); if (eq) eq[1] = 0;
            const char *there = strstr(b->settings, key);
            char tv[96] = "(not set)";
            if (there) { snprintf(tv, sizeof tv, "%s", there + strlen(key)); char *x = strchr(tv, ';'); if (x) *x = 0; }
            char full[200]; snprintf(full, sizeof full, "%s", it);
            int same = 0;
            if (there) { char item[200]; snprintf(item, sizeof item, "%s%s", key, tv); same = !strcmp(item, full); }
            if (!same) ADD(" %s here, %s there;", full, tv);
        }
        for (char *it = tb, *e; it && *it; it = e) {
            e = strchr(it, ';'); if (e) *e++ = 0;
            char key[96]; snprintf(key, sizeof key, "%s", it);
            char *eq = strchr(key, '='); if (eq) eq[1] = 0;
            if (!strstr(a->settings, key)) ADD(" %s there, not set here;", it);
        }
    }
#undef ADD
    (void)n;
}

void ls_schedule_delay(LsSession *s, uint32_t at, unsigned d)
{
    if (d < 1) d = 1;
    if (d > LS_MAX_DELAY) d = LS_MAX_DELAY;
    add_delay(s, at, d);
    s->sched_at = at; s->sched_d = (uint8_t)d;
}

unsigned ls_wanted_delay(const LsSession *s)
{
    if (!s->rtt_samples) return ls_delay(s, s->assigned);
    /* the peer's input for a frame leaves D frames before it is needed and
     * takes half a round trip; the spread and the two frame clocks' phase
     * need a margin on top */
    double need = s->srtt_ms / 2 + 2 * s->rttvar_ms + 3.0;
    unsigned d = (unsigned)ceil(need / FRAME_MS);
    if (d < 1) d = 1;
    if (d > LS_MAX_DELAY) d = LS_MAX_DELAY;
    return d;
}

int ls_adapt(LsSession *s, uint32_t tick)
{
    if (!s->host || tick < s->adapt_next) return 0;
    s->adapt_next = tick + 30;
    if (s->sched_at && s->peer_sched_seen != s->sched_at && tick < s->sched_at + 120) return 0;   /* one change at a time */
    unsigned now = ls_delay(s, tick), want = ls_wanted_delay(s);
    /* the change reaches the joiner before it samples that frame: it is at
     * most `now` frames past me, and the packet takes a round trip */
    uint32_t lead = 2 * LS_MAX_DELAY + (uint32_t)ceil(s->srtt_ms / FRAME_MS) + 8;
    if (want > now) { ls_schedule_delay(s, tick + lead, want); s->low_since = 0; return 1; }
    if (want < now) {
        if (!s->low_since) s->low_since = tick;
        if (tick - s->low_since >= 180) { ls_schedule_delay(s, tick + lead, now - 1); s->low_since = 0; return 1; }
    } else s->low_since = 0;
    return 0;
}
