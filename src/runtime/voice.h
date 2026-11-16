/*
 * voice.h - voice chat beside a netplay session (voice.c).
 *
 * The microphone is cut into 40 ms pieces, sent only while the player talks
 * (a level gate, as Quest 3 Strikers does), compressed with Opus and carried
 * by the session's own connection (UDP or the WebSocket relay) as LS_VOICE
 * packets. Voice never touches the lockstep: a lost or late piece is simply
 * not heard, and the game's own (deterministic) audio is a separate stream.
 *
 *   LINDBERGH_VOICE=0          no voice chat
 *   LINDBERGH_VOICE_MIC=0      start with the microphone muted
 *   LINDBERGH_VOICE_IN=<file>  the "microphone" is a file: raw 16-bit mono
 *                              48 kHz, looped (tests, and the Mac test player)
 *   LINDBERGH_VOICE_OUT=<file> also write what the other player said there
 *                              (raw 16-bit mono 48 kHz)
 *   LINDBERGH_VOICE_GATE=<dB>  the gate's level, dBFS (default -38)
 */
#ifndef LINDBERGH_VOICE_H
#define LINDBERGH_VOICE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int on;              /* a session with voice is running */
    int mic;             /* our microphone is not muted */
    int mic_ok;          /* a microphone (or the test file) is open */
    int talking;         /* we are sending right now */
    int they_talk;       /* the other player was heard in the last 300 ms */
    unsigned sent, heard, lost;
} VoiceStatus;

void voice_init(void);                         /* once, when a session is set up (netplay.c) */
void voice_receive(const uint8_t *p, size_t n); /* an LS_VOICE packet's body, from the network thread */
void voice_set_mic(int on);
int  voice_mic(void);
void voice_status(VoiceStatus *st);

/* netplay.c: send an LS_VOICE packet with this body to the other player
 * (dropped when no session is running) */
void netplay_send_voice(const uint8_t *body, size_t n);

#endif
