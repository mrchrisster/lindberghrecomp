/*
 * hle_segaapi.c - libsegaapi, the Lindbergh's sound card API, as a mixer.
 *
 * The House of the Dead 4 plays everything through SEGAAPI. It creates
 * buffers - 24 mono ones for effects, 4 small stereo rings for the streamed
 * music and voices - writes PCM into the memory each one maps, and starts,
 * stops, loops and repositions them, reading the play position back to know
 * where to refill a ring. So the position has to be the one the listener is
 * actually hearing, and the way to have that is to mix here: an SDL audio
 * callback walks every playing buffer straight out of guest memory, at the
 * buffer's own rate, and advances its position by exactly what it played.
 *
 * Levels: each source channel has a volume and up to seven sends, each send
 * routed to an output (0 front left, 1 front right, 2 centre, 4/5 rear L/R;
 * 0xFFFF0001 is "not routed"). A send's level times the channel's volume is
 * what reaches that output. Volumes and levels are 0..0xFFFFFFFF.
 *
 * The buffer config, as libsegaapi lays it out on i386:
 *      +0   dwPriority      +4  dwSampleRate   +8  dwSampleFormat
 *      +12  byNumChans      +16 dwReserved     +20 hUserData
 *      +24  mapData.dwSize  +28 mapData.dwOffset  +32 mapData.hBufferHdr
 * Sample formats: 0x20 signed 16-bit, 0x04 unsigned 8-bit.
 *
 * Muted (the player's setting, or LINDBERGH_MUTE) the mix still runs - the
 * game depends on positions moving - and only the output is silence.
 * LINDBERGH_SEGA_METER=1 logs the mix's peak level every two seconds, which
 * is how a muted run checks there is sound to hear.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lindbergh_rt.h"

#if !defined(_WIN32) || defined(RECOMP_GUEST_BASE)
#include <SDL.h>
#include "host_settings.h"
#define HAVE_MIXER 1
#if defined(__ANDROID__) && defined(LINDBERGH_VULKAN_OFFSCREEN)
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#endif
#endif

#define SEGA_OK 0
/* flags: 1 a synth (effect) voice, 2 the caller's own memory, 4 mapped memory */
#define HABUF_ALLOC_USER_MEM  2u
#define OUT_RATE 48000
#define NSEND 7
#define NOT_ROUTED 0xFFFF0001u

/* Each buffer is a guest block the game holds a pointer to. */
typedef struct SegaBuf {
    uint32_t user;
    uint32_t data, size;
    uint32_t rate, format, chans;
    int      playing, paused, looping, release;
    double   pos;                     /* in frames */
    uint32_t loop_start, loop_end, end;   /* bytes */
    float    vol[2];
    float    atten;                   /* SetSynthParam 0, as a gain */
    float    level[2][NSEND];
    uint32_t route[2][NSEND];
    struct SegaBuf *next;             /* every buffer, for the mixer */
    /* A streamed ring (the game refills it and says so with UpdateBuffer):
     * how far the game has written and the mixer has played, in bytes
     * from the last SetPlaybackPosition, to count starvation - the mixer
     * playing what the game has not refilled yet (last lap's audio). */
    int      stream;
    int64_t  wr_abs, rd_abs;
    int      starved_now;             /* inside a starved stretch */
} SegaBuf;

static SegaBuf *g_bufs;
static float    g_io[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };

#ifdef HAVE_MIXER
static SDL_mutex *g_lock;
#define LOCK()   SDL_LockMutex(g_lock)
#define UNLOCK() SDL_UnlockMutex(g_lock)
#else
#define LOCK()   ((void)0)
#define UNLOCK() ((void)0)
#endif

static void seg_log(const char *name, CPU *c);
static SegaBuf *buf_of(uint32_t h) { return h ? (SegaBuf *)GP(h) : NULL; }
static uint32_t frame_bytes(const SegaBuf *b) { return (b->format == 0x04 ? 1 : 2) * (b->chans ? b->chans : 1); }
static float level01(uint32_t v) { return (float)(v / 4294967295.0); }

#ifdef HAVE_MIXER
static float sample(const SegaBuf *b, uint32_t frame, int ch)
{
    uint32_t off = frame * frame_bytes(b) + (uint32_t)ch * (b->format == 0x04 ? 1 : 2);
    if (off + 2 > b->size) return 0;
    if (b->format == 0x04) return (((const uint8_t *)GP(b->data))[off] - 128) / 128.0f;
    int16_t s;
    memcpy(&s, (const uint8_t *)GP(b->data) + off, 2);
    return s / 32768.0f;
}

