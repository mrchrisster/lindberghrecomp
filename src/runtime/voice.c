/*
 * voice.c - voice chat beside a netplay session (see voice.h).
 *
 * One thread of its own: while a session runs it opens a microphone and a
 * speaker stream, cuts the microphone into 40 ms pieces, sends the ones above
 * the level gate as Opus, and decodes what comes in into a small jitter
 * buffer the speaker stream plays from. The design follows Quest 3 Strikers'
 * (ai_docs/research/netplay-example-strikers.md in the VT3 repository):
 * a -38 dBFS gate held open 300 ms, about 60 ms gathered before playing and at
 * most 240 ms kept, older audio dropped.
 *
 * Quest: AAudio, the microphone with the VOICE_COMMUNICATION preset (Android's
 * echo canceller and noise suppressor, so the game's sound from the headset's
 * speakers is not sent back). Elsewhere: SDL's audio devices.
 */
#include "voice.h"
#include "lockstep.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(LINDBERGH_VOICE) && !defined(_WIN32)
#include <opus.h>
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#else
#include <SDL.h>
#endif

int netplay_active(void);

#define RATE        48000
#define PIECE       1920                 /* 40 ms */
#define PRIME       2880                 /* 60 ms gathered before playing */
#define KEEP_MAX    11520                /* 240 ms at most queued */
#define KEEP_AFTER  4800                 /* ... trimmed back to 100 ms */
#define RING        (RATE)               /* one second, a power of nothing: indexes wrap by % */
#define HANG_MS     300
#define MAX_PKT     400

static double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* a mono 16-bit ring, written by one side and read by the other under a lock */
typedef struct { int16_t s[RING]; size_t rd, wr; pthread_mutex_t lock; } Ring;
static size_t ring_fill(const Ring *r) { return r->wr - r->rd; }
static void ring_write(Ring *r, const int16_t *p, size_t n)
{
    pthread_mutex_lock(&r->lock);
    for (size_t i = 0; i < n; i++) r->s[(r->wr + i) % RING] = p[i];
    r->wr += n;
    if (ring_fill(r) > RING) r->rd = r->wr - RING;
    pthread_mutex_unlock(&r->lock);
}
static size_t ring_read(Ring *r, int16_t *p, size_t n)
{
    pthread_mutex_lock(&r->lock);
    size_t have = ring_fill(r); if (n > have) n = have;
    for (size_t i = 0; i < n; i++) p[i] = r->s[(r->rd + i) % RING];
    r->rd += n;
    pthread_mutex_unlock(&r->lock);
    return n;
}

static Ring g_cap = { .lock = PTHREAD_MUTEX_INITIALIZER };   /* microphone -> voice thread */
static Ring g_out = { .lock = PTHREAD_MUTEX_INITIALIZER };   /* voice thread -> speaker */
static int  g_playing;                                       /* the speaker side: primed and playing */

/* packets in from the network thread */
static struct { uint8_t b[MAX_PKT]; size_t n; } g_in[32];
static int g_in_rd, g_in_wr;
static pthread_mutex_t g_in_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile int g_mic = 1, g_on, g_mic_ok, g_talking;
static volatile double g_heard_at;
static volatile unsigned g_sent, g_heard, g_lost;
static int g_disabled;

/* the speaker: whatever is queued, once 60 ms have gathered */
static void out_fill(int16_t *p, size_t n)
{
    size_t got = 0;
    if (!g_playing && ring_fill(&g_out) >= PRIME) g_playing = 1;
    if (g_playing) got = ring_read(&g_out, p, n);
    if (got < n) { memset(p + got, 0, (n - got) * 2); if (g_playing && got < n) g_playing = 0; }
}

