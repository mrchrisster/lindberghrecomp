/*
 * lockstep.h - the network input layer's protocol, without sockets or a game.
 *
 * Two copies of a game run the same frames from the same cabinet input. Each
 * sends the other its own side's input for frame F + D (D, the input delay,
 * the same for both and changed only at agreed frames); a frame is simulated
 * when both sides' input for it is in. Packets carry the last few frames of
 * input again (redundancy instead of retransmit), an ack, the sender's clock
 * and an echo of the peer's (round trip), the host's delay schedule and the
 * latest state hashes (desync detection).
 *
 * Everything here is plain C on byte buffers and caller-supplied times, so it
 * is unit-tested without a network or a game (tests/test_netplay.py).
 * netplay.c owns the sockets, the threads and the hook into the cabinet.
 * The wire format is in docs/NETPLAY.md of the VT3 repository.
 */
#ifndef LOCKSTEP_H
#define LOCKSTEP_H

#include <stddef.h>
#include <stdint.h>

#define LS_MAGIC        0x504E474Cu     /* "LGNP" little-endian */
#define LS_PROTO        1u
#define LS_WINDOW       1024u           /* frames kept per side (a power of two) */
#define LS_REDUNDANT    16u             /* frames of input repeated per packet */
#define LS_MAX_DELAY    12u
#define LS_HASHES       4u              /* state hashes carried per packet */
#define LS_PACKET_MAX   512u

enum { LS_HELLO = 1, LS_WELCOME = 2, LS_REFUSE = 3, LS_INPUT = 4, LS_BYE = 5,
       LS_QUERY = 6,        /* "who hosts here, and with what?" (unicast or broadcast) - before the game starts */
       LS_SETTINGS = 7,     /* the host's answer: its name and the NAME=value lines a joiner adopts */
       LS_VOICE = 8 };      /* voice chat beside the session (voice.c): never part of the lockstep */
enum { LS_BYE_QUIT = 1, LS_BYE_DESYNC = 2, LS_BYE_TIMEOUT = 3 };

/* One side's cabinet input for one frame. Buttons are already in their final
 * bit positions (the joiner's lever is CAB_P2_*); coins are that side's own
 * running counts; the gun is in the board's 0..65535 units (-1 off screen). */
typedef struct {
    uint32_t buttons;
    uint16_t coins[2];
    int32_t  gx, gy;
    uint8_t  gun;
} LsInput;

/* What both copies must agree on before they start. */
typedef struct {
    uint32_t proto;
    uint8_t  elf_sha[32];
    char     runtime[40];               /* the runtime's git hash (and -dirty) */
    char     settings[256];             /* "name=value;..." of everything that changes the simulation */
    uint8_t  want_delay;
} LsHello;

typedef struct { uint32_t tick; uint64_t hash; } LsHash;

typedef struct {
    int      host;                      /* 1: this side is the host (P1), 0: the joiner (P2) */
    uint32_t nonce;
    /* inputs by frame */
    LsInput  mine[LS_WINDOW], theirs[LS_WINDOW];
    uint32_t mine_tick[LS_WINDOW], theirs_tick[LS_WINDOW];   /* which frame a slot holds (0: none) */
    uint32_t assigned;                  /* the last frame my input has been assigned to */
    uint32_t peer_ack;                  /* the peer has all my input up to this frame */
    uint32_t have;                      /* I have all the peer's input up to this frame */
    /* the delay: d from frame `at` on (the newest schedule entry wins) */
    uint32_t delay_at[8]; uint8_t delay_d[8]; unsigned ndelay;
    uint32_t sched_at; uint8_t sched_d; /* the host's latest change, repeated until the joiner has it */
    uint32_t peer_sched_seen;           /* host: the joiner echoed this change */
    /* round trip */
    uint32_t peer_send_us, peer_send_at_us;   /* the peer's last clock, and when (mine) I had it */
    double   srtt_ms, rttvar_ms, rtt_min_ms, rtt_max_ms;
    unsigned rtt_samples;
    /* state hashes */
    LsHash   my_hash[LS_HASHES * 4], peer_hash[LS_HASHES];
    unsigned n_my_hash, n_peer_hash;
    uint32_t desync_tick;               /* first frame whose hashes differ (0: none) */
    uint32_t hashes_compared;
    /* counters */
    uint32_t sent, received, bad, dup_inputs, late_inputs;
    uint32_t peer_tick;                 /* the frame the peer was on when it last sent */
    int      bye;                       /* the peer said goodbye: LS_BYE_* */
    /* adaptive delay (host) */
    uint32_t adapt_next, low_since;
    unsigned waits;                     /* frames this side waited since the last adapt (ls_note_wait) */
    unsigned extra;                     /* frames added on top of the round trip's need because of waits */
    unsigned min_delay;                 /* the delay never goes below this (online: 3, the relay's TCP bursts) */
    uint32_t extra_until;               /* ...kept at least until this frame */
} LsSession;