/* How much of one source channel reaches the left and right outputs. */
static void gains(const SegaBuf *b, int ch, float *l, float *r)
{
    *l = *r = 0;
    for (int s = 0; s < NSEND; s++) {
        float g = b->level[ch][s];
        if (g <= 0) continue;
        switch (b->route[ch][s]) {
        /* the usual stereo downmix: centre and rears at -3 dB, the
         * subwoofer's own feed left out (Let's Go Jungle sends a voice to
         * every speaker at once, which at full level tripled it) */
        case 0: *l += g; break;
        case 1: *r += g; break;
        case 4: *l += g * 0.707f; break;
        case 5: *r += g * 0.707f; break;
        case 2: *l += g * 0.707f; *r += g * 0.707f; break;
        case 3: break;
        default: break;
        }
    }
    *l *= b->vol[ch] * b->atten * g_io[0];
    *r *= b->vol[ch] * b->atten * g_io[1];
}

/* The game's whole mix, before the limiter: Let's Go Jungle plays a dozen
 * voices near full scale at once, which the cabinet's amplifier had the
 * headroom for and a soft limit at 1.0 does not (segaapi_master_gain). */
static float g_master = 1.0f;
void segaapi_master_gain(float g) { if (g > 0) g_master = g; }

/* ---- health counters and the WAV dump ----
 * Always on (cheap): stream starvation (the mixer reached a streamed ring's
 * unrefilled part), late device callbacks (the gap since the last one well
 * over the block's own length), the time spent waiting for the lock and
 * mixing, and samples the limiter had to squash. Logged every 10 s when
 * anything went wrong, every 2 s with LINDBERGH_SEGA_METER=1:
 *   [sega-api] health: starved N events / M ms, stream lead min X ms ...
 * LINDBERGH_AUDIO_DUMP=<path>: the final mix (after the limiter, before
 * the player's volume and mute) as a 48 kHz stereo 16-bit WAV. Its header
 * is rewritten every log period, so a killed run leaves a valid file. */
typedef struct {
    unsigned starved_events, starved_frames;   /* frames at OUT_RATE */
    double   lead_min_ms;                      /* least written-ahead seen */
    unsigned late, blocks;
    double   gap_max_ms, lockwait_max_ms, mix_max_ms;
    unsigned over_knee, over_one, samples;
} Health;
static Health g_h = { .lead_min_ms = 1e9 };
static unsigned g_total_starved;

static FILE    *g_dump, *g_sdump;          /* the mix; the streamed rings alone */
static uint32_t g_dump_bytes, g_sdump_bytes;
static void wav_header(FILE *f, uint32_t sz)
{
    uint8_t h[44];
    uint32_t rate = OUT_RATE, br = OUT_RATE * 4;
    memcpy(h, "RIFF", 4); uint32_t r = 36 + sz; memcpy(h + 4, &r, 4);
    memcpy(h + 8, "WAVEfmt ", 8); uint32_t fl = 16; memcpy(h + 16, &fl, 4);
    uint16_t fmt = 1, ch = 2, ba = 4, bits = 16;
    memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &br, 4); memcpy(h + 32, &ba, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &sz, 4);
    long at = ftell(f);
    fseek(f, 0, SEEK_SET); fwrite(h, 1, 44, f); fflush(f);
    fseek(f, at > 44 ? at : 44, SEEK_SET);
}
static void dump_header(void)
{
    if (g_dump) wav_header(g_dump, g_dump_bytes);
    if (g_sdump) wav_header(g_sdump, g_sdump_bytes);
}
static void dump_open(void)
{
    const char *p = getenv("LINDBERGH_AUDIO_DUMP");
    if (!p || !*p) return;
    g_dump = fopen(p, "wb");
    if (!g_dump) { fprintf(stderr, "[sega-api] audio dump: cannot write %s\n", p); return; }
    setvbuf(g_dump, NULL, _IOFBF, 1 << 20);
    fprintf(stderr, "[sega-api] audio dump: the final mix to %s (48 kHz stereo 16-bit)\n", p);
    const char *v = getenv("LINDBERGH_AUDIO_DUMP_STREAMS");
    if (v && *v == '1') {
        char sp[1024];
        snprintf(sp, sizeof sp, "%s.streams.wav", p);
        if ((g_sdump = fopen(sp, "wb"))) {
            setvbuf(g_sdump, NULL, _IOFBF, 1 << 20);
            fprintf(stderr, "[sega-api] audio dump: the streamed rings alone (music, voices) to %s\n", sp);
        }
    }
    dump_header();
}
static void dump_block(FILE *f, uint32_t *bytes, const float *out, int frames)
{
    int16_t s[2048];
    for (int i = 0; i < frames * 2; i += 1024) {
        int n = frames * 2 - i < 1024 ? frames * 2 - i : 1024;
        for (int k = 0; k < n; k++) {
            float v = out[i + k] * 32767.0f;
            s[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v));
        }
        fwrite(s, 2, (size_t)n, f);
        *bytes += (uint32_t)n * 2;
    }
}