/* ---------------- the platform's streams ---------------- */
#ifdef __ANDROID__
static AAudioStream *g_ain, *g_aout;
static aaudio_data_callback_result_t in_cb(AAudioStream *s, void *u, void *data, int32_t n)
{
    (void)s; (void)u; ring_write(&g_cap, (const int16_t *)data, (size_t)n);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static aaudio_data_callback_result_t out_cb(AAudioStream *s, void *u, void *data, int32_t n)
{
    (void)s; (void)u; out_fill((int16_t *)data, (size_t)n);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static AAudioStream *aa_open(int input)
{
    AAudioStreamBuilder *b; AAudioStream *st = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return NULL;
    AAudioStreamBuilder_setDirection(b, input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, RATE);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    if (input) AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    else { AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME); AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SPEECH); }
    AAudioStreamBuilder_setDataCallback(b, input ? in_cb : out_cb, NULL);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &st);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { fprintf(stderr, "[voice] no %s: %s\n", input ? "microphone (is the permission given?)" : "speaker stream", AAudio_convertResultToText(r)); return NULL; }
    if (AAudioStream_getSampleRate(st) != RATE || AAudioStream_getChannelCount(st) != 1) {
        fprintf(stderr, "[voice] the %s runs at %d Hz x%d, not 48 kHz mono\n", input ? "microphone" : "speaker", AAudioStream_getSampleRate(st), AAudioStream_getChannelCount(st));
        AAudioStream_close(st); return NULL;
    }
    if (AAudioStream_requestStart(st) != AAUDIO_OK) { AAudioStream_close(st); return NULL; }
    return st;
}
static int  open_out(void) { return (g_aout = aa_open(0)) != NULL; }
static int  open_mic(void) { return (g_ain = aa_open(1)) != NULL; }
static void close_all(void)
{
    if (g_ain) { AAudioStream_requestStop(g_ain); AAudioStream_close(g_ain); g_ain = NULL; }
    if (g_aout) { AAudioStream_requestStop(g_aout); AAudioStream_close(g_aout); g_aout = NULL; }
}
#else
static SDL_AudioDeviceID g_din, g_dout;
static void SDLCALL sdl_in(void *u, Uint8 *p, int n) { (void)u; ring_write(&g_cap, (const int16_t *)p, (size_t)n / 2); }
static void SDLCALL sdl_out(void *u, Uint8 *p, int n) { (void)u; out_fill((int16_t *)p, (size_t)n / 2); }
static SDL_AudioDeviceID sdl_open(int input)
{
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec want = { 0 }, got;
    want.freq = RATE; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 960;
    want.callback = input ? sdl_in : sdl_out;
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(NULL, input, &want, &got, 0);
    if (!d) { fprintf(stderr, "[voice] no %s: %s\n", input ? "microphone" : "speaker", SDL_GetError()); return 0; }
    SDL_PauseAudioDevice(d, 0);
    return d;
}
static int  open_out(void) { return (g_dout = sdl_open(0)) != 0; }
static int  open_mic(void) { return (g_din = sdl_open(1)) != 0; }
static void close_all(void)
{
    if (g_din) { SDL_CloseAudioDevice(g_din); g_din = 0; }
    if (g_dout) { SDL_CloseAudioDevice(g_dout); g_dout = 0; }
}
#endif

/* ---------------- the voice thread ---------------- */
static FILE *g_file_in, *g_file_out;
static double g_file_t0; static uint64_t g_file_done;

/* the test file as a microphone: as many samples as real time has asked for */
static void file_feed(void)
{
    if (!g_file_in) return;
    uint64_t due = (uint64_t)((now_ms() - g_file_t0) * (RATE / 1000.0));
    int16_t b[PIECE];
    while (g_file_done + PIECE <= due) {
        size_t n = fread(b, 2, PIECE, g_file_in);
        if (n < PIECE) { rewind(g_file_in); n += fread(b + n, 2, PIECE - n, g_file_in); if (n < PIECE) memset(b + n, 0, (PIECE - n) * 2); }
        ring_write(&g_cap, b, PIECE);
        g_file_done += PIECE;
    }
}

static void play(const int16_t *p, size_t n)
{
    pthread_mutex_lock(&g_out.lock);
    size_t fill = ring_fill(&g_out);
    if (fill + n > KEEP_MAX) g_out.rd = g_out.wr - (KEEP_AFTER < fill ? KEEP_AFTER : fill);   /* too far behind: drop the oldest */
    pthread_mutex_unlock(&g_out.lock);
    ring_write(&g_out, p, n);
    if (g_file_out) fwrite(p, 2, n, g_file_out);
}