/* little helpers, exported for the tests */
void     ls_sha256(const void *p, size_t n, uint8_t out[32]);
uint64_t ls_hash64(uint64_t h, const void *p, size_t n);   /* start with LS_HASH_SEED */
#define  LS_HASH_SEED 0xcbf29ce484222325ull

void     ls_init(LsSession *s, int host, uint32_t nonce, uint32_t start_tick, unsigned delay);
unsigned ls_delay(const LsSession *s, uint32_t tick);
/* my input sampled at frame `tick` -> its frame tick + D: gaps a larger delay
 * opens are filled with the input before, frames a smaller one would reuse are
 * dropped. Returns the frame it went to, 0 if dropped. */
uint32_t ls_local(LsSession *s, uint32_t tick, const LsInput *in);
int      ls_mine(const LsSession *s, uint32_t tick, LsInput *out);
int      ls_theirs(const LsSession *s, uint32_t tick, LsInput *out);
void     ls_merge(const LsInput *p1, const LsInput *p2, LsInput *out);

/* packets: each returns the length written (0 on no room) */
size_t   ls_write_hello(uint8_t *buf, size_t cap, const LsHello *h);
size_t   ls_write_welcome(uint8_t *buf, size_t cap, uint32_t nonce, unsigned delay, uint32_t start_tick);
size_t   ls_write_refuse(uint8_t *buf, size_t cap, const char *why);
size_t   ls_write_query(uint8_t *buf, size_t cap);
size_t   ls_write_settings(uint8_t *buf, size_t cap, const char *name, const char *lines);
int      ls_read_settings(const uint8_t *buf, size_t len, char *name, size_t ncap, char *lines, size_t lcap);
size_t   ls_write_bye(uint8_t *buf, size_t cap, uint32_t nonce, int reason);
size_t   ls_write_input(LsSession *s, uint8_t *buf, size_t cap, uint32_t now_us, uint32_t my_tick);
int      ls_type(const uint8_t *buf, size_t len);         /* LS_* or 0 for not ours */
int      ls_read_hello(const uint8_t *buf, size_t len, LsHello *h);
int      ls_read_welcome(const uint8_t *buf, size_t len, uint32_t *nonce, unsigned *delay, uint32_t *start_tick);
int      ls_read_refuse(const uint8_t *buf, size_t len, char *why, size_t cap);
/* an INPUT or BYE for this session: 1 taken, 0 not ours / malformed */
int      ls_receive(LsSession *s, const uint8_t *buf, size_t len, uint32_t now_us);

/* the handshake: "" when the two may play, else what differs (for the message) */
void     ls_compare_hello(const LsHello *mine, const LsHello *theirs, char *why, size_t cap);

/* host: schedule a delay change from frame `at` (both apply it there) */
void     ls_schedule_delay(LsSession *s, uint32_t at, unsigned d);
/* host, once a frame: the delay the round trip asks for, changed when it should
 * be (up at once, down after 3 s of less); returns 1 when a change was made */
void     ls_note_wait(LsSession *s);   /* this frame waited for the peer (host: it raises the delay when they pile up) */
int      ls_adapt(LsSession *s, uint32_t tick);
unsigned ls_wanted_delay(const LsSession *s);

/* my state hash for a frame; compared with the peer's as both come in */
void     ls_note_hash(LsSession *s, uint32_t tick, uint64_t hash);

#endif