/* LINDBERGH_AUDIO_DUMP_WRITES=1 (with AUDIO_DUMP): what the game writes into
 * each streamed 16-bit stereo ring, in the order written, as
 * <path>.writes.<buffer>.wav at the ring's rate - the decoder's output
 * before the mixer touches it (test only). */
static void dump_write(SegaBuf *b, uint32_t off, uint32_t len)
{
    static int on = -1;
    static struct { uint32_t h; FILE *f; uint32_t bytes; } w[8];
    if (on < 0) { const char *v = getenv("LINDBERGH_AUDIO_DUMP_WRITES"); on = v && *v == '1' && getenv("LINDBERGH_AUDIO_DUMP"); }
    if (!on || b->format != 0x20 || b->chans != 2 || off + len > b->size) return;
    int i;
    for (i = 0; i < 8 && w[i].f && w[i].h != H2G(b); i++) ;
    if (i == 8) return;
    if (!w[i].f) {
        char p[1024];
        snprintf(p, sizeof p, "%s.writes.%x.wav", getenv("LINDBERGH_AUDIO_DUMP"), H2G(b));
        if (!(w[i].f = fopen(p, "wb"))) return;
        w[i].h = H2G(b);
        fseek(w[i].f, 44, SEEK_SET);
    }
    fwrite((const uint8_t *)GP(b->data) + off, 1, len, w[i].f);
    w[i].bytes += len;
    wav_header(w[i].f, w[i].bytes);
}

/* LINDBERGH_SEGA_POS_WRAP=0: the old position at a loop's end (the loop
 * end itself until the next read - see the mixer). */
static int g_wrap = 1;

/* LINDBERGH_AUDIO_INTERP=0: the old nearest-sample read (no interpolation
 * between source samples when the voice's rate is not the output's). */
static int g_interp = -1;