static void *voice_thread(void *arg)
{
    (void)arg;
    OpusEncoder *enc = NULL; OpusDecoder *dec = NULL;
    int open = 0; double mic_retry = 0, hang_until = 0, last_log = 0;
    uint16_t seq = 0, want_seq = 0; int have_seq = 0, gate_open = 0;
    double gate = 32768.0 * pow(10.0, (getenv("LINDBERGH_VOICE_GATE") ? atof(getenv("LINDBERGH_VOICE_GATE")) : -38.0) / 20.0);
    const char *in_path = getenv("LINDBERGH_VOICE_IN"), *out_path = getenv("LINDBERGH_VOICE_OUT");
    for (;;) {
        int active = netplay_active();
        if (active && !open) {
            int err;
            enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &err);
            dec = opus_decoder_create(RATE, 1, &err);
            if (!enc || !dec) { fprintf(stderr, "[voice] Opus failed: %d - no voice chat\n", err); return NULL; }
            opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
            opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(8));
            opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            open_out();
            if (in_path) {
                g_file_in = fopen(in_path, "rb"); g_file_t0 = now_ms(); g_file_done = 0;
                g_mic_ok = g_file_in != NULL;
                fprintf(stderr, "[voice] the microphone is %s%s\n", in_path, g_file_in ? "" : " (cannot open it)");
            } else g_mic_ok = open_mic();
            if (out_path) g_file_out = fopen(out_path, "wb");
            mic_retry = now_ms() + 5000;
            g_on = 1; open = 1;
            fprintf(stderr, "[voice] voice chat on (microphone %s, %s)\n", g_mic_ok ? "open" : "none", g_mic ? "live" : "muted");
        } else if (!active && open) {
            close_all();
            if (g_file_in) { fclose(g_file_in); g_file_in = NULL; }
            if (g_file_out) { fclose(g_file_out); g_file_out = NULL; }
            opus_encoder_destroy(enc); opus_decoder_destroy(dec); enc = NULL; dec = NULL;
            g_on = 0; open = 0; g_mic_ok = 0; g_talking = 0;
            fprintf(stderr, "[voice] voice chat off: %u pieces sent, %u heard, %u lost\n", g_sent, g_heard, g_lost);
        }
        if (!open) { usleep(50000); continue; }

        /* no microphone yet (permission given later?): try again now and then */
        if (!g_mic_ok && !in_path && now_ms() > mic_retry) { g_mic_ok = open_mic(); mic_retry = now_ms() + 5000; if (g_mic_ok) fprintf(stderr, "[voice] microphone open\n"); }

        /* out: 40 ms pieces above the gate */
        file_feed();
        int16_t pcm[PIECE];
        while (ring_fill(&g_cap) >= PIECE) {
            ring_read(&g_cap, pcm, PIECE);
            double sum = 0;
            for (int i = 0; i < PIECE; i++) sum += (double)pcm[i] * pcm[i];
            double rms = sqrt(sum / PIECE), t = now_ms();
            if (rms >= gate) hang_until = t + HANG_MS;
            int send = g_mic && t < hang_until;
            if (send) {
                uint8_t pkt[MAX_PKT];
                pkt[0] = (uint8_t)seq; pkt[1] = (uint8_t)(seq >> 8); pkt[2] = gate_open ? 0 : 1;   /* 1: a new burst of talk */
                int n = opus_encode(enc, pcm, PIECE, pkt + 3, MAX_PKT - 3);
                if (n > 0) { netplay_send_voice(pkt, (size_t)n + 3); g_sent++; }
            }
            seq++;                    /* counts silent pieces too: the far end sees a gap, not a loss */
            gate_open = send; g_talking = send;
        }

        /* in: decode what came, conceal a lost piece or two inside a burst */
        for (;;) {
            uint8_t b[MAX_PKT]; size_t n = 0;
            pthread_mutex_lock(&g_in_lock);
            if (g_in_rd != g_in_wr) { n = g_in[g_in_rd].n; memcpy(b, g_in[g_in_rd].b, n); g_in_rd = (g_in_rd + 1) % 32; }
            pthread_mutex_unlock(&g_in_lock);
            if (n < 4) break;
            uint16_t s = (uint16_t)(b[0] | b[1] << 8); int first = b[2] & 1;
            int16_t d[PIECE];
            if (have_seq) {
                uint16_t gap = (uint16_t)(s - want_seq);
                if (gap >= 0x8000) continue;                     /* late: already played past it */
                if (!first && gap > 0 && gap <= 3) {
                    for (unsigned k = 0; k < gap; k++) { int m = opus_decode(dec, NULL, 0, d, PIECE, 0); if (m > 0) play(d, (size_t)m); }
                    g_lost += gap;
                }
            }
            int m = opus_decode(dec, b + 3, (opus_int32)(n - 3), d, PIECE, 0);
            if (m > 0) play(d, (size_t)m);
            want_seq = (uint16_t)(s + 1); have_seq = 1;
            g_heard++; g_heard_at = now_ms();
        }
        if (now_ms() - last_log > 30000) {
            last_log = now_ms();
            if (g_sent || g_heard) fprintf(stderr, "[voice] %u pieces sent, %u heard, %u lost\n", g_sent, g_heard, g_lost);
        }
        usleep(5000);
    }
    return NULL;
}

void voice_init(void)
{
    static int done; if (done++) return;
    const char *v = getenv("LINDBERGH_VOICE");
    if (v && *v == '0') { g_disabled = 1; fprintf(stderr, "[voice] voice chat off (LINDBERGH_VOICE=0)\n"); return; }
    const char *m = getenv("LINDBERGH_VOICE_MIC");
    if (m && *m == '0') g_mic = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, voice_thread, NULL) == 0) pthread_detach(t);
}

void voice_receive(const uint8_t *p, size_t n)
{
    if (g_disabled || !g_on || n > MAX_PKT) return;
    pthread_mutex_lock(&g_in_lock);
    int next = (g_in_wr + 1) % 32;
    if (next != g_in_rd) { memcpy(g_in[g_in_wr].b, p, n); g_in[g_in_wr].n = n; g_in_wr = next; }
    pthread_mutex_unlock(&g_in_lock);
}

void voice_set_mic(int on) { g_mic = on ? 1 : 0; fprintf(stderr, "[voice] microphone %s\n", on ? "live" : "muted"); }
int  voice_mic(void) { return g_mic; }
void voice_status(VoiceStatus *st)
{
    memset(st, 0, sizeof *st);
    st->on = g_on; st->mic = g_mic; st->mic_ok = g_mic_ok; st->talking = g_talking && g_mic;
    st->they_talk = g_on && now_ms() - g_heard_at < HANG_MS;
    st->sent = g_sent; st->heard = g_heard; st->lost = g_lost;
}
#else
void voice_init(void) {}
void voice_receive(const uint8_t *p, size_t n) { (void)p; (void)n; }
void voice_set_mic(int on) { (void)on; }
int  voice_mic(void) { return 0; }
void voice_status(VoiceStatus *st) { memset(st, 0, sizeof *st); }
#endif
