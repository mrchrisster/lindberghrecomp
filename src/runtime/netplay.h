/*
 * netplay.h - the one input layer every reader of the cabinet goes through,
 * and what is built on it: input recording and replay, the simulation state
 * hash, deterministic mode and input lockstep over the network (netplay.c).
 *
 * With none of the switches below set, cab_* are host_cabinet_* exactly.
 *
 *   LINDBERGH_INPUT_REC=<file>     write both players' cabinet state each frame
 *   LINDBERGH_INPUT_PLAY=<file>    play it back instead of the real input
 *   LINDBERGH_STATE_HASH=<N>       log "[hash] frame F: <hex>" every N frames
 *   LINDBERGH_DETERMINISTIC=1      the fixes for identical runs (implied by
 *                                  REC, PLAY and NETPLAY)
 *   LINDBERGH_NETPLAY=host[:port] | join:<ip>[:port]
 *   LINDBERGH_NETSIM=delay_ms,jitter_ms,loss_pct[,reorder_pct]
 * The full list and the protocol: docs/NETPLAY.md (VT3 repository).
 */
#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdint.h>
#include "lindbergh_rt.h"

/* The cabinet as the game reads it. cab_switch_read is the JVS switch read
 * (command 0x20): each one starts a new input frame ("tick"); the coin and
 * analog reads after it (cab_input) and game hooks between reads
 * (cab_buttons) see that frame's state. */
void     cab_switch_read(CabinetInput *out);
void     cab_input(CabinetInput *out);
unsigned cab_buttons(void);

/* Deterministic mode (LINDBERGH_DETERMINISTIC=1, or REC / PLAY / NETPLAY). */
int      lg_deterministic(void);
uint32_t lg_input_tick(void);              /* input frames so far (switch reads) */
/* the guest's clock in deterministic mode: a fixed date plus the input frame
 * count at 60 Hz - the same in every run and on both sides of a session */
uint64_t lg_det_time_us(void);
/* libc rand()/srand() in deterministic mode: glibc's random() (TYPE_3), the
 * generator the arcade's Linux had, the same on every host */
int      lg_det_rand(void);
void     lg_det_srand(unsigned seed);

/* A setting that changes what the game does (a flow switch, a test jump, the
 * mode): recorded in an input file's header and compared at the netplay
 * handshake, so two copies that would compute differently refuse to start.
 * A game module registers a callback that reports its effective values. */
void     netplay_setting(const char *name, long value);
void     netplay_settings_source(void (*report)(void));

/* A game's own small hash of what its match is (ball, players, score, its
 * random generators' state): logged beside the memory hash as
 * "[shash] frame F: <hex> <what>" - it names what went apart - and the one a
 * netplay session compares when there is one (LINDBERGH_NETPLAY_HASH=mem
 * compares the memory hash instead). */
void     netplay_semantic_hash(uint64_t (*fn)(char *what, unsigned cap));

/* Deterministic mode: called at the start of each input frame, before the
 * hash and the input - a game waits here until its loading threads have
 * finished what they were given, so a load completes at the same frame on
 * every machine however fast its disk is (VT3: hle_sega.c). */
void     netplay_frame_barrier(void (*fn)(void));

/* the session is running (stalls, hashes, locked settings) */
int      netplay_active(void);
int      netplay_take_stalled(void);   /* 1 once after a frame that waited for the other player */
/* the game's executable, for the version check (guest.c hands it over) */
void     netplay_set_elf(const void *img, unsigned long len);
void     netplay_add_image_rw(uint32_t lo, uint32_t hi);   /* a writable PT_LOAD segment: hashed by default */

#endif