static void mix(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    float *out = (float *)stream;
    int frames = len / (int)(2 * sizeof(float));
    memset(stream, 0, (size_t)len);
    static float sout[8192];
    int sfr = g_sdump && frames <= 4096;
    if (sfr) memset(sout, 0, (size_t)frames * 2 * sizeof(float));
    if (g_interp < 0) { const char *v = getenv("LINDBERGH_AUDIO_INTERP"); g_interp = !(v && *v == '0'); }

    /* callback timing: a gap well over the block's length means the device
     * ran dry (or will) unless it buffers more than one block */
    static Uint64 last;
    Uint64 t0 = SDL_GetPerformanceCounter();
    double freq = (double)SDL_GetPerformanceFrequency();
    double block_ms = frames * 1000.0 / OUT_RATE;
    if (last) {
        double gap = (t0 - last) * 1000.0 / freq;
        if (gap > g_h.gap_max_ms) g_h.gap_max_ms = gap;
        if (gap > block_ms * 1.5) g_h.late++;
    }
    last = t0;
    g_h.blocks++;

    LOCK();
    Uint64 t1 = SDL_GetPerformanceCounter();
    double lw = (t1 - t0) * 1000.0 / freq;
    if (lw > g_h.lockwait_max_ms) g_h.lockwait_max_ms = lw;
    for (SegaBuf *b = g_bufs; b; b = b->next) {
        if (!b->playing || b->paused || !b->data || !b->size) continue;
        uint32_t fb = frame_bytes(b), total = b->size / fb;
        uint32_t end = (b->end && b->end <= b->size ? b->end : b->size) / fb;
        uint32_t lend = (b->loop_end && b->loop_end <= b->size ? b->loop_end : b->size) / fb;
        uint32_t lstart = b->loop_start / fb;
        double step = (b->rate ? b->rate : OUT_RATE) / (double)OUT_RATE;
        int chans = b->chans >= 2 ? 2 : 1;
        float gl[2], gr[2];
        for (int ch = 0; ch < chans; ch++) gains(b, ch, &gl[ch], &gr[ch]);
        int loops = b->looping && !b->release && lend > lstart;
        int lerp = g_interp && step != 1.0;
        double adv = 0;
        float *o = out;
        if (sfr && b->stream && loops) o = sout;   /* mixed into out afterwards */
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)b->pos;
            if (b->looping && !b->release ? f >= lend : f >= end) {
                if (loops) { b->pos = lstart + (b->pos - lend); f = (uint32_t)b->pos; }
                else { b->playing = 0; b->pos = end; break; }
            }
            if (f >= total) { b->playing = 0; break; }
            if (lerp) {
                /* linear between this source frame and the next (the loop's
                 * start after its end), so a 44.1 / 32 kHz voice is not read
                 * as a staircase */
                uint32_t n = f + 1;
                if (loops ? n >= lend : n >= end) n = loops ? lstart : f;
                if (n >= total) n = f;
                float t = (float)(b->pos - f);
                for (int ch = 0; ch < chans; ch++) {
                    float a = sample(b, f, ch), s = a + (sample(b, n, ch) - a) * t;
                    o[2 * i]     += s * gl[ch];
                    o[2 * i + 1] += s * gr[ch];
                }
            } else {
                for (int ch = 0; ch < chans; ch++) {
                    float s = sample(b, f, ch);
                    o[2 * i]     += s * gl[ch];
                    o[2 * i + 1] += s * gr[ch];
                }
            }
            b->pos += step;
            adv += step;
        }
        /* A looping voice that ends the block exactly on its loop end is
         * wrapped now, not at its next read: the game reads the position
         * between blocks, and a ring's position must be inside the ring.
         * (The rings are 16384 frames = 16 blocks at 48 kHz, so this was
         * every lap: the reading "size" told the game the ring was empty,
         * it rewrote all of it and the music jumped 341 ms each lap.) */
        if (loops && g_wrap && b->pos >= lend) b->pos = lstart + (b->pos - lend);
        if (b->stream && loops) {
            /* starvation: played past what the game has refilled */
            b->rd_abs += (int64_t)(adv * fb);
            int64_t lead = b->wr_abs - b->rd_abs;
            double lead_ms = (double)lead / fb * 1000.0 / (b->rate ? b->rate : OUT_RATE);
            if (lead_ms < g_h.lead_min_ms) g_h.lead_min_ms = lead_ms;
            if (lead < 0) {
                double over = (double)-lead / fb / step;          /* output frames */
                g_h.starved_frames += (unsigned)(over < frames ? over : frames);
                if (!b->starved_now) { g_h.starved_events++; g_total_starved++; }
                b->starved_now = 1;
            } else b->starved_now = 0;
        }
    }
    UNLOCK();

    if (sfr) {
        for (int i = 0; i < frames * 2; i++) { out[i] += sout[i]; sout[i] *= g_master; }
        dump_block(g_sdump, &g_sdump_bytes, sout, frames);
    }
    for (int i = 0; i < frames * 2; i++) out[i] *= g_master;
    static float peak; static unsigned blocks;
    static int meter = -1;
    if (meter < 0) { const char *v = getenv("LINDBERGH_SEGA_METER"); meter = v && *v == '1'; }
    HostSettings *st = settings();
    float vol = st->muted ? 0.0f : st->volume / 100.0f;
    for (int i = 0; i < frames * 2; i++) {
        float a = fabsf(out[i]);
        if (a > peak) peak = a;
        if (a > 0.8f) { g_h.over_knee++; if (a > 1.0f) g_h.over_one++; }
        /* soft knee: untouched below 0.8, then eased towards 1.0, so a
         * firefight of a dozen voices compresses instead of clipping */
        float v = out[i], m = fabsf(v);
        if (m > 0.8f) v = copysignf(0.8f + 0.2f * tanhf((m - 0.8f) / 0.2f), v);
        out[i] = v;
    }
    g_h.samples += (unsigned)frames * 2;
    if (g_dump) dump_block(g_dump, &g_dump_bytes, out, frames);
    for (int i = 0; i < frames * 2; i++) out[i] *= vol;

    double mix_ms = (SDL_GetPerformanceCounter() - t1) * 1000.0 / freq;
    if (mix_ms > g_h.mix_max_ms) g_h.mix_max_ms = mix_ms;

    ++blocks;
    unsigned period = meter ? 2 : 10;
    if (blocks * (unsigned)frames >= period * OUT_RATE) {
        int playing = 0, made = 0, streams = 0;
        LOCK();
        for (SegaBuf *b = g_bufs; b; b = b->next) {
            made++;
            if (b->playing && !b->paused) { playing++; if (b->stream && b->looping) streams++; }
        }
        UNLOCK();
        if (meter)
            fprintf(stderr, "[sega-api] mix peak %.3f%s, %d of %d voices playing\n", peak, st->muted ? " (muted)" : "", playing, made);
        int bad = g_h.starved_events || g_h.late;
        if (meter || bad)
            fprintf(stderr, "[sega-api] health: starved %u events / %.0f ms (%u total), "
                    "stream lead min %.0f ms (%d streams), device %u late of %u blocks "
                    "(gap max %.1f ms, block %.1f), lock wait max %.2f ms, mix max %.2f ms, "
                    "limiter %.2f%% over 0.8 / %.2f%% over 1.0\n",
                    g_h.starved_events, g_h.starved_frames * 1000.0 / OUT_RATE, g_total_starved,
                    g_h.lead_min_ms < 1e8 ? g_h.lead_min_ms : 0.0, streams, g_h.late, g_h.blocks,
                    g_h.gap_max_ms, block_ms, g_h.lockwait_max_ms, g_h.mix_max_ms,
                    g_h.samples ? 100.0 * g_h.over_knee / g_h.samples : 0.0,
                    g_h.samples ? 100.0 * g_h.over_one / g_h.samples : 0.0);
        memset(&g_h, 0, sizeof g_h); g_h.lead_min_ms = 1e9;
        peak = 0; blocks = 0;
        dump_header();
    }
}

#if defined(__ANDROID__) && defined(LINDBERGH_VULKAN_OFFSCREEN)
/* Even SDL's dummy audio driver calls Android_JNI_AudioSetThreadPriority
 * from SDL_RunAudio. A CLI has no JavaVM. Keep the real mix/voice clock on
 * an owned pthread with discarded output; this is diagnostic audio only. */
static pthread_t diagnostic_audio;
static atomic_int diagnostic_audio_running;
static void *diagnostic_mix(void *unused)
{
    (void)unused;float discarded[1024*2];struct timespec next;
    clock_gettime(CLOCK_MONOTONIC,&next);
    while(atomic_load(&diagnostic_audio_running)) {
        mix(NULL,(Uint8 *)discarded,sizeof discarded);
        next.tv_nsec+=1000000000LL*1024/OUT_RATE;
        if(next.tv_nsec>=1000000000){next.tv_nsec-=1000000000;++next.tv_sec;}
        clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&next,NULL);
    }
    return NULL;
}
static void close_diagnostic_audio(void)
{
    atomic_store(&diagnostic_audio_running,0);pthread_join(diagnostic_audio,NULL);
}
#endif
static void open_output(void)
{
    static int done;
    if (done) return;
    done = 1;
    g_lock = SDL_CreateMutex();
    { const char *v = getenv("LINDBERGH_SEGA_POS_WRAP"); if (v && *v == '0') { g_wrap = 0; fprintf(stderr, "[sega-api] old loop-end position (LINDBERGH_SEGA_POS_WRAP=0)\n"); } }
#if defined(__ANDROID__) && defined(LINDBERGH_VULKAN_OFFSCREEN)
    atomic_store(&diagnostic_audio_running,1);
    if(pthread_create(&diagnostic_audio,NULL,diagnostic_mix,NULL)) {
        atomic_store(&diagnostic_audio_running,0);fprintf(stderr,"[sega-api] diagnostic mixer thread failed\n");return;
    }
    if(atexit(close_diagnostic_audio)){close_diagnostic_audio();return;}
    fprintf(stderr,"[sega-api] diagnostic mix at %d Hz; discarded output, no JavaVM\n",OUT_RATE);
    return;
#endif
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[sega-api] no audio: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want = { 0 }, got;
    want.freq = OUT_RATE; want.format = AUDIO_F32SYS; want.channels = 2;
    want.samples = 1024; want.callback = mix;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!dev) { fprintf(stderr, "[sega-api] no audio device: %s\n", SDL_GetError()); return; }
    dump_open();
    SDL_PauseAudioDevice(dev, 0);
    fprintf(stderr, "[sega-api] mixing at %d Hz, %d-frame blocks (%s)\n", got.freq, got.samples,
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
}
#else
static void open_output(void) { }
#endif

/* ---- the API ---- */

/* SEGAAPI_CreateBuffer(config, callback, flags, *handle) */
static void h_create(CPU *c)
{
    open_output();
    uint32_t cfg = A32(0), flags = A32(2), out = A32(3);
    SegaBuf *b = (SegaBuf *)guest_alloc(sizeof *b, 16);
    memset(b, 0, sizeof *b);
    b->rate = rd32(cfg + 4); b->format = rd32(cfg + 8); b->chans = rd32(cfg + 12);
    b->user = rd32(cfg + 20); b->size = rd32(cfg + 24);
    if (flags & HABUF_ALLOC_USER_MEM) {
        b->data = rd32(cfg + 32);
    } else {
        void *mem = guest_alloc(b->size ? b->size : 16, 16);
        memset(mem, 0, b->size ? b->size : 16);
        b->data = H2G(mem);
        wr32(cfg + 32, b->data);
    }
    b->end = b->loop_end = b->size;
    b->vol[0] = b->vol[1] = 1;
    b->atten = 1;
    for (int ch = 0; ch < 2; ch++)
        for (int s = 0; s < NSEND; s++) b->route[ch][s] = NOT_ROUTED;
    /* until told otherwise: channel 0 left (and right, if mono), 1 right */
    b->route[0][0] = 0; b->level[0][0] = 1;
    if (b->chans >= 2) { b->route[1][1] = 1; b->level[1][1] = 1; }
    else               { b->route[0][1] = 1; b->level[0][1] = 1; }
    LOCK(); b->next = g_bufs; g_bufs = b; UNLOCK();
    if (out) wr32(out, H2G(b));
    {
        const char *v = getenv("LINDBERGH_SEGA_LOG");
        if (v && *v == '1')
            fprintf(stderr, "[sega-api]   -> buffer %#x: %u Hz, format %#x, %u ch, %u bytes (%.0f ms), flags %#x\n",
                    H2G(b), b->rate, b->format, b->chans, b->size,
                    b->rate ? b->size / (double)frame_bytes(b) * 1000.0 / b->rate : 0.0, flags);
    }
    RET(SEGA_OK);
}

static void h_destroy(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    LOCK();
    for (SegaBuf **p = &g_bufs; b && *p; p = &(*p)->next)
        if (*p == b) { *p = b->next; break; }
    UNLOCK();
    RET(SEGA_OK);
}
static void h_play(CPU *c)  { SegaBuf *b = buf_of(A32(0)); LOCK(); if (b) { b->playing = 1; b->paused = 0; b->release = 0; } UNLOCK(); RET(SEGA_OK); }
static void h_stop(CPU *c)  { SegaBuf *b = buf_of(A32(0)); LOCK(); if (b) { b->playing = 0; b->paused = 0; } UNLOCK(); RET(SEGA_OK); }
static void h_pause(CPU *c) { SegaBuf *b = buf_of(A32(0)); LOCK(); if (b) b->paused = 1; UNLOCK(); RET(SEGA_OK); }
/* status: 0 stop, 1 active, 2 pause */
static void h_status(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    RET(!b || !b->playing ? 0 : b->paused ? 2 : 1);
}
static void h_get_pos(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (!b) { RET(0); return; }
    uint32_t fb = frame_bytes(b), f = (uint32_t)b->pos;
#ifdef HAVE_MIXER
    /* never report the loop end itself: that is the loop's start */
    uint32_t lend = (b->loop_end && b->loop_end <= b->size ? b->loop_end : b->size) / fb;
    uint32_t lstart = b->loop_start / fb;
    if (g_wrap && b->looping && !b->release && lend > lstart && f >= lend) f = lstart + (f - lend);
#endif
    RET(f * fb);
}
static void h_set_pos(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    LOCK();
    if (b) {
        b->pos = A32(1) / (double)frame_bytes(b);
        b->rd_abs = b->wr_abs = (int64_t)((uint32_t)b->pos * frame_bytes(b));
        b->starved_now = 0;
    }
    UNLOCK();
    RET(SEGA_OK);
}
/* UpdateBuffer(h, offset, length): the game has written that part of the
 * buffer. The data is read where the game wrote it, so the mix needs
 * nothing; it marks a streamed ring and moves its written-ahead mark. The
 * write is placed in the ring's coming lap: at or after the play position
 * this lap, behind it the next (what a ring means). */
static void h_update(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    uint32_t off = A32(1), len = A32(2);
    if (b && b->size && len) {
#ifdef HAVE_MIXER
        dump_write(b, off, len);
#endif
        LOCK();
        b->stream = 1;
        int64_t sz = b->size, rp = b->rd_abs % sz;
        int64_t at = b->rd_abs + (((int64_t)off - rp) % sz + sz) % sz;
        int64_t to = at + len;
        if (to > b->rd_abs + sz) to = b->rd_abs + sz;
        if (to > b->wr_abs) b->wr_abs = to;
        UNLOCK();
        seg_log("UpdateBuffer", c);
        {
            static int on = -1; static unsigned n;
            if (on < 0) { const char *v = getenv("LINDBERGH_SEGA_LOG"); on = v && *v == '1'; }
            if (on && n++ < 4000)
                fprintf(stderr, "[sega-api]   update %#x at %u +%u, mixer at %u, lead %lld\n", H2G(b), off, len,
                        (uint32_t)b->pos * frame_bytes(b), (long long)(b->wr_abs - b->rd_abs));
        }
    }
    RET(SEGA_OK);
}
static void h_get_user(CPU *c) { SegaBuf *b = buf_of(A32(0)); RET(b ? b->user : 0); }
static void h_set_user(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->user = A32(1); RET(SEGA_OK); }
static void h_set_loop(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->looping = A32(1) != 0; RET(SEGA_OK); }
static void h_set_release(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->release = A32(1) != 0; RET(SEGA_OK); }
static void h_set_start(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->loop_start = A32(1); RET(SEGA_OK); }
static void h_set_endloop(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->loop_end = A32(1); RET(SEGA_OK); }
static void h_set_end(CPU *c)  { SegaBuf *b = buf_of(A32(0)); if (b) b->end = A32(1); RET(SEGA_OK); }
static void h_set_rate(CPU *c) { SegaBuf *b = buf_of(A32(0)); if (b) b->rate = A32(1); RET(SEGA_OK); }
/* SetChannelVolume(h, channel, volume) */
static void h_vol(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b && A32(1) < 2) b->vol[A32(1)] = level01(A32(2));
    RET(SEGA_OK);
}
/* SetSendRouting(h, channel, send, destination) */
static void h_route(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b && A32(1) < 2 && A32(2) < NSEND) b->route[A32(1)][A32(2)] = A32(3);
    RET(SEGA_OK);
}
/* SetSendLevel(h, channel, send, level) */
static void h_send(CPU *c)
{
    SegaBuf *b = buf_of(A32(0));
    if (b && A32(1) < 2 && A32(2) < NSEND) b->level[A32(1)][A32(2)] = level01(A32(3));
    RET(SEGA_OK);
}
/* SetIOVolume(port, volume): the card's outputs */
static void h_iovol(CPU *c) { if (A32(0) < 8) g_io[A32(0)] = level01(A32(1)); RET(SEGA_OK); }
/* The data is read where the game wrote it, so an update needs nothing. */
static void h_ok(CPU *c) { RET(SEGA_OK); }

/* libkswapapi: the cabinet's key-swap helper. Nothing to collect. */
static void h_kswap_collect(CPU *c) { RET(0); }

/* LINDBERGH_SEGA_LOG=1: every call but the constant refills, with arguments. */
static void seg_log(const char *name, CPU *c)
{
    static int on = -1;
    static unsigned n;
    if (on < 0) { const char *v = getenv("LINDBERGH_SEGA_LOG"); on = v && *v == '1'; }
    if (!on || n++ > 4000) return;
    fprintf(stderr, "[sega-api] %s(%#x, %#x, %#x, %#x)\n", name, A32(0), A32(1), A32(2), A32(3));
}
#define LOGGED(name, fn) static void L_##fn(CPU *c) { seg_log(name, c); fn(c); }
LOGGED("CreateBuffer", h_create)   LOGGED("DestroyBuffer", h_destroy)
LOGGED("Play", h_play)             LOGGED("Stop", h_stop)            LOGGED("Pause", h_pause)
LOGGED("SetPlaybackPosition", h_set_pos)  LOGGED("SetLoopState", h_set_loop)
LOGGED("SetReleaseState", h_set_release)  LOGGED("SetStartLoopOffset", h_set_start)
LOGGED("SetEndLoopOffset", h_set_endloop) LOGGED("SetEndOffset", h_set_end)
LOGGED("SetSampleRate", h_set_rate)       LOGGED("SetChannelVolume", h_vol)
LOGGED("SetSendRouting", h_route)         LOGGED("SetSendLevel", h_send)
LOGGED("SetIOVolume", h_iovol)
/* SetSynthParam(h, param, value). Param 0 is the voice's attenuation in
 * centibels (HOTD4 sets 21..98); the rest it sets are envelope and filter
 * times left at their "off" value (-12000), which a plain player matches. */
static void h_synth(CPU *c)
{
    seg_log("SetSynthParam", c);
    SegaBuf *b = buf_of(A32(0));
    if (b && A32(1) == 0) {
        int cb = AI32(2);
        b->atten = cb <= 0 ? 1.0f : (float)pow(10.0, -cb / 200.0);
    }
    RET(SEGA_OK);
}

/* SetSynthParamMultiple(h, count, pairs): the same, for an array of
 * { int param; int value; } (Let's Go Jungle sets several at once) */
static void h_synth_multi(CPU *c)
{
    seg_log("SetSynthParamMultiple", c);
    SegaBuf *b = buf_of(A32(0));
    uint32_t n = A32(1), p = A32(2);
    {
        static int on = -1, shown;
        if (on < 0) { const char *v = getenv("LINDBERGH_SEGA_LOG"); on = v && *v == '1'; }
        for (uint32_t i = 0; on && p && i < n && i < 16 && shown < 400; i++, shown++)
            fprintf(stderr, "[sega-api]   param %d = %d\n", (int)rd32(p + 8 * i), (int)rd32(p + 8 * i + 4));
    }
    for (uint32_t i = 0; b && p && i < n && i < 64; i++) {
        if (rd32(p + 8 * i) != 0) continue;
        int cb = (int)rd32(p + 8 * i + 4);
        b->atten = cb <= 0 ? 1.0f : (float)pow(10.0, -cb / 200.0);
    }
    RET(SEGA_OK);
}

void hle_register_segaapi(void)
{
    hle_bind("SEGAAPI_Init", h_ok);
    hle_bind("SEGAAPI_Exit", h_ok);
    hle_bind("SEGAAPI_Reset", h_ok);
    hle_bind("SEGAAPI_SetGlobalEAXProperty", h_ok);     /* the room reverb: not modelled */
    hle_bind("SEGAAPI_UpdateBuffer", h_update);
    hle_bind("SEGAAPI_CreateBuffer", L_h_create);
    hle_bind("SEGAAPI_DestroyBuffer", L_h_destroy);
    hle_bind("SEGAAPI_Play", L_h_play);
    hle_bind("SEGAAPI_Stop", L_h_stop);
    hle_bind("SEGAAPI_Pause", L_h_pause);
    hle_bind("SEGAAPI_GetPlaybackStatus", h_status);
    hle_bind("SEGAAPI_GetPlaybackPosition", h_get_pos);
    hle_bind("SEGAAPI_SetPlaybackPosition", L_h_set_pos);
    hle_bind("SEGAAPI_GetUserData", h_get_user);
    hle_bind("SEGAAPI_SetUserData", h_set_user);
    hle_bind("SEGAAPI_SetLoopState", L_h_set_loop);
    hle_bind("SEGAAPI_SetReleaseState", L_h_set_release);
    hle_bind("SEGAAPI_SetStartLoopOffset", L_h_set_start);
    hle_bind("SEGAAPI_SetEndLoopOffset", L_h_set_endloop);
    hle_bind("SEGAAPI_SetEndOffset", L_h_set_end);
    hle_bind("SEGAAPI_SetSampleRate", L_h_set_rate);
    hle_bind("SEGAAPI_SetChannelVolume", L_h_vol);
    hle_bind("SEGAAPI_SetSendRouting", L_h_route);
    hle_bind("SEGAAPI_SetSendLevel", L_h_send);
    hle_bind("SEGAAPI_SetIOVolume", L_h_iovol);
    hle_bind("SEGAAPI_SetSynthParam", h_synth);
    hle_bind("SEGAAPI_SetSynthParamMultiple", h_synth_multi);
    hle_bind("kswap_collect", h_kswap_collect);
}
