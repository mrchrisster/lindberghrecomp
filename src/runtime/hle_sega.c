#include "display_cadence.h"
/*
 * hle_sega.c - the cabinet's own library, answered from here.
 *
 * A Lindbergh game does not talk to its hardware directly. It calls SEGA's
 * amLib: a C API, statically linked into this binary, that reaches a base
 * board over a serial link - the JVS I/O board with the coin slots, buttons
 * and guns on it, the EEPROM holding the bookkeeping, the security dongle.
 * None of that exists here, so every one of those calls fails and the game
 * stops on "Error 11 - JVS I/O board is not connected to main board" before
 * it ever reaches its attract mode.
 *
 * These are plain C symbols, so they override by name the same way the
 * XF86VidMode entry points do. What is answered here is only what the game
 * actually asks on the way to attract mode; the game narrates the rest
 * through _sDebug::putConsole (LINDBERGH_CONSOLE=1), which is how each of
 * these was found rather than guessed.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <zlib.h>

#include "lindbergh_rt.h"
#if !defined(_WIN32)
#include "host_settings.h"
#ifndef _WIN32
#include "stereo.h"
#include "world_mode.h"
#endif
#endif

/* The library's own success code. Every am* function returns 0 for success
 * and a negative value for a failure the game then reports. */
#define AM_OK 0

/* The JVS packet the game handed to the transport.
 *
 * Request layout, read off amJvspClearPacket and amJvsSendRequest: byte 0 is
 * the node the packet is addressed to, byte 1 the length of the frame, and
 * bytes 2.. the frame itself. The acknowledge comes back in the same object
 * at byte 0x101 (length) and 0x102.. (frame).
 *
 * LINDBERGH_JVS=1 prints what is asked, which is the only honest way to find
 * out what this board has to be able to answer. */
static void jvs_dump(const char *tag, uint32_t pkt, unsigned off, unsigned len)
{
    const unsigned char *p = (const unsigned char *)GP(pkt);
    char line[512];
    int at = snprintf(line, sizeof line, "[jvs] %s node %u len %u:", tag, p[0], len);
    for (unsigned i = 0; i < len && i < 40 && at < 470; i++)
        at += snprintf(line + at, sizeof line - at, " %02X", p[off + i]);
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

/* ---- the I/O board itself ----
 *
 * JVS is a small request/response protocol over a serial line. A frame is
 *
 *     E0 <dest> <count> <payload...> <sum>
 *
 * where count covers everything after itself including the checksum, and the
 * checksum is the bytes from <dest> to the last payload byte, added up and
 * truncated to eight bits. A reply is the same shape addressed to 00, with a
 * status byte ahead of the reports and one report per command asked.
 *
 * The packet object carries more than the frame. Bytes 1.. of a REQUEST hold
 * a zero-terminated list of where each command starts inside it, built as the
 * game appended them; the matching ACK has to come back with the same list
 * pointing at where each report starts. That list is why this does not need
 * to know how long any command is - the game already said.
 *
 * Nothing here is pressed, inserted or aimed. An attract mode wants a board
 * that answers, not one that plays.
 */

#define JVS_SYNC      0xE0u
#define JVS_STATUS_OK 0x01u
#define JVS_REPORT_OK 0x01u

#define PKT_INDEX     1u        /* command / report offsets, zero-terminated */
#define PKT_FRAME_LEN 0x101u
#define PKT_FRAME     0x102u

/* What the board calls itself. The game reads this back as a C string and
 * shows it in the test menu; the shape is the one every SEGA I/O board of
 * this era reports. */
static const char JVS_IO_IDENT[] =
    "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";

/* Switches, coins, analog channels, and general purpose output - enough for a
 * two player gun cabinet. Each block is kind, then three parameters. */
static const unsigned char JVS_FEATURES[] = {
    0x01, 2, 12, 0,        /* players, buttons each */
    0x02, 2,  0, 0,        /* coin slots */
    0x03, 8, 16, 0,        /* analog channels, bits each */
    0x12, 6,  0, 0,        /* general purpose output */
    0x00                   /* end of list */
};

/* Where the cabinet's reload input lands in a player's switch bytes: byte 0,
 * bit 0 by default; The House of the Dead 4 Special's guns report a shake as
 * the second byte's bit 6. */
static unsigned g_reload_byte, g_reload_mask = 0x01;
void jvs_reload_switch(unsigned byte, unsigned mask) { g_reload_byte = byte; g_reload_mask = mask; }

static unsigned char g_action_switch[2][3] = {{0, 1, 0x80}, {1, 1, 0x80}};
void jvs_action_switch(unsigned player, unsigned target, unsigned byte, unsigned mask)
{
    if (player >= 2 || target >= 2 || byte >= 2) return;
    g_action_switch[player][0] = (unsigned char)target;
    g_action_switch[player][1] = (unsigned char)byte;
    g_action_switch[player][2] = (unsigned char)mask;
}

unsigned char jvs_switch_byte(unsigned buttons, unsigned player, unsigned byte)
{
    if (player >= 2 || byte >= 2) return 0;
    unsigned g = player ? buttons >> CAB_P2_SHIFT : buttons;
    unsigned j = player ? buttons >> CAB_P2_JOY_SHIFT : buttons;   /* the lever and buttons 2, 3 */
    unsigned char b = 0;
    if (!byte) {
        if (buttons & (player ? CAB_P2_START : CAB_P1_START)) b |= 0x80;
        if (!player && (buttons & CAB_SERVICE)) b |= 0x40;
        if (g & CAB_P1_TRIGGER) b |= 0x02;
        if (j & CAB_P1_UP)      b |= 0x20;
        if (j & CAB_P1_DOWN)    b |= 0x10;
        if (j & CAB_P1_LEFT)    b |= 0x08;
        if (j & CAB_P1_RIGHT)   b |= 0x04;
        if (j & CAB_P1_BUTTON2) b |= 0x01;
    } else {
        if (g & CAB_P1_FIREMODE) b |= 0x40;
        if (j & CAB_P1_BUTTON3)  b |= 0x80;
    }
    if ((g & CAB_P1_RELOAD) && byte == g_reload_byte) b |= (unsigned char)g_reload_mask;
    for (unsigned i = 0; i < 2; i++) {
        const unsigned char *s = g_action_switch[i];
        if (s[0] == player && s[1] == byte && (buttons & (i ? CAB_P2_ACTION : CAB_P1_ACTION))) b |= s[2];
    }
    return b;
}

/* One reply at a time: the game sends, then receives, on the same thread. */
static unsigned char g_ack[0x200];
static unsigned      g_ack_len;
static unsigned char g_report_at[0x40];
static unsigned      g_reports;

static void ack_begin(void)
{
    g_ack_len = 0; g_reports = 0;
    g_ack[g_ack_len++] = JVS_SYNC;
    g_ack[g_ack_len++] = 0x00;              /* addressed to the main board */
    g_ack[g_ack_len++] = 0x00;              /* count, filled in at the end */
    g_ack[g_ack_len++] = JVS_STATUS_OK;
}

/* Remember where this report starts before writing it: that offset is what
 * the game looks up to find the answer to the command it asked. */
static void ack_report(void)
{
    if (g_reports < sizeof g_report_at) g_report_at[g_reports++] = (unsigned char)g_ack_len;
    g_ack[g_ack_len++] = JVS_REPORT_OK;
}

static void ack_byte(unsigned char b)
{
    if (g_ack_len < sizeof g_ack - 1) g_ack[g_ack_len++] = b;
}

static void ack_zeros(unsigned n) { while (n--) ack_byte(0); }

static void ack_end(void)
{
    unsigned sum = 0;
    g_ack[2] = (unsigned char)(g_ack_len - 3 + 1);   /* payload + checksum */
    for (unsigned i = 1; i < g_ack_len; i++) sum += g_ack[i];
    g_ack[g_ack_len++] = (unsigned char)sum;
}

/* Answer one command. `a` is the command byte, `p` its parameters. */
static void jvs_command(const unsigned char *p)
{
    switch (p[0]) {
    case 0x10:                                   /* read board identity */
        ack_report();
        for (const char *s = JVS_IO_IDENT; *s; s++) ack_byte((unsigned char)*s);
        ack_byte(0);
        break;
    case 0x11: ack_report(); ack_byte(0x13); break;   /* command revision */
    case 0x12: ack_report(); ack_byte(0x30); break;   /* JVS revision */
    case 0x13: ack_report(); ack_byte(0x10); break;   /* communication version */
    case 0x14:                                   /* what this board has */
        ack_report();
        for (unsigned i = 0; i < sizeof JVS_FEATURES; i++) ack_byte(JVS_FEATURES[i]);
        break;
    case 0x20: {                                 /* switches: test, then players */
        /* One test byte, then `bytes` bytes for each of `players`. In the
         * first of those, bit 7 is start and bit 1 is the first push button,
         * which for a gun cabinet is the trigger. */
        CabinetInput in;
        host_cabinet_input(&in);
        unsigned players = p[1], bytes = p[2];
        {   /* Confirm the game is actually asking, and with what shape. */
            static int shown;
            if (shown < 3 && getenv("LINDBERGH_JVS")) {
                shown++;
                fprintf(stderr, "[jvs] switch read: %u players x %u bytes\n",
                        players, bytes);
            }
        }
        ack_report();
        ack_byte((in.buttons & CAB_TEST) ? 0x80 : 0x00);
        for (unsigned pl = 0; pl < players; pl++) {
            ack_byte(jvs_switch_byte(in.buttons, pl, 0));
            if (bytes > 1) {
                ack_byte(jvs_switch_byte(in.buttons, pl, 1));
                ack_zeros(bytes - 2);
            }
        }
        host_cabinet_switches_read();
        break;
    }
    case 0x21: {                                 /* coins */
        /* Two bytes a slot: the top two bits are the mechanism's condition,
         * zero meaning normal, and the remaining fourteen are the count. */
        CabinetInput in;
        host_cabinet_input(&in);
        ack_report();
        for (unsigned slot = 0; slot < p[1]; slot++) {
            unsigned n = (slot < 2 ? in.coins[slot] : 0) & 0x3FFF;
            ack_byte((unsigned char)(n >> 8));
            ack_byte((unsigned char)(n & 0xFF));
        }
        break;
    }
    case 0x22: {                                 /* analog: where the gun points */
        CabinetInput in;
        host_cabinet_input(&in);
        int w = in.screen_w > 0 ? in.screen_w : 1;
        int h = in.screen_h > 0 ? in.screen_h : 1;
        unsigned x = (unsigned)((long)in.gun_x * 65535 / w);
        unsigned y = (unsigned)((long)in.gun_y * 65535 / h);
        if (in.gun_x < 0) x = 0; if (x > 65535) x = 65535;
        if (in.gun_y < 0) y = 0; if (y > 65535) y = 65535;
        unsigned x2 = x, y2 = y;     /* channels 2/3: the second gun, or a copy of the first */
        if (in.gun2) {
            x2 = in.gun2_x < 0 ? 0 : (unsigned)((long)in.gun2_x * 65535 / w);
            y2 = in.gun2_y < 0 ? 0 : (unsigned)((long)in.gun2_y * 65535 / h);
            if (x2 > 65535) x2 = 65535; if (y2 > 65535) y2 = 65535;
        }
        {   static int shown;   /* LINDBERGH_JVS: how many channels the game reads */
            if (!shown && getenv("LINDBERGH_JVS")) { shown = 1; fprintf(stderr, "[jvs] analog read: %u channels\n", p[1]); }
        }
        ack_report();
        {   /* LINDBERGH_NO_GUN=1 reports a centred, still gun. A test, not a
             * setting: it tells apart a game upset by where the gun is from a
             * game upset by something else entirely. */
            static int flat = -1;
            if (flat < 0) { const char *v = getenv("LINDBERGH_NO_GUN");
                            flat = (v && *v && *v != '0') ? 1 : 0; }
            for (unsigned ch = 0; ch < p[1]; ch++) {
                unsigned v = flat ? 0x8000u
                           : ch == 0 ? x : ch == 1 ? y : ch == 2 ? x2 : ch == 3 ? y2 : 0x8000u;
                /* The game keeps only the first byte: amJvspAckAnalogInput
                 * copies the two bytes as they came and the game reads them
                 * as a little-endian word scaled by 1/256, which is right
                 * only when the second byte is zero - an 8-bit gun. A full
                 * 16-bit value lands the low byte on top and the aim is
                 * noise. */
                ack_byte((unsigned char)(v >> 8));
                ack_byte(0);
            }
        }
        break;
    }
    case 0x23: ack_report(); ack_zeros((unsigned)p[1] * 2u); break;  /* rotary */
    default:                                     /* acknowledged, nothing to say */
        ack_report();
        break;
    }
}

static void h_jvs_send(CPU *c)
{
    uint32_t pkt = A32(0);
    {   static unsigned calls;
        if (getenv("LINDBERGH_JVS") && (++calls <= 4 || calls % 300 == 0))
            fprintf(stderr, "[jvs] send #%u pkt %#x\n", calls, pkt); }
    if (!pkt) { RET(-7); return; }
    const unsigned char *p = (const unsigned char *)GP(pkt);
    const unsigned char *frame = p + 2;

    if (getenv("LINDBERGH_JVS")) jvs_dump("req", pkt, 2, p[1]);

    ack_begin();
    for (unsigned i = 0; i < 0x40 && p[PKT_INDEX + 0x101 + i]; i++) {
        /* The command index lives at 0x102 in the request, where the reply
         * frame lives in an acknowledge packet. Same offset, different use. */
        unsigned off = p[PKT_FRAME + i];
        /* The guard is against running off the packet, not off the frame: a
         * command sits at the end of its frame and its parameters are read
         * past p[1] quite legitimately. Comparing against the frame length
         * rejected every single-command request there is. */
        if (off < 3u || off + 2u >= 0x100u) break;
        jvs_command(frame + off);
    }
    ack_end();
    RET(AM_OK);
}

static void h_jvs_recv(CPU *c)
{
    uint32_t ack = A32(1);
    if (!ack || !g_ack_len) { RET(-6); return; }
    unsigned char *a = (unsigned char *)GP(ack);

    memcpy(a + PKT_FRAME, g_ack, g_ack_len);
    a[PKT_FRAME_LEN] = (unsigned char)g_ack_len;
    /* The report offsets, in the order the commands were asked, zero
     * terminated - the same shape amJvspMakeReportIndex would have left. */
    for (unsigned i = 0; i < g_reports; i++) a[PKT_INDEX + i] = g_report_at[i];
    a[PKT_INDEX + g_reports] = 0;

    if (getenv("LINDBERGH_JVS")) jvs_dump("ack", ack, PKT_FRAME, g_ack_len);
    RET(AM_OK);
}

static int g_country;
static void h_country(CPU *c) { RET(g_country); }

static void h_ok(CPU *c)   { RET(AM_OK); }
static void h_true(CPU *c) { RET(1); }
static void h_one(CPU *c)  { RET(1); }

/* A base board serial, in the shape the cabinet's own would take. */
static void h_serial_id(CPU *c)
{
    static const char id[] = "AAFE-01A00000000";
    uint32_t out = A32(0);
    if (out) memcpy((void *)GP(out), id, sizeof id);
    RET(AM_OK);
}

/* ---- the cabinet's non-volatile memory ----
 *
 * Bookkeeping, settings and credits live in two places on a Lindbergh: a
 * battery-backed SRAM on the base board and a small EEPROM. The game reaches
 * both through four wrapper functions, and everything above them - record
 * layout, duplicate copies, CRCs, validation - is the game's own code, which
 * works perfectly well as long as the bytes come back.
 *
 * So this is the whole of it: two byte arrays that persist to a file. Nothing
 * models a record, because nothing needs to.
 */

#define SRAM_SIZE   0x40000u
#define EEPROM_SIZE 0x10000u

static unsigned char g_sram[SRAM_SIZE];
static unsigned char g_eeprom[EEPROM_SIZE];
static int           g_nvram_loaded;

static const char *nvram_path(void)
{
    static char path[512];
    if (!*path) {
        const char *p = getenv("LINDBERGH_NVRAM");
        snprintf(path, sizeof path, "%s", (p && *p) ? p : "lindbergh_nvram.bin");
    }
    return path;
}

static int g_sysdata_region;    /* the game's region comes from the EEPROM's system data */
static int region_value(void);

/* The EEPROM's static system data record: 32 bytes at its start, a CRC-32
 * (no final inversion) of the other 28 first, the region at byte 14 (0 Japan,
 * 1 USA, 2 export), then the board's serial. A game that takes its region
 * from here (The House of the Dead 4 Special) gets the Language setting
 * written into it; a blank store gets a whole record. */
static void sysdata_apply_region(void)
{
    unsigned char *r = g_eeprom;
    int blank = 1;
    for (int i = 0; i < 32; i++) if (r[i]) { blank = 0; break; }
    if (blank) memcpy(r + 15, "AALG-TG-933F3904", 16);
    r[14] = (unsigned char)region_value();
    uint32_t crc = (uint32_t)crc32(0L, r + 4, 28) ^ 0xFFFFFFFFu;
    memcpy(r, &crc, 4);
    fprintf(stderr, "[sega] system data region %d\n", r[14]);
}

void sega_sysdata_region(int on) { g_sysdata_region = on; }

static void nvram_read(void);
static void nvram_load(void)
{
    if (g_nvram_loaded) return;
    g_nvram_loaded = 1;
    nvram_read();
    if (g_sysdata_region) sysdata_apply_region();
}

static void nvram_read(void)
{
    FILE *f = fopen(nvram_path(), "rb");
    if (!f) return;
    if (fread(g_sram, 1, sizeof g_sram, f) != sizeof g_sram ||
        fread(g_eeprom, 1, sizeof g_eeprom, f) != sizeof g_eeprom) {
        /* A short or corrupt file is not worth keeping: the game rebuilds its
         * records from defaults the moment it finds them invalid. */
        memset(g_sram, 0, sizeof g_sram);
        memset(g_eeprom, 0, sizeof g_eeprom);
    }
    fclose(f);
}

static void nvram_save(void)
{
    FILE *f = fopen(nvram_path(), "wb");
    if (!f) return;
    fwrite(g_sram, 1, sizeof g_sram, f);
    fwrite(g_eeprom, 1, sizeof g_eeprom, f);
    fclose(f);
}

/* The wrappers answer 0, or -12 the way the real ones do when the base board
 * does not reply. An access outside the store is that failure. */
#define AM_BACKUP_FAIL (-12)

static int nvram_copy(unsigned char *store, unsigned size, uint32_t guest,
                      uint32_t addr, uint32_t len, int writing)
{
    nvram_load();
    if (!guest || addr > size || len > size - addr) return AM_BACKUP_FAIL;
    void *g = (void *)GP(guest);
    if (writing) { memcpy(store + addr, g, len); nvram_save(); }
    else           memcpy(g, store + addr, len);
    return AM_OK;
}

/* Each wrapper reorders its arguments on the way down to a different device,
 * so which of the three is the buffer is not the same for any two of them.
 * LINDBERGH_NVLOG=1 prints all three and lets their magnitude say: a guest
 * pointer is up in the image, an offset is small. Read takes the offset
 * first, write takes the buffer first - reading both the same way puts a
 * pointer's value in as an address and reports a broken record. */
static void nv_log(const char *who, CPU *c)
{
    if (getenv("LINDBERGH_NVLOG"))
        fprintf(stderr, "[nv] %-10s a0=0x%08X a1=0x%08X a2=0x%08X\n",
                who, A32(0), A32(1), A32(2));
}

/* The EEPROM, reached directly rather than through the backup record layer.
 *
 * OutRun 2 SP SDX saves its assignment data by calling amEepromRead and
 * amEepromWrite itself. Their argument orders differ from each other the same
 * way the SRAM wrappers do - read takes the address first, write takes the
 * buffer first - and the address is a sixteen bit word in both. */
static void h_eeprom_read_api(CPU *c)
{
    nv_log("eepromRead", c);
    RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(1), A32(0) & 0xFFFFu, A32(2), 0));
}

static void h_eeprom_write_api(CPU *c)
{
    nv_log("eepromWrite", c);
    RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(0), A32(1) & 0xFFFFu, A32(2), 1));
}

static void h_sram_read(CPU *c)
{ nv_log("sramRead", c);  RET(nvram_copy(g_sram, SRAM_SIZE, A32(1), A32(0), A32(2), 0)); }
static void h_sram_write(CPU *c)
{ nv_log("sramWrite", c); RET(nvram_copy(g_sram, SRAM_SIZE, A32(0), A32(1), A32(2), 1)); }

/* The EEPROM pair reorders differently again and has not been reached yet;
 * these follow the SRAM shape and the log will say if that is wrong. */
static void h_eeprom_read(CPU *c)
{ nv_log("eepRead", c);   RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(1), A32(0), A32(2), 0)); }
static void h_eeprom_write(CPU *c)
{ nv_log("eepWrite", c);  RET(nvram_copy(g_eeprom, EEPROM_SIZE, A32(0), A32(1), A32(2), 1)); }

/* ---- the error the blank store raises ----
 *
 * acpSystem keeps one error code in a global and paints it over the screen
 * once set; anything other than -1 or -2 comes out as "Error 15 - Game
 * Program Not Found". The backup layer sets -3 when its records fail their
 * CRC, which they do here because the store is blank: the game's own repair
 * path writes through an EEPROM that lives on an I2C bus this runtime does
 * not have, so the records can never become valid and the error is permanent.
 *
 * A cabinet ships with that store already written. Suppressing this one code
 * is the same statement - the bookkeeping is assumed good - and it is scoped
 * to exactly that: every other error still reaches the game untouched, and
 * the suppression announces itself once on the way past.
 *
 * setError is reimplemented rather than skipped, which means knowing the two
 * globals it touches. Those are read out of the function's own first bytes
 * instead of being written down here, so this survives a different build of
 * the same framework:
 *
 *     55                 push ebp
 *     A1 <error var>     mov  eax, [error var]
 *     80 3D <latch> 00   cmp  byte [latch], 0
 *     ...
 *     A3/89 05 <var>     mov  [error var], eax
 */
#define ACP_ERR_BACKUP (-3)

static uint32_t g_err_var, g_err_latch;

static uint32_t rd32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int find_error_globals(uint32_t va)
{
    const unsigned char *p = (const unsigned char *)GP(va);
    for (unsigned i = 0; i < 24; i++) {
        if (!g_err_var && p[i] == 0xA1) g_err_var = rd32le(p + i + 1);
        if (!g_err_latch && p[i] == 0x80 && p[i + 1] == 0x3D)
            g_err_latch = rd32le(p + i + 2);
    }
    return g_err_var != 0;
}

static void h_set_error(CPU *c)
{
    int32_t v = (int32_t)A32(0);

    if (v == ACP_ERR_BACKUP) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "[sega] backup records are blank and cannot be "
                            "initialised without the EEPROM bus; treating the "
                            "bookkeeping as good\n");
        }
        RET(0);
        return;
    }

    /* Faithful otherwise: the original only stores while the latch is clear,
     * so that the first error to happen is the one that gets reported. */
    if (g_err_var &&
        (!g_err_latch || *(const unsigned char *)GP(g_err_latch) == 0))
        *(uint32_t *)GP(g_err_var) = (uint32_t)v;

    if (getenv("LINDBERGH_ACPERR")) {
        fprintf(stderr, "[acp] setError(%d) from:\n", (int)v);
        guest_backtrace(c);
    }
    RET(0);
}

/* ---- the security dongle ----
 *
 * A Lindbergh cabinet carries a hardware dongle, and a game does not simply
 * ask whether it is present. Ghost Squad Evolution generates sixteen random
 * bytes, has the dongle transform them, and checks the answer. The key for
 * that transform lives inside the device; it is not in the disc, not in the
 * binary, and not recoverable from either.
 *
 * So there is nothing to emulate here and no cleverness that would help. The
 * cabinet this game ran on had the dongle fitted, and the runtime makes the
 * same statement - the licence is present - by answering the one predicate
 * the game gates on, and saying so once on the way past. The challenge and
 * response themselves are left entirely alone.
 *
 * Bound by symbol name, so it applies to a game that has these and is simply
 * absent from one that does not. */
static void h_dongle_valid(CPU *c)
{
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "[sega] no security dongle is fitted; treating the "
                        "licence as present\n");
    }
    RET(1);
}

/* amDipswGetData(unsigned char *out): the baseboard's DIP switches. On a
 * Lindbergh, switches 6-8 (bits 5-7) select the monitor's resolution, and a
 * game sizes everything it draws from them; with no answer it falls back to
 * 640x480 whatever the window is. So the bits are the window's size:
 *   0x00 640x480   0x20 800x480   0x40 1024x768  0x60 1280x768
 *   0x80 800x600   0xA0 1024x600  0xC0 1280x1024 0xE0 1280x720 (1360x768
 *   to House of the Dead 4, which renders and aims for that)
 * LINDBERGH_DIPSW=<hex> sets the whole byte instead. */
static void h_dipsw_get(CPU *c)
{
    static int dips = -1;
    if (dips < 0) {
        const char *v = getenv("LINDBERGH_DIPSW");
        if (v && *v) dips = (int)strtol(v, NULL, 16) & 0xFF;
        else {
            static const struct { int w, h, bits; } res[] = {
                { 640, 480, 0x00 }, { 800, 480, 0x20 }, { 1024, 768, 0x40 }, { 1280, 768, 0x60 },
                { 800, 600, 0x80 }, { 1024, 600, 0xA0 }, { 1280, 1024, 0xC0 }, { 1280, 720, 0xE0 },
                { 1360, 768, 0xE0 },    /* what 0xE0 is to House of the Dead 4 */
            };
            int w, h;
            lindbergh_window_size(&w, &h);
            dips = 0x60;
            for (unsigned i = 0; i < sizeof res / sizeof *res; i++)
                if (res[i].w == w && res[i].h == h) dips = res[i].bits;
        }
        fprintf(stderr, "[sega] DIP switches %#04x\n", dips);
    }
    if (A32(0)) *(uint8_t *)APTR(0) = (uint8_t)dips;
    RET(0);
}

static int region_value(void)
{
    const char *v = getenv("LINDBERGH_REGION");
    if (v && *v) return atoi(v);
    return g_country ? 1 : 0;
}
static void h_region(CPU *c) { RET(region_value()); }
#if defined(__ANDROID__)
/* Ghost Squad Evolution paces itself: idle_wait_func spins on the wall clock
 * until 16.683 ms (59.94 Hz) have passed since the frame began. The headset
 * shows 120 Hz, two display frames per game frame at exactly 60: at 59.94 the
 * game slid through the display's phase, and each time it crossed a display
 * frame one game frame was shown once and the next three times - judder.
 * The frame target is read only by the waits there, so for the call it is
 * the display's own (host_frame_period_ms: the headset's period times its
 * frames per game frame). Pacing by the display alone was tried: the game
 * then begins a frame only when the headset releases it, too late to finish
 * in two display frames. The same constant elsewhere (main_loop_func's frame
 * ratios) is untouched. */
#define FRAME_TARGET_VA 0x081c1d40u
static void (*g_idle_wait)(CPU *);
static void h_idle_wait(CPU *c)
{
    uint32_t keep = rd32(FRAME_TARGET_VA), now;
    float t = host_frame_period_ms();
    if (!(t > 5.0f && t < 40.0f)) memcpy(&t, &keep, 4);
    memcpy(&now, &t, 4);
    wr32(FRAME_TARGET_VA, now);
    push32(c, 0xDEADBEEFu);                 /* the return slot its ret pops */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    g_idle_wait(c);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    wr32(FRAME_TARGET_VA, keep);
    {   /* headroom: how long the frame waited, for the 5-second report */
        static double sum, lo = 1e9; static unsigned n, tight;
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        sum += ms; if (ms < lo) lo = ms; if (ms < 1.0) tight++;
        if (++n == 300) {
            fprintf(stderr, "[idle] frame wait: mean %.1f ms, least %.1f ms, under 1 ms %u of 300\n", sum / n, lo, tight);
            sum = 0; lo = 1e9; n = 0; tight = 0;
        }
    }
}
static int pace_by_display(void)
{
    uint32_t va = guest_symbol("_Z14idle_wait_funcv");
    extern void (*dispatch_lifted(uint32_t va))(CPU *);
    uint32_t bits = va ? rd32(FRAME_TARGET_VA) : 0;
    float target;
    memcpy(&target, &bits, 4);
    if (!va || target < 16.6f || target > 16.8f || !(g_idle_wait = dispatch_lifted(va))) return 0;
    return guest_override("_Z14idle_wait_funcv", h_idle_wait);
}
#endif

/* The game culls its models against a viewing volume as wide as its camera's
 * lens (dxMakePerspectiveViewingVolume, from set_camera_transform). In the
 * headset the view is far wider than the arcade monitor - about 100 degrees,
 * and the head turns - so a soldier who steps close and to the side was
 * dropped while still in view. In VR the volume is built wider than the
 * camera (LINDBERGH_CULL_FOV, degrees, vertical; 0 keeps the game's). Only
 * the culling is widened: the projection is set elsewhere and unchanged. */
static void (*g_view_volume)(CPU *);
static float g_cull_fov;
static void h_view_volume(CPU *c)
{
    uint32_t bits = A32(0);
    float fov;
    memcpy(&fov, &bits, 4);
    static int force = -1;               /* LINDBERGH_CULL_FORCE=1: exactly that angle, narrower too (tests) */
    if (force < 0) { const char *f = getenv("LINDBERGH_CULL_FORCE"); force = f && *f == '1'; }
    if (fov > 0 && (fov < g_cull_fov || force)) {
        memcpy(&bits, &g_cull_fov, 4);
        wr32(c->esp, bits);             /* the caller's argument, its own to clean */
    }
    push32(c, 0xDEADBEEFu);             /* the return slot its ret pops */
    g_view_volume(c);
}
static int widen_culling(void)
{
    const char *v = getenv("LINDBERGH_CULL_FOV");
    g_cull_fov = v ? (float)atof(v) : 0.0f;  /* off unless asked for */
    if (!(g_cull_fov > 0 && g_cull_fov < 179)) return 0;
    uint32_t va = guest_symbol("_Z30dxMakePerspectiveViewingVolumeffff");
    extern void (*dispatch_lifted(uint32_t va))(CPU *);
    if (!va || !(g_view_volume = dispatch_lifted(va))) return 0;
    return guest_override("_Z30dxMakePerspectiveViewingVolumeffff", h_view_volume);
}

/* LINDBERGH_GOD=1: the player takes no damage - for test runs that must
 * reach the end of a mission. */
static void h_no_damage(CPU *c) { RET(0); }

/* The player's route. A mission is a chain of routes (a static table of 76,
 * each a path, the next route and an area); the player's record holds the
 * current one. LINDBERGH_ROUTE_LOG=1 reports each change; LINDBERGH_ROUTE=N
 * puts the player on route N a second into the mission - a jump, e.g. to
 * a mission's last scene. */
#define PLAYER0_VA   0x086617e0u
#define ROUTE_TABLE  0x08276ea0u
static void (*g_game_loop)(CPU *);
static int g_route_jump = -1, g_route_log;
static int player_route(void)
{
    uint32_t pw = rd32(PLAYER0_VA + 0xf4);
    if (!pw) return -1;
    uint32_t r = rd32(pw + 0x100);
    for (int i = 0; r && i < 76; i++) if (rd32(ROUTE_TABLE + 4u * i) == r) return i;
    return -1;
}
static void h_game_loop(CPU *c)
{
    static int last = -2, loops;
    extern unsigned g_frame;
    int r = player_route();
    if (g_route_log && r != last)
        fprintf(stderr, "[route] frame %u: route %d (path %d, area %d)\n", g_frame, r,
                r >= 0 ? (int)rd32(rd32(ROUTE_TABLE + 4u * r) + 4) : -1, r >= 0 ? (int)rd32(rd32(ROUTE_TABLE + 4u * r) + 16) : -1);
    last = r;
    if (r >= 0) loops++; else loops = 0;
    {   /* LINDBERGH_HURT=N (test): the player takes a hit every N frames of play */
        static int hurt = -1;
        if (hurt < 0) { const char *h = getenv("LINDBERGH_HURT"); hurt = h ? atoi(h) : 0; }
        if (hurt > 0 && loops > 0 && loops % hurt == 0) {
            uint32_t fn = guest_symbol("_Z17set_player_damageiiii");
            uint32_t a[4] = { 0, 1, 0, 0 };
            if (fn) guest_call(c, fn, a, 4);
            fprintf(stderr, "[sega] frame %u: test hit\n", g_frame);
        }
    }
    if (g_route_jump >= 0 && loops == 60) {
        uint32_t fn = guest_symbol("_Z19set_player_route_idii");
        uint32_t a[2] = { 0, (uint32_t)g_route_jump };
        uint32_t ok = fn ? guest_call(c, fn, a, 2) : 0;
        uint32_t path = rd32(rd32(PLAYER0_VA + 0xf4) + 0x108);    /* its path: start and end frame */
        int p0 = path ? (int)rd32(path) : 0, p1 = path ? (int)rd32(path + 4) : 0;
        fprintf(stderr, "[route] frame %u: jumped to route %d (%s), its path frames %d..%d\n", g_frame, g_route_jump, ok ? "ok" : "refused", p0, p1);
        g_route_jump = -1;
        const char *pf = getenv("LINDBERGH_PATHFRAME");   /* and this far along it (negative: from its end) */
        uint32_t fs = guest_symbol("_Z20set_player_pathframeif");
        if (ok && pf && *pf && fs) {
            float f = (float)atof(pf);
            f += f < 0 ? p1 : p0;
            uint32_t b2[2] = { 0, 0 };
            memcpy(&b2[1], &f, 4);
            guest_call(c, fs, b2, 2);
            fprintf(stderr, "[route] frame %u: path frame set to %.0f\n", g_frame, f);
        }
    }
    {   /* LINDBERGH_SHIMMER=<strength>: the plane finale's air shimmer, in any scene - for testing it */
        static float sh = -1;
        if (sh < 0) { const char *v = getenv("LINDBERGH_SHIMMER"); sh = v && *v ? (float)atof(v) : 0; }
        if (sh > 0 && loops >= 60) {
            uint32_t g = guest_symbol("_Z12efcSetGlobaliifPv"), w = guest_symbol("_Z17efcSetShimmerWarpf");
            float one = 1.0f; uint32_t a[4] = { 7, 1, 0, 0 }, b1[1];
            memcpy(&a[2], &one, 4); memcpy(&b1[0], &sh, 4);
            if (loops == 60 && g) fprintf(stderr, "[sega] shimmer on: %u\n", guest_call(c, g, a, 4));
            if (w) guest_call(c, w, b1, 1);
        }
    }
    {   /* LINDBERGH_GUNACT_LOG=1: player 1's gun - its input words (held,
         * pressed) and the 16 gun actions the game derives from them
         * (is_gun_action; CQC asks for action 14) - on every change */
        static int on = -1;
        if (on < 0) { const char *v = getenv("LINDBERGH_GUNACT_LOG"); on = v && *v == '1'; }
        if (on) {
            enum { GUN0 = 0x8651e60 };
            uint32_t in = rd32(GUN0 + 0x40);
            uint32_t now[4] = { rd32(GUN0 + 0x64), in ? rd32(in + 4) : 0, in ? rd32(in + 8) : 0, rd32(GUN0 + 0x4) };
            static uint32_t last[4];
            if (memcmp(now, last, sizeof now)) {
                fprintf(stderr, "[gunact] frame %u: actions %#x, held %#x, pressed %#x, gun flags %#x\n",
                        g_frame, now[0], now[1], now[2], now[3]);
                memcpy(last, now, sizeof now);
            }
        }
    }
    push32(c, 0xDEADBEEFu);
    g_game_loop(c);
}
/* The sniping and CQC events always report to stereo.c: they change its
 * scope test and how the gun is aimed. LINDBERGH_QTE_LOG=1: each frame of CQC (qte_ctrl_grapple) and of the
 * sniping events (qte_ctrl_snipe): the markers CQC checks the gun against,
 * in screen coordinates, and where the cabinet says the gun points. */
static void (*g_qte_grapple)(CPU *), (*g_qte_snipe)(CPU *);
static void qte_log(const char *what, uint32_t q)
{
    extern unsigned g_frame;
    CabinetInput in;
    host_cabinet_input(&in);
    char marks[256] = "";
    int n = (int)rd32(q + 0x38), len = 0;
    for (int i = 0; i < n && i < 8 && len < 200; i++) {
        uint32_t e = q + 0x40 + 0x14u * i;
        len += snprintf(marks + len, sizeof marks - len, " %d@%d,%d", (int16_t)rd32(e) , (int16_t)rd32(e + 4), (int16_t)rd32(e + 6));
    }
    fprintf(stderr, "[qte] frame %u: %s, state %d, gun %d,%d buttons %#x, %d marks:%s\n", g_frame, what,
            (int)rd32(q + 4), in.gun_x, in.gun_y, in.buttons, n, marks);
}
static int g_qte_log;
static void h_qte_grapple(CPU *c)
{
#ifndef _WIN32
    stereo_cqc_running();
#endif
    if (g_qte_log) qte_log("CQC", A32(0));
    push32(c, 0xDEADBEEFu);
    g_qte_grapple(c);
}
static void h_qte_snipe(CPU *c)
{
#ifndef _WIN32
    stereo_snipe_running();
#endif
    if (g_qte_log) qte_log("snipe", A32(0));
    push32(c, 0xDEADBEEFu);
    g_qte_snipe(c);
}
static int watch_qte(void)
{
    const char *v = getenv("LINDBERGH_QTE_LOG");
    g_qte_log = v && *v == '1';
    extern void (*dispatch_lifted(uint32_t va))(CPU *);
    uint32_t g = guest_symbol("_Z16qte_ctrl_grappleP3QTE"), s = guest_symbol("_Z14qte_ctrl_snipeP3QTE");
    int n = 0;
    if (g && (g_qte_grapple = dispatch_lifted(g))) n += guest_override("_Z16qte_ctrl_grappleP3QTE", h_qte_grapple);
    if (s && (g_qte_snipe = dispatch_lifted(s))) n += guest_override("_Z14qte_ctrl_snipeP3QTE", h_qte_snipe);
    return n;
}

static int watch_route(void)
{
    const char *j = getenv("LINDBERGH_ROUTE"), *l = getenv("LINDBERGH_ROUTE_LOG");
    g_route_log = l && *l == '1';
    if (j && *j) g_route_jump = atoi(j);
    if (!g_route_log && g_route_jump < 0 && !getenv("LINDBERGH_SHIMMER") && !getenv("LINDBERGH_HURT") && !getenv("LINDBERGH_GUNACT_LOG")) return 0;
    uint32_t va = guest_symbol("_Z8GameLoopP8GAMEWORK");
    extern void (*dispatch_lifted(uint32_t va))(CPU *);
    if (!va || !(g_game_loop = dispatch_lifted(va))) return 0;
    return guest_override("_Z8GameLoopP8GAMEWORK", h_game_loop);
}

/* Virtua Tennis 3: jumps to a broken section, for reproducing bugs without
 * playing up to them. The match object (g_pTennis + 0x1c) holds the round of
 * a Road to the Championship run at +0x0c (match::setRound; setNext_round adds
 * one per win; setMatching picks that round's opponent from +0xac), and the
 * current game's points at +0x06 / +0x07 (0, 1, 2, 3 = 0, 15, 30, 40; zeroed by
 * match::init and calcGame).
 *   LINDBERGH_VT3_ROUND=N   the run's first match is round N (0-based)
 *   LINDBERGH_VT3_POINTS=a,b every game starts at those points (3,0 = 40-0)
 *   LINDBERGH_VT3_LOG=1     each matching, next round and new game logged
 *   LINDBERGH_VT3_SPLIT=1   every point ends on the split-screen camera:
 *                           camera::initWinlose picks it when its first
 *                           getRandMT_U32() % 10 is 0 (singles), so that one
 *                           number comes back 0 - 1 point in 10 otherwise
 *   LINDBERGH_VT3_WIN=1     1P wins every point (the point's winner, match
 *                           +0x61, is set to player 0 before match::calcPoint
 *                           scores it): matches won in a few rallies
 *   LINDBERGH_VT3_PRIZE_TRACE=N  LINDBERGH_TRACE_VP starts N frames after the
 *                           result slate's first prize coin sound (its
 *                           counter rolling), whatever frame that lands on */
#define VT3_G_PTENNIS 0x085061a4u
static void (*g_vt3_set_matching)(CPU *), (*g_vt3_next_round)(CPU *), (*g_vt3_match_init)(CPU *), (*g_vt3_calc_game)(CPU *);
static int g_vt3_round = -1, g_vt3_pts[2] = { -1, -1 }, g_vt3_log;
static void h_vt3_set_matching(CPU *c)
{
    extern unsigned g_frame;
    uint32_t m = A32(0);
    if (g_vt3_round >= 0) {
        fprintf(stderr, "[vt3] frame %u: round %u -> %d (LINDBERGH_VT3_ROUND)\n", g_frame, rd32(m + 0x0c), g_vt3_round);
        wr32(m + 0x0c, (uint32_t)g_vt3_round);
        g_vt3_round = -1;                    /* once: later wins advance from there */
    }
    push32(c, 0xDEADBEEFu);
    g_vt3_set_matching(c);
    if (g_vt3_log)
        fprintf(stderr, "[vt3] frame %u: setMatching, round %u, mode %u\n", g_frame, rd32(m + 0x0c), rd32(rd32(VT3_G_PTENNIS) + 0x24));
}
static void h_vt3_next_round(CPU *c)
{
    extern unsigned g_frame;
    uint32_t m = A32(0), before = rd32(m + 0x0c);
    push32(c, 0xDEADBEEFu);
    g_vt3_next_round(c);
    fprintf(stderr, "[vt3] frame %u: setNext_round, round %u -> %u\n", g_frame, before, rd32(m + 0x0c));
}
/* A new game: the points are 0-0 after match::init (the match's first) and
 * after calcGame (each finished game; match::initGame is never called) */
static void vt3_new_game(uint32_t m, const char *by)
{
    extern unsigned g_frame;
    if (rd8(m + 0x06) || rd8(m + 0x07)) return;     /* calcGame without a finished game */
    if (g_vt3_pts[0] >= 0) {
        wr8(m + 0x06, (uint8_t)g_vt3_pts[0]);
        wr8(m + 0x07, (uint8_t)g_vt3_pts[1]);
    }
    if (g_vt3_log)
        fprintf(stderr, "[vt3] frame %u: new game (%s), points %u-%u, round %u\n", g_frame, by, rd8(m + 0x06), rd8(m + 0x07), rd32(m + 0x0c));
}
static void h_vt3_match_init(CPU *c)
{
    uint32_t m = A32(0);
    push32(c, 0xDEADBEEFu);
    g_vt3_match_init(c);
    vt3_new_game(m, "match");
}
static void h_vt3_calc_game(CPU *c)
{
    uint32_t m = A32(0);
    push32(c, 0xDEADBEEFu);
    g_vt3_calc_game(c);
    vt3_new_game(m, "game");
}
/* camera::initWinlose's first random number picks the close-up: 0 of 10 is
 * the split screen (camera::setSplit_screen, both players side by side).
 * LINDBERGH_VT3_SPLIT=1 (test) answers 0 every time; a point cutaway drawn in
 * the world (Immersive, Point cutaways In the world / Camera view) never
 * takes it - two pictures of the scene would land on top of each other - the
 * roll is moved off 0 and the game takes its usual single close-up. The flag
 * is the calling thread's: the camera's routine runs on a worker thread. */
static void (*g_vt3_init_winlose)(CPU *), (*g_vt3_rand)(CPU *);
static int g_vt3_split;
static __thread int g_vt3_split_next;
static int vt3_cut_in_world(void)
{
    const int v = world_mode_cutaway_view();
    return world_mode_immersive() && (v == LG_VIEW_WORLD || v == LG_VIEW_CAMERA);
}
static void h_vt3_init_winlose(CPU *c)
{
    extern unsigned g_frame;
    g_vt3_split_next = 1;                   /* the next random number is the camera's pick */
    push32(c, 0xDEADBEEFu);
    g_vt3_init_winlose(c);
    if (g_vt3_split_next == 2 && g_vt3_log) fprintf(stderr, "[vt3] frame %u: point over, split-screen camera forced\n", g_frame);
    if (g_vt3_split_next == 3) fprintf(stderr, "[vt3] frame %u: point over, split-screen close-up left out (cutaway in the world)\n", g_frame);
    g_vt3_split_next = 0;
}
static void h_vt3_rand(CPU *c)
{
    if (g_vt3_split_next == 1) {
        if (vt3_cut_in_world()) {
            push32(c, 0xDEADBEEFu); g_vt3_rand(c);
            g_vt3_split_next = c->eax % 10u == 0 || g_vt3_split ? 3 : 4;
            if (c->eax % 10u == 0) c->eax += 1;
            return;
        }
        if (g_vt3_split) { g_vt3_split_next = 2; RET(0); return; }
        g_vt3_split_next = 4;
    }
    push32(c, 0xDEADBEEFu);
    g_vt3_rand(c);
}
static void (*g_vt3_calc_point)(CPU *);
static void h_vt3_calc_point(CPU *c)
{
    wr8(A32(0) + 0x61, 0);                  /* the point goes to 1P */
    push32(c, 0xDEADBEEFu);
    g_vt3_calc_point(c);
}
static void (*g_vt3_prize_coin)(CPU *);
static long g_vt3_prize_trace = -1;
static void h_vt3_prize_coin(CPU *c)
{
    extern unsigned g_frame;
    extern long g_arb_trace_from;
    if (g_vt3_prize_trace >= 0) {
        g_arb_trace_from = (long)g_frame + g_vt3_prize_trace;
        fprintf(stderr, "[vt3] frame %u: prize counter rolling, program trace from frame %ld\n", g_frame, g_arb_trace_from);
        g_vt3_prize_trace = -1;             /* the first coin only */
    }
    push32(c, 0xDEADBEEFu);
    g_vt3_prize_coin(c);
}
int g_arcade_overlays = 1, g_arcade_overlay_hooks;
/* Virtua Tennis 3's arcade overlays: credit::draw (CREDIT(S), INSERT COIN(S),
 * PRESS START BUTTON), skipped while g_arcade_overlays is 0, and the card
 * reader's message windows at the top (CCardSlotWindowManager::window_open,
 * window_slot, window_slot_usecard), not opened while it is 0 - those come back
 * with the next window the game opens. */
static void (*g_vt3_credit_draw)(CPU *), (*g_vt3_slot_open)(CPU *), (*g_vt3_slot)(CPU *), (*g_vt3_slot_card)(CPU *);
/* ...and the card information board's own credit line (ab_card_info: at
 * +0x14 a table of 3 {font, text} entries it alternates, "CREDIT 0" and
 * "INSERT COIN(S)"): the texts pointed at an empty string while hidden, the
 * originals put back when shown. Applied from credit::draw, every frame on
 * the game thread, when the setting changes. */
static void (*g_vt3_card_init)(CPU *);
static uint32_t g_vt3_card_table, g_vt3_card_text[3], g_vt3_empty;
static int g_vt3_card_shown = -1;
static void vt3_card_apply(void)
{
    if (!g_vt3_card_table || g_vt3_card_shown == g_arcade_overlays) return;
    if (!g_vt3_empty) { char *e = guest_alloc(4, 4); if (!e) return; e[0] = 0; g_vt3_empty = H2G(e); }
    for (int i = 0; i < 3; i++) wr32(g_vt3_card_table + 8u * (uint32_t)i + 4, g_arcade_overlays ? g_vt3_card_text[i] : g_vt3_empty);
    g_vt3_card_shown = g_arcade_overlays;
}
static void h_vt3_card_init(CPU *c)
{
    uint32_t self = A32(0);
    push32(c, 0xDEADBEEFu); g_vt3_card_init(c);
    g_vt3_card_table = rd32(self + 0x14);
    for (int i = 0; i < 3 && g_vt3_card_table; i++) g_vt3_card_text[i] = rd32(g_vt3_card_table + 8u * (uint32_t)i + 4);
    g_vt3_card_shown = -1;
    vt3_card_apply();
}
static void h_vt3_credit_draw(CPU *c) { vt3_card_apply(); if (!g_arcade_overlays) return; push32(c, 0xDEADBEEFu); g_vt3_credit_draw(c); }
static void h_vt3_slot_open(CPU *c) { if (!g_arcade_overlays) return; push32(c, 0xDEADBEEFu); g_vt3_slot_open(c); }
static void h_vt3_slot(CPU *c) { if (!g_arcade_overlays) return; push32(c, 0xDEADBEEFu); g_vt3_slot(c); }
static void h_vt3_slot_card(CPU *c) { if (!g_arcade_overlays) return; push32(c, 0xDEADBEEFu); g_vt3_slot_card(c); }
/* Virtua Tennis 3's camera, as the game has it apart: _sRender::
 * setProjectCameraMatrix multiplies the current scene manager's view (3x4
 * at +0x2c) by its projection (4x4 at +0x5c) into the one matrix its shaders
 * get - which the stereo path could only take apart again, and not its depth
 * (the model's terms mix in). After it runs, the projection is handed to
 * arbprog.c (arb_note_projection) for the world mode. */
/* The world camera handed to arbprog.c and stereo.c (arb_note_projection)
 * in the GL queue, in order with the draws: the game sets its camera after a
 * frame's draws, for the next frame, while the GL thread may still be drawing
 * the frame before - read straight from the game thread, the draws of the
 * last frame before a cut were taken apart with the new camera (a turned,
 * distorted in-between frame at every cut in Immersive). */
/* ...with the camera's mode (camera+0x38; ~0u for one built here) and
 * whether the replay rewound since the last (replay::reverseFrame: a new shot
 * of the same moment), for the replays and close-ups drawn in the world
 * (stereo.c stereo_vt3_view) */
typedef struct { float p[16], v[12]; int feeding; unsigned mode; int cut, at_far;
                 float tgt[3]; int has_tgt; float pl[4][3]; int npl; } Vt3Cam;   /* + the camera's target, the players' roots (Camera view) */
static void vt3_cam_job(void *arg)
{
    Vt3Cam *k = (Vt3Cam *)arg;
    extern int arb_note_projection(const float *proj, const float *view); extern int g_vt3_feeding;
    extern void stereo_vt3_view(unsigned mode, const float *view, int cut, const float *proj, const float *target, const float (*pl)[3], int npl);
    int was = g_vt3_feeding; g_vt3_feeding = k->feeding;
    int took = arb_note_projection(k->p, k->v);
    g_vt3_feeding = was;
    if (took == 1 && !k->feeding) {
        extern void stereo_vt3_far_end(int at_far);
        float P[16];        /* GL's layout, as arbprog.c takes it */
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) P[c * 4 + r] = k->p[r * 4 + c];
        stereo_vt3_view(k->mode, k->v, k->cut, P, k->has_tgt ? k->tgt : NULL, (const float (*)[3])k->pl, k->npl);
        stereo_vt3_far_end(k->at_far);
    }
    free(k);
}
static int g_vt3_far;           /* the human's player at the far end of the court (vt3_far_end_note) */
static uint32_t g_vt3_camera;   /* the camera object (camera::gameRoutine's / initUva's this): its mode at +0x38 */
static float vt3_rf(uint32_t a) { uint32_t u = rd32(a); float f; memcpy(&f, &u, 4); return f; }
/* the camera's target (+0x44, written by camera::replay / winlose) and the
 * players' roots (+0x18 of each in [g_pTennis+4]) for the Camera view */
static void vt3_camera_extras(Vt3Cam *k)
{
    if (g_vt3_camera) {
        for (int i = 0; i < 3; i++) k->tgt[i] = vt3_rf(g_vt3_camera + 0x44 + 4u * (uint32_t)i);
        k->has_tgt = isfinite(k->tgt[0]) && isfinite(k->tgt[1]) && isfinite(k->tgt[2]);
    }
    uint32_t t = rd32(VT3_G_PTENNIS), n = t ? rd32(t) : 0, arr = t ? rd32(t + 4) : 0;
    for (uint32_t i = 0; i < n && i < 4 && arr; i++) {
        uint32_t pl = rd32(arr + 4 * i);
        if (!pl) continue;
        for (int c = 0; c < 3; c++) k->pl[k->npl][c] = vt3_rf(pl + 0x18 + 4u * (uint32_t)c);
        k->npl++;
    }
}
static void vt3_note_projection(const float *p, const float *v, int feeding, unsigned mode, int cut)
{
    extern int glq_active(void); extern uint32_t glq_job(void (*job)(void *), void *arg);
    static int queued = -1;
    if (queued < 0) { const char *e = getenv("LINDBERGH_VT3_CAM_QUEUE"); queued = !(e && *e == '0'); }
    Vt3Cam *k = (Vt3Cam *)malloc(sizeof *k);
    if (!k) return;
    memcpy(k->p, p, sizeof k->p); memcpy(k->v, v, sizeof k->v); k->feeding = feeding; k->mode = mode; k->cut = cut; k->at_far = g_vt3_far;
    k->has_tgt = 0; k->npl = 0;
    if (!feeding && (mode == 6 || mode == 7)) vt3_camera_extras(k);
    if (queued && glq_active()) glq_job(vt3_cam_job, k); else vt3_cam_job(k);
}
/* VR's camera height at the far end (T12, owner: "on the backside of the
 * court make camera height +2 m"): the court camera does not keep the human
 * at the near end - after a change of ends the human (player +0x08 <= 3 in
 * [g_pTennis+4]) plays from the far side, its root z (+0x18 +8) of the other
 * sign than the camera's eye. Latched only outside a rally (the arcade loop
 * not GameLoop, mode 4: the switch comes with the change of ends, never in
 * play) and only once it held for 10 camera notes (~3 frames); handed to
 * the GL thread with the camera (stereo_vt3_far_end). */
static unsigned g_vt3_loop = ~0u;       /* the arcade loop manager's mode last set (h_vt3_set_mode) */
static void vt3_far_end_note(unsigned mode, const float *v)
{
    if (mode != 1 && mode != 2) return;
    uint32_t t = rd32(VT3_G_PTENNIS), n = t ? rd32(t) : 0, arr = t ? rd32(t + 4) : 0;
    int found = 0; float pz = 0;
    for (uint32_t i = 0; i < n && i < 4 && arr; i++) {
        uint32_t pl = rd32(arr + 4 * i);
        if (pl && (int32_t)rd32(pl + 8) <= 3) { uint32_t b = rd32(pl + 0x18 + 8); memcpy(&pz, &b, 4); found = 1; break; }
    }
    const float ez = -(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]);
    const int at_far = found && fabsf(pz) > 5.0f && fabsf(ez) > 50.0f && pz * ez < 0;
    static int held, last = -1;
    held = at_far == last ? held + 1 : 0; last = at_far;
    if (at_far != g_vt3_far && held >= 10 && g_vt3_loop != 4) {
        extern unsigned g_frame;
        g_vt3_far = at_far;
        fprintf(stderr, "[vt3] frame %u: your player at the %s end (player z %.0f, camera z %.0f)%s\n", g_frame, at_far ? "far" : "near", pz, ez,
                at_far ? " - VR camera height High" : "");
    }
}
static void (*g_vt3_proj_cam)(CPU *);
/* replay::reverseFrame (a two-pass replay shot's rewind): runs inside the
 * camera's routine, on one of the game's worker threads - only counted here,
 * handed on by the main thread's next camera (h_vt3_proj_cam) */
/* LINDBERGH_VT3_REPLAY_TYPE=<0..6>|cycle (test): camera::setReplayType's type
 * replaced - one type, or 0, 1, 2 ... 6 in turn, replay after replay */
static void (*g_vt3_set_replay_type)(CPU *);
static void h_vt3_set_replay_type(CPU *c)
{
    static int want = -2, next;
    if (want == -2) { const char *v = getenv("LINDBERGH_VT3_REPLAY_TYPE"); want = !v || !*v ? -1 : !strcmp(v, "cycle") ? 7 : atoi(v); }
    if (want >= 0) {
        extern unsigned g_frame;
        const uint32_t type = want == 7 ? (uint32_t)(next++ % 7) : (uint32_t)want;
        fprintf(stderr, "[vt3] frame %u: replay type %u -> %u (LINDBERGH_VT3_REPLAY_TYPE)\n", g_frame, rd32(c->esp + 4), type);
        wr32(c->esp + 4, type);
    }
    push32(c, 0xDEADBEEFu); g_vt3_set_replay_type(c);
}
static _Atomic unsigned g_vt3_rewinds;
static unsigned g_vt3_rewinds_told;
static void (*g_vt3_reverse)(CPU *);
static void h_vt3_reverse(CPU *c) { atomic_fetch_add(&g_vt3_rewinds, 1u); push32(c, 0xDEADBEEFu); g_vt3_reverse(c); }
static void h_vt3_proj_cam(CPU *c)
{
    push32(c, 0xDEADBEEFu);
    g_vt3_proj_cam(c);
    {   /* LINDBERGH_VT3_CAMMODE_LOG=1: the camera's mode whenever it changes, with the lens */
        static int lg = -1; static uint32_t last = ~0u; extern unsigned g_frame;
        if (lg < 0) { const char *v = getenv("LINDBERGH_VT3_CAMMODE_LOG"); lg = v && *v == '1'; }
        uint32_t m = g_vt3_camera ? rd32(g_vt3_camera + 0x38) : ~1u;
        if (lg && m != last) {
            uint32_t fn = guest_symbol("_ZN14_sSceneManager10getCurrentEv"), sm = fn ? guest_call(c, fn, NULL, 0) : 0;
            uint32_t b = sm ? rd32(sm + 0x5c) : 0; float lens; memcpy(&lens, &b, 4);
            fprintf(stderr, "[cammode] frame %u: camera mode %#x -> %#x (lens %.2f)\n", g_frame, last, m, lens);
            last = m;
        }
    }
    uint32_t fn = guest_symbol("_ZN14_sSceneManager10getCurrentEv");
    uint32_t sm = fn ? guest_call(c, fn, NULL, 0) : 0;
    if (!sm) return;
    float p[16], v[12];
    for (int i = 0; i < 16; i++) { uint32_t b = rd32(sm + 0x5c + 4u * (uint32_t)i); memcpy(&p[i], &b, 4); }
    for (int i = 0; i < 12; i++) { uint32_t b = rd32(sm + 0x2c + 4u * (uint32_t)i); memcpy(&v[i], &b, 4); }
    {
        const unsigned rw = atomic_load(&g_vt3_rewinds);
        const unsigned m = g_vt3_camera ? rd32(g_vt3_camera + 0x38) : ~0u;
        const int cut = rw != g_vt3_rewinds_told && m == 6;
        g_vt3_rewinds_told = rw;
        static int lg = -1;     /* LINDBERGH_VT3_CAMMODE_LOG=2: each replay / close-up frame's camera too */
        if (lg < 0) { const char *e = getenv("LINDBERGH_VT3_CAMMODE_LOG"); lg = e && *e == '2'; }
        if (lg && (m == 6 || m == 7) && g_vt3_camera) {
            extern unsigned g_frame; float fov; uint32_t b = rd32(g_vt3_camera + 0x5c); memcpy(&fov, &b, 4);
            fprintf(stderr, "[cammode] frame %u: mode %u replay type %d shot %d fov %.1f lens %.2f%s\n", g_frame, m,
                    (int)rd32(g_vt3_camera + 0x1f0), (int)rd32(g_vt3_camera + 0x1f4), fov, p[0], cut ? " (rewound)" : "");
        }
        if (p[0] > 0 && p[10] != 1.0f && p[11] / (p[10] - 1.0f) < 10.0f) vt3_far_end_note(m, v);   /* a world camera */
        vt3_note_projection(p, v, 0, m, cut);
    }
    /* a world camera (not the pixel camera, near 800): its mode and lens to the
     * stereo path, in order with the draws, for flat or world at a cut */
    if (p[0] > 0 && p[10] != 1.0f && p[11] / (p[10] - 1.0f) < 10.0f && g_vt3_camera) {
        extern void stereo_vt3_camera_post(unsigned mode, float lens);
        stereo_vt3_camera_post(rd32(g_vt3_camera + 0x38), p[0]);
    }
}
static int vt3_wrap(const char *sym, void (**orig)(CPU *), HleHandler h)
{
    extern void (*dispatch_lifted(uint32_t va))(CPU *);
    uint32_t va = guest_symbol(sym);
    return va && (*orig = dispatch_lifted(va)) && guest_override(sym, h);
}
/* LINDBERGH_VT3_CAMLOG=1: each camera state the game starts, and when */
#define CAM_STATES(X) X(initReady, "_ZN6camera9initReadyEv") \
    X(initMatch, "_ZN6camera9initMatchEv") X(initPlayerintro, "_ZN6camera15initPlayerintroEv") \
    X(initGameRoutine_kawa, "_ZN6camera20initGameRoutine_kawaEv") \
    X(initNormal, "_ZN6camera10initNormalEv") X(initAb, "_ZN6camera6initAbEv") X(initContinue_cam, "_ZN6camera16initContinue_camEi") \
    X(initReplay, "_ZN6camera10initReplayEv") X(initHowtoplay, "_ZN6camera13initHowtoplayEv")
#define CAM_DEF(n, sym) static void (*g_cam_##n)(CPU *); \
    static void h_cam_##n(CPU *c) { extern unsigned g_frame; fprintf(stderr, "[camstate] frame %u: %s(this %#x, %#x)\n", g_frame, #n, rd32(c->esp), rd32(c->esp + 4)); \
                                    push32(c, 0xDEADBEEFu); g_cam_##n(c); }
CAM_STATES(CAM_DEF)
static void vt3_camlog_install(void)
{
    const char *v = getenv("LINDBERGH_VT3_CAMLOG");
    if (!v || *v != '1') return;
#define CAM_INST(n, sym) if (!vt3_wrap(sym, &g_cam_##n, h_cam_##n)) fprintf(stderr, "[camstate] %s not wrapped\n", sym);
    CAM_STATES(CAM_INST)
    fprintf(stderr, "[camstate] logging the camera's states\n");
}
/* LINDBERGH_VT3_FLOWLOG=1: each arcade screen loop the game starts (usrAc...Loop::init), and when */
#define FLOW_STATES(X) X(GameLoop, "_ZN13usrAcGameLoop4initEv") \
    X(LoadLoop, "_ZN13usrAcLoadLoop4initEv") \
    X(PassLoop, "_ZN13usrAcPassLoop4initEv") \
    X(JudgeLoop, "_ZN14usrAcJudgeLoop4initEv") \
    X(StartLoop, "_ZN14usrAcStartLoop4initEv") \
    X(StatsLoop, "_ZN14usrAcStatsLoop4initEv") \
    X(EndingLoop, "_ZN15usrAcEndingLoop4initEv") \
    X(ReplayLoop, "_ZN15usrAcReplayLoop4initEv") \
    X(WinloseLoop, "_ZN16usrAcWinloseLoop4initEv") \
    X(ContinueLoop, "_ZN17usrAcContinueLoop4initEv") \
    X(EntranceLoop, "_ZN17usrAcEntranceLoop4initEv") \
    X(GameinitLoop, "_ZN17usrAcGameinitLoop4initEv") \
    X(GameoverLoop, "_ZN17usrAcGameoverLoop4initEv") \
    X(ChangeendLoop, "_ZN18usrAcChangeendLoop4initEv") \
    X(GamesetmatchLoop, "_ZN21usrAcGamesetmatchLoop4initEv") \
    X(TobecontinuedLoop, "_ZN22usrAcTobecontinuedLoop4initEv")
/* Virtua Tennis 3, each match's start (usrAcEntranceLoop::init): the game's
 * next frame draws the stadium through the menus' camera (from high above,
 * lens 1.45) before the intro's camera is set - not seen on the arcade, a
 * split second of the court from above in the headset. That frame is held
 * (host_xr.c: the eyes show the frame before). LINDBERGH_VT3_HOLD=0 off. */
void (*g_hold_next_frame)(void);      /* host_xr.c sets it (xr_hold_next_frame); none on the desktop */
static int vt3_hold_on(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("LINDBERGH_VT3_HOLD"); on = !(v && *v == '0'); }
    return on;
}
static void vt3_entrance_note(void)
{
    if (vt3_hold_on() && g_hold_next_frame) g_hold_next_frame();
}
static void (*g_vt3_entrance)(CPU *);
static void h_vt3_entrance(CPU *c) { vt3_entrance_note(); push32(c, 0xDEADBEEFu); g_vt3_entrance(c); }
#define FLOW_DEF(n, sym) static void (*g_flow_##n)(CPU *); \
    static void h_flow_##n(CPU *c) { extern unsigned g_frame; fprintf(stderr, "[flow] frame %u: %s\n", g_frame, #n); \
                                     if (!strcmp(#n, "EntranceLoop")) vt3_entrance_note(); \
                                     push32(c, 0xDEADBEEFu); g_flow_##n(c); }
FLOW_STATES(FLOW_DEF)
static int g_vt3_flowlog;
static void vt3_flowlog_install(void)
{
    const char *v = getenv("LINDBERGH_VT3_FLOWLOG");
    if (!v || *v != '1') return;
    g_vt3_flowlog = 1;
#define FLOW_INST(n, sym) if (!vt3_wrap(sym, &g_flow_##n, h_flow_##n)) fprintf(stderr, "[flow] %s not wrapped\n", sym);
    FLOW_STATES(FLOW_INST)
}
/* LINDBERGH_VT3_SKIP_UVA=1 (default in world mode; =0 keeps it): the fly-by intro camera (camera::initUva, at a
 * match's start and between games) not started - the camera stays on its
 * game routine. (Running the fly-by fast instead overran its animation.) */
static void (*g_vt3_init_uva)(CPU *);
static void h_vt3_init_uva(CPU *c)
{
    extern unsigned g_frame;
    uint32_t self = A32(0), mode = self >= 0x08000000u ? rd32(self + 0x38) : 0;
    if (self >= 0x08000000u) g_vt3_camera = self;
    /* camera::reset starts a fly-by (initUva(-1): the animation chosen at
     * +0x228) for camera modes 4 (usrAcEntranceLoop: the match intro), 8
     * (usrAcChangeendLoop: between games), 0xa (usrAcStatsLoop, usrChResultLoop,
     * usrChTotalResultLoop: the result slates), 0xe (usrAcEndingLoop) and 0x10
     * (usrPlayerintroLoop). Only the first two are the intro fly-by this skips.
     * The result slate's exec() ends when camera::isUvaFinished() - no
     * animation at +0x224 counts as finished - so skipping its fly-by ended the
     * slate after ~4 frames (the slate is a flat menu screen in world mode
     * anyway); the others are kept for the same reason. */
    if (mode != 4 && mode != 8) {
        if (g_vt3_log) fprintf(stderr, "[vt3] frame %u: fly-by camera kept (camera mode %#x)\n", g_frame, mode);
        push32(c, 0xDEADBEEFu); g_vt3_init_uva(c); return;
    }
    if (g_vt3_log) fprintf(stderr, "[vt3] frame %u: intro fly-by camera skipped (camera mode %#x)\n", g_frame, mode);
    RET(0);
}
/* LINDBERGH_VT3_CAMFIELDS=<from>-<to>: during those frames, each small integer
 * field of the camera object (and the match manager at 0x85061a4) that changes,
 * logged per frame - finding the start-of-match camera's state */
static int g_rout_on;
static void vt3_rout_mark(int i);
static void (*g_vt3_game_routine)(CPU *);
static void h_vt3_game_routine(CPU *c)
{
    extern unsigned g_frame;
    static long a = -2, b; static uint32_t prev[0x400 / 4]; static uint32_t pself; static int n;
    uint32_t self = A32(0);   /* argument 0 (was read at esp+4: the caller's saved ebx, the same camera by luck - checked) */
    {   /* LINDBERGH_VT3_LOG: which stack slot holds the camera (checking an off-by-one: argument 0 is at esp) */
        static int shown;
        if (g_vt3_log && shown < 3 && g_frame % 600 == 0) {
            shown++;
            fprintf(stderr, "[vt3] frame %u: gameRoutine esp+0 %#x (+8 -> %#x), esp+4 %#x (+8 -> %#x)\n", g_frame,
                    A32(0), A32(0) >= 0x08000000u ? rd32(A32(0) + 8) : 0, A32(1), A32(1) >= 0x08000000u ? rd32(A32(1) + 8) : 0);
        }
    }
    if (a == -2) { const char *v = getenv("LINDBERGH_VT3_CAMFIELDS"); a = v && *v ? atol(v) : -1; b = v && strchr(v, '-') ? atol(strchr(v, '-') + 1) : a; }
    if (g_rout_on) vt3_rout_mark(16);
    if (self >= 0x08000000u && self < 0xC0000000u) g_vt3_camera = self;
    push32(c, 0xDEADBEEFu); g_vt3_game_routine(c);
    {   /* World mode: at a match's start the game draws through the broadcast camera
         * for ~3 s before it sets it through setProjectCameraMatrix (the hook
         * world mode reads it from), and without it world mode guessed the camera
         * from the draws - an overview and a top-down view before the real one.
         * The routine has just written the camera's eye and target into the scene
         * camera (+0x78 eye, +0x84 target): when the game has not set the world
         * camera for a few frames, it is built from those and the broadcast lens.
         * LINDBERGH_VT3_FEEDCAM=0 turns it off. */
        static int feed = -1;
        if (feed < 0) { const char *v = getenv("LINDBERGH_VT3_FEEDCAM");
                        feed = world_mode_immersive() && !(v && *v == '0'); }
        extern volatile unsigned g_game_world_real_frame; extern int g_vt3_feeding; extern unsigned g_frame;
        if (feed && self >= 0x08000000u && self < 0xC0000000u &&
            (g_game_world_real_frame == ~0u || g_frame - g_game_world_real_frame > 3)) {
            uint32_t sc = rd32(self + 8); float e[3], t[3];
            for (int k = 0; k < 3; k++) { uint32_t u = rd32(sc + 0x78 + 4u * k); memcpy(&e[k], &u, 4);
                                          u = rd32(sc + 0x84 + 4u * k); memcpy(&t[k], &u, 4); }
            float f[3] = { t[0] - e[0], t[1] - e[1], t[2] - e[2] };
            float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (fl > 1e-3f && isfinite(fl)) {
                for (int k = 0; k < 3; k++) f[k] /= fl;
                float r[3] = { -f[2], 0, f[0] };                    /* f x (0,1,0) */
                float rl = sqrtf(r[0] * r[0] + r[2] * r[2]);
                if (rl > 1e-3f) {
                    r[0] /= rl; r[2] /= rl;
                    float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
                    float view[12] = { r[0], r[1], r[2], 0, u[0], u[1], u[2], 0, -f[0], -f[1], -f[2], 0 };
                    for (int row = 0; row < 3; row++)
                        view[row * 4 + 3] = -(view[row * 4] * e[0] + view[row * 4 + 1] * e[1] + view[row * 4 + 2] * e[2]);
                    static const float proj[16] = { 3.20261f, 0, 0, 0, 0, 5.67128f, 0, 0, 0, 0, -1.00067f, -2.00067f, 0, 0, -1, 0 };
                    static unsigned told;
                    if (g_vt3_log && g_frame - told > 60) { told = g_frame; fprintf(stderr, "[vt3] frame %u: world camera built from the camera routine (eye %.0f %.0f %.0f)\n", g_frame, e[0], e[1], e[2]); }
                    vt3_note_projection(proj, view, 1, ~0u, 0);
                }
            }
        }
    }
    if (a < 0 || (long)g_frame < a || (long)g_frame > b) return;
    if (g_frame % 10 == 0) {    /* what the routine wrote into the scene camera, and the view drawn with */
        extern volatile float g_game_view[12];
        uint32_t sc = rd32(self + 8); float w[6];
        for (int i = 0; i < 6; i++) { uint32_t u = rd32(sc + 0x78 + 4u * i); memcpy(&w[i], &u, 4); }
        const volatile float *V = g_game_view;
        float e[3]; for (int k = 0; k < 3; k++) e[k] = -(V[k] * V[3] + V[4 + k] * V[7] + V[8 + k] * V[11]);
        fprintf(stderr, "[camview] frame %u scam %#x wrote %.1f %.1f %.1f | %.1f %.1f %.1f ; drawn eye %.1f %.1f %.1f\n",
                g_frame, sc, w[0], w[1], w[2], w[3], w[4], w[5], e[0], e[1], e[2]);
    }
    {   /* the global block gameRoutine starts from (0x850e200..0x850e400), as floats */
        static uint32_t gprev[0x200 / 4]; static int ginit, gn;
        for (int i = 0; i < 0x200 / 4; i++) {
            uint32_t v = rd32(0x0850e200u + 4u * i);
            if (ginit && v != gprev[i] && gn < 4000) {
                float f0, f1; memcpy(&f0, &gprev[i], 4); memcpy(&f1, &v, 4);
                fprintf(stderr, "[camglb] frame %u 0x%08x %g -> %g (%#x)\n", g_frame, 0x0850e200u + 4u * i, f0, f1, v); gn++;
            }
            gprev[i] = v;
        }
        ginit = 1;
    }
    if (self != pself) { for (int i = 0; i < 0x400 / 4; i++) prev[i] = rd32(self + 4u * i); pself = self;
                         fprintf(stderr, "[camfld] frame %u camera %#x\n", g_frame, self); return; }
    for (int i = 0; i < 0x400 / 4 && n < 60000; i++) {
        uint32_t v = rd32(self + 4u * i);
        static int all = -1; if (all < 0) { const char *e = getenv("LINDBERGH_VT3_CAMFIELDS_ALL"); all = e && *e == '1'; }
        if (v != prev[i] && (all || ((v < 0x10000 || prev[i] < 0x10000) && v < 0x10000000 && prev[i] < 0x10000000))) {
            fprintf(stderr, "[camfld] frame %u +%#x %u -> %u\n", g_frame, 4 * i, prev[i], v); n++;
        }
        prev[i] = v;
    }
}
/* LINDBERGH_VT3_CAMROUTINES=1: which of the camera's per-frame routines run,
 * logged whenever the set changes from one frame to the next */
#define CAM_ROUT(X) X(0, procMain, "_ZN6camera8procMainEv") X(1, gameRoutine_kawa, "_ZN6camera16gameRoutine_kawaEv") \
    X(2, normal, "_ZN6camera6normalEv") X(3, match, "_ZN6camera5matchEv") X(4, ready, "_ZN6camera5readyEv") \
    X(5, ab, "_ZN6camera2abEv") X(6, uva, "_ZN6camera3uvaEv") X(7, playerintro, "_ZN6camera11playerintroEv") \
    X(8, continue_cam, "_ZN6camera12continue_camEv") X(9, replay, "_ZN6camera6replayEv") X(10, winlose, "_ZN6camera7winloseEv") \
    X(11, howtoplay, "_ZN6camera9howtoplayEv") X(12, ending, "_ZN6camera6endingEv") X(13, setLook_down, "_ZN6camera12setLook_downEv") \
    X(14, calcParameter, "_ZN6camera13calcParameterEv") X(15, continue_init, "_ZN6camera16initContinue_camEi")
static const char *const g_rout_name[] = {
#define CAM_RN(i, n, sym) #n,
    CAM_ROUT(CAM_RN) };
static unsigned g_rout_mask, g_rout_frame = ~0u, g_rout_prev = ~0u;
static void vt3_rout_mark(int i)   /* also called by the gameRoutine hook (bit 16) */
{
    extern unsigned g_frame;
    if (g_frame != g_rout_frame) {
        if (g_rout_frame != ~0u && g_rout_mask != g_rout_prev) {
            fprintf(stderr, "[camrout] frame %u:", g_rout_frame);
            for (int k = 0; k < 17; k++) if (g_rout_mask >> k & 1) fprintf(stderr, " %s", k == 16 ? "gameRoutine" : g_rout_name[k]);
            fprintf(stderr, "\n");
            g_rout_prev = g_rout_mask;
        }
        g_rout_frame = g_frame; g_rout_mask = 0;
    }
    g_rout_mask |= 1u << i;
}
#define CAM_RD(i, n, sym) static void (*g_rt_##n)(CPU *); static void h_rt_##n(CPU *c) { vt3_rout_mark(i); push32(c, 0xDEADBEEFu); g_rt_##n(c); }
CAM_ROUT(CAM_RD)
static void vt3_routines_install(void)
{
    const char *v = getenv("LINDBERGH_VT3_CAMROUTINES");
    if (!v || *v != '1') return;
    g_rout_on = 1;
#define CAM_RI(i, n, sym) if (i != 0 && i != 5 && i != 13 && i != 14 && i != 15 && !vt3_wrap(sym, &g_rt_##n, h_rt_##n)) fprintf(stderr, "[camrout] %s not wrapped\n", sym);
    CAM_ROUT(CAM_RI)
}
/* LINDBERGH_VT3_ATTACHLOG=1: each camera the scene manager is given, the frame and the caller */
static void (*g_vt3_attach_cam)(CPU *);
static void h_vt3_attach_cam(CPU *c)
{
    extern unsigned g_frame;
    fprintf(stderr, "[attach] frame %u: scene manager %#x camera %#x (from %#x)\n", g_frame, rd32(c->esp), rd32(c->esp + 4), rd32(c->esp - 4));   /* the return address is popped, still below esp */
    push32(c, 0xDEADBEEFu); g_vt3_attach_cam(c);
}
int g_replays = 1, g_replay_hook;
static void (*g_vt3_is_replay)(CPU *);
static void h_vt3_is_replay(CPU *c)
{
    extern unsigned g_frame; static unsigned told = ~0u;     /* asked every frame at a point's end: told once */
    uint32_t m = rd32(rd32(0x085061A4u) + 0x1c);             /* the match: +0xd6 match over (match::isMatch), +0x1c events */
    if (g_vt3_log && g_frame - told > 120)
        fprintf(stderr, "[vt3] frame %u: point over (match over %u, events %#x)%s\n", g_frame, rd8(m + 0xd6), rd32(m + 0x1c),
                g_replays ? "" : ": point replay skipped (Replays off)");
    if (!g_replays) { told = g_frame; RET(0); return; }
    push32(c, 0xDEADBEEFu); g_vt3_is_replay(c);
    if (g_vt3_log && c->eax && g_frame - told > 120) fprintf(stderr, "[vt3] point replayed\n");
    told = g_frame;
}
/* The match's end with Replays off. Between points usrAcPassLoop::exec picks
 * the next screen: a replay (mtrLoopManager mode 7) when isReplay_point says
 * so, else straight to the next point (mode 8). The match's end is decided
 * only where a replay finishes: usrAcReplayLoop::exec sees the match event
 * (match::setEvent(12): bit 0x1000 of the match's +0x1c, and +0xd6 =
 * match::isMatch), clears it and sets mode 0xa (game set and match) for the
 * winner or 0xb for the loser. isReplay_point answers yes whenever the match
 * is over, so with replays on the closing replay always runs and the match
 * ends; the switch answering no sent the last point to mode 8, and the match
 * went on into new games. So: when the pass loop sets mode 8 while the match
 * event is up, do what the replay loop's end does instead - the closing
 * replay is skipped and the match ends. Arcade path only (the replay loop's
 * own checks for the attract demo and challenge mode pick other modes). */
static void (*g_vt3_set_mode)(CPU *);
static void h_vt3_set_mode(CPU *c)
{
    extern unsigned g_frame;
    uint32_t from = rd32(c->esp - 4u);                      /* the caller's return address (popped, still there) */
    uint32_t self = A32(0), mode = A32(1), mgr = rd32(0x085061A4u);
    uint32_t m = mgr ? rd32(mgr + 0x1c) : 0, loser = 0;
    /* The one way into a replay: usrAcPassLoop::exec's setMode at return
     * 0x08086B17, taken only when replay::isReplay_point says yes - mode 7
     * on the arcade manager ([mgr+0x104]), 6 on the attract demo's, 9 on
     * challenge mode's ([mgr+0x108]). With Replays off the isReplay_point
     * wrap answers no, so this must not happen: logged, not redirected. */
    if (!g_replays && from == 0x08086B17u && (mode == 6 || mode == 7 || mode == 9))
        fprintf(stderr, "[vt3] frame %u: replay requested with Replays off (setMode(%#x, %u) from %#x) - isReplay_point not answered?\n",
                g_frame, self, mode, from);
    if (!g_replays && from == 0x08086C6Du && mode == 8 && m && (rd32(m + 0x1c) & 0x1000u) &&
        mgr && self == rd32(rd32(mgr + 0x104) + 0x14)) {
        wr32(m + 0x1c, rd32(m + 0x1c) & ~0x1000u);
        uint32_t kind = rd32(mgr + 0x24);
        if (kind == 0 || kind == 2) {                       /* as usrAcReplayLoop::exec: winner or loser */
            uint32_t gm = guest_symbol("_ZN2vs19getGame_master_plnoEv"), iw = guest_symbol("_ZN5match8isWinnerEl");
            uint32_t a[2] = { rd32(mgr + 0x94), 0 };
            if (gm && iw) { a[1] = guest_call(c, gm, a, 1); a[0] = m; loser = !(guest_call(c, iw, a, 2) & 0xff); }
        }
        mode = loser ? 0xb : 0xa;
        wr32(c->esp + 4u, mode);
        fprintf(stderr, "[vt3] frame %u: match over with Replays off - closing replay skipped, %s\n", g_frame,
                loser ? "lost (mode 0xb)" : "game set and match (mode 0xa)");
    }
    /* Point cutaways off: no win/lose close-up at all. After a point the pass
     * loop (return 0x08086C6D) - or a point's replay ending (0x0808A6A7) -
     * picks mode 8, usrAcWinloseLoop: camera 7 (the winner's close-up),
     * which even skipped at once ran ~17 frames with two camera cuts. Instead
     * the choice its exec() makes at its end, in the arcade path: change of
     * ends (9) on a tie-break change or the match's change-ends event (bit
     * 0x400 of match+0x1c, cleared as it does), else the next point (3).
     * What its init and exit do besides is for the close-up (camera mode,
     * players' celebration poses for the camera, the centre "POINT" plate,
     * the crowd's cheer card animation, glare) and is not needed; the score
     * box has the new score. Left to the game: a card being written, a
     * versus game asked for (vs::isReq_vs_game), the attract demo
     * (checkMode 1), challenge mode (checkMode 4: never mode 8 from these),
     * a match already decided (0xa/0xb above). */
    extern int g_cutaways;
    if (!g_cutaways && mode == 8 && (from == 0x08086C6Du || from == 0x0808A6A7u) && mgr && m &&
        self == rd32(rd32(mgr + 0x104) + 0x14) && !(rd32(m + 0x1c) & 0x1000u)) {
        uint32_t cw = guest_symbol("_ZN13playerManager14isCard_writingEv"), ck = guest_symbol("_ZN14mtrLoopManager9checkModeEl");
        uint32_t tb = guest_symbol("_ZN5match20isTiebreak_changeendEv");
        uint32_t a[2] = { rd32(mgr + 0x68), 0 };
        int writing = cw ? (guest_call(c, cw, a, 1) & 0xff) != 0 : 1;
        a[0] = rd32(mgr + 0xf4); a[1] = 1;
        int demo = ck ? (guest_call(c, ck, a, 2) & 0xff) != 0 : 1;
        uint32_t rv = guest_symbol("_ZN2vs13isReq_vs_gameEv");     /* exec() also waits while a versus game is asked for */
        a[0] = rd32(mgr + 0x94);
        int versus = rv ? (guest_call(c, rv, a, 1) & 0xff) != 0 : 1;
        if (!writing && !demo && !versus && tb) {
            a[0] = m;
            int change = (guest_call(c, tb, a, 1) & 0xff) != 0;
            if (!change && (rd32(m + 0x1c) & 0x400u)) { wr32(m + 0x1c, rd32(m + 0x1c) & ~0x400u); change = 1; }
            mode = change ? 9 : 3;
            wr32(c->esp + 4u, mode);
            if (g_vt3_flowlog) fprintf(stderr, "[flow] frame %u: point cutaway left out (Point cutaways off) - straight to %s\n", g_frame,
                                       change ? "change of ends (mode 9)" : "the next point (mode 3)");
        }
    }
    if (mgr && self == rd32(rd32(mgr + 0x104) + 0x14)) g_vt3_loop = mode;   /* the arcade loops' (far-end height latch) */
    if (g_vt3_flowlog && from >= 0x0807E000u && from < 0x0808F000u)    /* the arcade screen loops' choices */
        fprintf(stderr, "[flow] frame %u: setMode(%#x, %u) from %#x\n", g_frame, self, mode, from);
    push32(c, 0xDEADBEEFu); g_vt3_set_mode(c);
    if (loser) {
        uint32_t ss = guest_symbol("_ZN5sound12stopSoundAllEv"), a[1] = { rd32(mgr + 0x88) };
        if (ss) guest_call(c, ss, a, 1);
    }
}
/* Point cutaways: after each decided point usrAcWinloseLoop (mode 8) shows the
 * winner close up for 120 frames (camera::winlose; 1 in 10 split screen).
 * Its exec() ends early once playerManager::check_skip_button_winner() says
 * a player pressed a shot button and more than playerManager::getCnt_skip_max()
 * frames (+0x78, 15 since the constructor) have passed - then it takes the
 * same exit as at frame 120 (next point, change of ends, game over...). With
 * g_cutaways 0 the call from the win/lose loop (returns to 0x0808E6CB) is
 * answered yes, so the close-up lasts ~16 frames; the other loops that ask
 * (start, game set and match, replay) are untouched. g_cutaways comes from
 * the mode settings (world_mode.c); LINDBERGH_VT3_CUTAWAYS=0|1 for a run. */
int g_cutaways = 1, g_cutaway_hook;
static void (*g_vt3_skip_winner)(CPU *);
static void h_vt3_skip_winner(CPU *c)
{
    uint32_t from = rd32(c->esp - 4u);                      /* the caller's return address (popped, still there) */
    if (!g_cutaways && from == 0x0808E6CBu) {
        extern unsigned g_frame; static unsigned told = ~0u;
        if (g_vt3_flowlog && g_frame - told > 30) {
            uint32_t mgr = rd32(0x085061A4u), pm = mgr ? rd32(mgr + 0x68) : 0;
            fprintf(stderr, "[flow] frame %u: point cutaway skipped (Point cutaways off, after %d frames)\n",
                    g_frame, pm ? (int)rd32(pm + 0x78) + 1 : -1);
        }
        told = g_frame;
        RET(1); return;
    }
    /* Replays switched off while one plays (the panel applies at once): the
     * replay loop (usrAcReplayLoop::exec, return 0x0808A850) asks the same
     * question each frame of a replay; answered yes, it ends as the game's
     * own skip does - once past getCnt_skip_max() frames, still held while a
     * card is written (isCard_writing / cardManager::isFinishedCue) - and
     * takes its usual exit (next point, change of ends, game set and match). */
    if (!g_replays && from == 0x0808A850u) {
        extern unsigned g_frame; static unsigned told = ~0u;
        if (g_frame - told > 30)
            fprintf(stderr, "[vt3] frame %u: replay playing with Replays off - skipped\n", g_frame);
        told = g_frame;
        RET(1); return;
    }
    push32(c, 0xDEADBEEFu); g_vt3_skip_winner(c);
}
/* Point cutaways off: no big-point close-up of the server either. On a
 * "heat-up" point (game, break, set or match point) usrAcStartLoop::init
 * puts the camera on the server (camera mode 5, ~150 frames, heat-up poses
 * and, in tournaments, the heat-up music) instead of the court camera (mode
 * 1); in Immersive that was a cut to a flat close-up and back before the
 * serve. With the switch off its questions (returns 0x080856D3 and
 * 0x08085775, and exec's 0x08085AB2) are answered "no": the point starts as
 * any other, from the court camera (restartPlusAb: the score boards shown). */
static void (*g_vt3_is_heatup)(CPU *);
static void h_vt3_is_heatup(CPU *c)
{
    uint32_t from = rd32(c->esp - 4u);
    push32(c, 0xDEADBEEFu); g_vt3_is_heatup(c);
    /* ...nor with the cutaways in the world (it adds nothing from your seat) */
    if ((!g_cutaways || vt3_cut_in_world()) && (c->eax & 0xff) && (from == 0x080856D3u || from == 0x08085775u || from == 0x08085AB2u)) {
        extern unsigned g_frame; static unsigned told = ~0u;
        if (g_vt3_flowlog && g_frame - told > 30) fprintf(stderr, "[flow] frame %u: big-point serve close-up left out (Point cutaways %s)\n", g_frame,
                                                          g_cutaways ? "in the world" : "off");
        told = g_frame;
        c->eax &= ~0xffu;
    }
}
/* Virtua Tennis 3's HUD items placed at a 3D point in world mode
 * (LINDBERGH_VT3_IMMERSIVE=1). hud::getScreenPosFrom3DPos(const _sVector3d&)
 * projects a point through the game camera to the HUD's units (+-480 x
 * +-360, x 4/3 when tennis::bWide_); the serve meter (hud::drawServePower,
 * once per serve) and the 1P/2P markers (hud::setPlayerIcon, every frame)
 * are drawn there through the pixel camera. Around the head that plane is
 * seen from elsewhere, so the position is worked out again for the eyes
 * (stereo_vt3_hud_anchor) - LINDBERGH_VT3_HUD_ANCHOR=0 turns it off.
 * The function returns its _sVector4d through a hidden pointer (argument 0,
 * popped by it: retl $4); this = argument 1, the point = argument 2.
 * LINDBERGH_VT3_SERVELOG=1 logs each answer and, every frame a serve meter
 * shows, the server's state and power. */
#define VT3_HUD_G 0x0850e280u               /* the HUD's matrix into the pixel camera's world (3x4) */
static void (*g_vt3_screen_pos)(CPU *), (*g_vt3_serve_power)(CPU *);
static int g_vt3_hud_anchor, g_vt3_servelog;
static struct { uint32_t pt; float o[2], n[2]; int ok; unsigned frame; } g_vt3_anchor_last[4];
static float vt3_f(uint32_t a) { uint32_t u = rd32(a); float f; memcpy(&f, &u, 4); return f; }
static void vt3_wf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(a, u); }
static void h_vt3_screen_pos(CPU *c)
{
    extern unsigned g_frame;
    extern int stereo_vt3_hud_anchor(const float *wP, const float *wV, const float *G, const float pt[3], float hud[2], float *eyes);
    uint32_t ret = A32(0), ptp = A32(2), from = rd32(c->esp - 4u);
    push32(c, 0xDEADBEEFu); g_vt3_screen_pos(c);   /* pops its hidden pointer as the game's own call would */
    float o[2] = { vt3_f(ret), vt3_f(ret + 4) }, n[2] = { o[0], o[1] }, eyes[4] = { 0 };
    float pt[3] = { vt3_f(ptp), vt3_f(ptp + 4), vt3_f(ptp + 8) };
    int ok = 0;
    /* the game camera it projected through: [g_pTennis + 0x38], virtual +8:
     * projection 4x4 at +0x34, view 3x4 at +4 (as the function reads them) */
    uint32_t t = rd32(VT3_G_PTENNIS), e = t ? rd32(t + 0x38) : 0, cam = 0;
    if (e) { uint32_t a[1] = { e }; cam = guest_call(c, rd32(rd32(e) + 8), a, 1); }
    float wP[16], wV[12], G[12];
    if (cam) {
        for (int i = 0; i < 16; i++) wP[i] = vt3_f(cam + 0x34 + 4u * i);
        for (int i = 0; i < 12; i++) wV[i] = vt3_f(cam + 4 + 4u * i);
        for (int i = 0; i < 12; i++) G[i] = vt3_f(VT3_HUD_G + 4u * i);
        if (g_vt3_hud_anchor && stereo_vt3_hud_anchor(wP, wV, G, pt, n, eyes)) {
            ok = 1;
            vt3_wf(ret, n[0]); vt3_wf(ret + 4, n[1]);
        }
    }
    c->eax = ret;
    if (g_vt3_servelog) {
        static unsigned told;
        int slot = -1;                              /* the players' own points, for the per-frame log */
        uint32_t np = t ? rd32(t) : 0, pl = t ? rd32(t + 4) : 0;
        for (uint32_t i = 0; i < np && i < 4; i++) if (rd32(pl + 4 * i) + 0x18 == ptp) slot = (int)i;
        if (slot >= 0) { g_vt3_anchor_last[slot].pt = ptp; g_vt3_anchor_last[slot].ok = ok; g_vt3_anchor_last[slot].frame = g_frame;
                         memcpy(g_vt3_anchor_last[slot].o, o, sizeof o); memcpy(g_vt3_anchor_last[slot].n, n, sizeof n); }
        if (from == 0x0810f4f6u || g_frame - told >= 30) {   /* the serve meter's every call, the rest twice a second */
            told = g_frame;
            fprintf(stderr, "[servelog] frame %u: screen pos from %#x, player %d point %.1f %.1f %.1f: game %.1f %.1f -> %s %.1f %.1f (eyes %.1f %.1f | %.1f %.1f) G %.4f %.4f %.4f %.1f %.1f %.1f\n",
                    g_frame, from, slot, pt[0], pt[1], pt[2], o[0], o[1], ok ? "anchored" : "kept", n[0], n[1],
                    eyes[0], eyes[1], eyes[2], eyes[3], cam ? G[0] : 0, cam ? G[5] : 0, cam ? G[10] : 0, cam ? G[3] : 0, cam ? G[7] : 0, cam ? G[11] : 0);
        }
    }
}
/* LINDBERGH_VT3_SERVELOG=1: every frame a serve meter shows (a player's
 * +0x265 serve state not 0 or 4, +0x258 not 0xf, as hud::drawServePower
 * tests): the state, the power (+0x244, 1 = full), whether the meter's place
 * is latched (hud +0x60) and the last screen position worked out for it */
/* LINDBERGH_VT3_SERVESHOT=<n>[,<from frame>]: for the first n serves (from that frame), a full-size game shot
 * (host_xr.c) as the power passes 0.3, 0.5, 0.7, 0.85, 0.9, 0.95, then every
 * 3rd frame for 45 frames from full (state 2) and the swing (state 3) */
volatile int g_game_shot_req;
static void vt3_serve_shot(uint8_t st, float pw)
{
    extern unsigned g_frame;
    static int n = -2, serves, prev_st, step, after; static long from;
    if (n == -2) { const char *v = getenv("LINDBERGH_VT3_SERVESHOT"); n = v && *v ? atoi(v) : 0;
                   from = v && strchr(v, ',') ? atol(strchr(v, ',') + 1) : 0; }
    if (n <= 0 || (long)g_frame < from) return;
    static const float at[] = { 0.3f, 0.5f, 0.7f, 0.85f, 0.9f, 0.95f, 2.0f };
    if (st == 1 && prev_st != 1) { serves++; step = 0; after = 0; }
    prev_st = st;
    if (serves < 1 || serves > n) return;
    if (st == 1) { if (pw >= at[step]) { while (pw >= at[step]) step++; g_game_shot_req = 2; } return; }
    if (after < 45) { if (after % 3 == 0) g_game_shot_req = 2; after++; }
}
static void h_vt3_serve_power(CPU *c)
{
    extern unsigned g_frame;
    uint32_t self = A32(0), latched = rd8(self + 0x60);
    push32(c, 0xDEADBEEFu); g_vt3_serve_power(c);
    {   uint32_t t = rd32(VT3_G_PTENNIS), p0 = t && rd32(t) ? rd32(rd32(t + 4)) : 0;   /* 1P's serves */
        if (p0) vt3_serve_shot(rd8(p0 + 0x265), vt3_f(p0 + 0x244)); }
    uint32_t t = rd32(VT3_G_PTENNIS), np = t ? rd32(t) : 0, pl = t ? rd32(t + 4) : 0;
    for (uint32_t i = 0; i < np && i < 4; i++) {
        uint32_t p = rd32(pl + 4 * i);
        uint8_t st = rd8(p + 0x265);
        if (!p || st == 0 || st == 4 || rd32(p + 0x258) == 0xf) continue;
        fprintf(stderr, "[servelog] frame %u: meter player %u state %u power %.3f latched %u max-flag %u, pos game %.1f %.1f -> %s %.1f %.1f (worked out at frame %u), player at %.1f %.1f %.1f\n",
                g_frame, i, st, vt3_f(p + 0x244), latched, rd8(0x08500840u),
                g_vt3_anchor_last[i].o[0], g_vt3_anchor_last[i].o[1], g_vt3_anchor_last[i].ok ? "anchored" : "kept",
                g_vt3_anchor_last[i].n[0], g_vt3_anchor_last[i].n[1], g_vt3_anchor_last[i].frame,
                vt3_f(p + 0x18), vt3_f(p + 0x1c), vt3_f(p + 0x20));
    }
}
static void vt3_hud_anchor_install(void)
{
    const char *v = getenv("LINDBERGH_VT3_HUD_ANCHOR"), *l = getenv("LINDBERGH_VT3_SERVELOG");
    g_vt3_servelog = l && *l == '1';
    g_vt3_hud_anchor = world_mode_immersive() && !(v && *v == '0');
    if ((g_vt3_hud_anchor || g_vt3_servelog) &&
        vt3_wrap("_ZN3hud21getScreenPosFrom3DPosERKN8_sSerial10_sVector3dE", &g_vt3_screen_pos, h_vt3_screen_pos))
        fprintf(stderr, "[sega] Virtua Tennis 3: HUD items at 3D points %s\n", g_vt3_hud_anchor ? "placed for the eyes (world mode)" : "as the game has them (logged)");
    if (g_vt3_servelog && vt3_wrap("_ZN3hud14drawServePowerEv", &g_vt3_serve_power, h_vt3_serve_power))
        fprintf(stderr, "[sega] Virtua Tennis 3: serve meter logged (LINDBERGH_VT3_SERVELOG)\n");
}
/* World mode's scene culling. _sCamera::isVisible(const _sVector4d *sphere)
 * tests a bounding sphere (world x, y, z, radius) against the camera's six
 * planes (this+0xb8, 16 bytes each, inward: a x + b y + c z + d <= -r is out).
 * The headset sees far more than the broadcast camera, so world mode first
 * made every node visible - the whole stadium drawn every frame. Now, for the
 * game's world camera, the sphere is tested against the eyes' own frusta
 * widened by LINDBERGH_VT3_CULL_MARGIN degrees a side (default 20;
 * stereo_vt3_cull_planes); any other camera (the shadow map's light) keeps
 * the game's test. The world camera is the one with a plane facing the game
 * camera's forward (its near plane). LINDBERGH_VT3_CULL=0 everything visible
 * (as before), =1 the game's own culling, unset/2 this. LINDBERGH_VT3_CULLLOG=1
 * logs the calls and counts. Measured (2026-11-11): the only caller,
 * _sTree::addScene, tests only trees with their culling flag (+0x40 bit 0),
 * from uva::SceneObject::addScene - in a whole arcade run (menus, match,
 * result slate) it was never called, and frame maps show the same 283 draws
 * with culling off, the game's or this. World mode's frame rate was the sky
 * (stereo.c sky_draw), not the culling. */
static void (*g_vt3_is_visible)(CPU *);
static float g_vt3_cull_margin = 20;
static int g_vt3_cull_log;
static void h_vt3_is_visible(CPU *c)
{
    extern unsigned g_frame;
    extern int stereo_vt3_cull_planes(float planes[2][4][4], float fwd[3], float margin);
    static unsigned pf = ~0u; static int ok; static float pl[2][4][4], fwd[3];
    static uint32_t last_cam; static unsigned last_cam_frame = ~0u; static int last_world;
    static unsigned n_call, n_world, n_out, n_game_out, n_other;
    uint32_t self = A32(0), sp = A32(1);
    if (g_frame != pf) {
        pf = g_frame; ok = stereo_vt3_cull_planes(pl, fwd, g_vt3_cull_margin);
        if (g_vt3_cull_log && g_frame % 300 == 0 && n_call) {
            fprintf(stderr, "[cull] frame %u: per frame %.0f tests, %.0f by the world camera (%.0f out, the game's own test %.0f out), %.0f by others\n",
                    g_frame, n_call / 300.0, n_world / 300.0, n_out / 300.0, n_game_out / 300.0, n_other / 300.0);
            n_call = n_world = n_out = n_game_out = n_other = 0;
        }
    }
    n_call++;
    if (g_vt3_cull_log) { static unsigned told = ~0u; if (told == ~0u || g_frame - told > 600) { told = g_frame;
        fprintf(stderr, "[cull] frame %u: isVisible(camera %#x) called (world camera known %d)\n", g_frame, self, ok); } }
    if (!ok) { RET(1); return; }                            /* no world camera yet: everything, as before */
    if (self != last_cam || g_frame != last_cam_frame) {    /* is this the world camera? */
        last_cam = self; last_cam_frame = g_frame; last_world = 0;
        for (int i = 0; i < 6; i++) {
            float n[3] = { vt3_f(self + 0xb8 + 16u * i), vt3_f(self + 0xbc + 16u * i), vt3_f(self + 0xc0 + 16u * i) };
            float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l > 1e-6f && (n[0] * fwd[0] + n[1] * fwd[1] + n[2] * fwd[2]) / l > 0.999f) { last_world = 1; break; }
        }
    }
    if (!last_world) { n_other++; push32(c, 0xDEADBEEFu); g_vt3_is_visible(c); return; }
    n_world++;
    float s[4] = { vt3_f(sp), vt3_f(sp + 4), vt3_f(sp + 8), vt3_f(sp + 12) };
    int vis = 0;
    for (int e = 0; e < 2 && !vis; e++) {
        int in = 1;
        for (int k = 0; k < 4 && in; k++)
            if (pl[e][k][0] * s[0] + pl[e][k][1] * s[1] + pl[e][k][2] * s[2] + pl[e][k][3] <= -s[3]) in = 0;
        vis = in;
    }
    if (!vis) n_out++;
    if (g_vt3_cull_log) {                                   /* the game's own answer, for the counts */
        int gin = 1;
        for (int i = 0; i < 6 && gin; i++)
            if (vt3_f(self + 0xb8 + 16u * i) * s[0] + vt3_f(self + 0xbc + 16u * i) * s[1] + vt3_f(self + 0xc0 + 16u * i) * s[2] +
                vt3_f(self + 0xc4 + 16u * i) <= -s[3]) gin = 0;
        if (!gin) n_game_out++;
    }
    RET(vis);
}
/* Virtua Tennis 3, Immersive: Sega's whole stadium drawn. field::createTree
 * (0x080FFF8E) gives each tree of the stadium model a type byte in
 * [field+0x3c][i]: GAME_SOUTH 1, GAME_NORTH 2, GAME_NO 3 (the upper tiers,
 * roof, walls, upper scoreboards, the sky dome and the sun), else 0.
 * field::draw (0x080FFB86) leaves out type 3 and the end stand behind the
 * camera while the camera is the court camera (camera mode 1 or 2: the
 * broadcast camera never frames them); every other camera draws all. The
 * crowd cards ignore that rule, so in the headset they floated on seats that
 * were not drawn. Here the type bytes are zeroed (0 = drawn by every camera)
 * for the length of the call and put back after - per call, so Classic, or a
 * later switch of the mode, gets the game's own rule.
 * LINDBERGH_VT3_STANDS=0 keeps the game's rule in Immersive too. */
static void (*g_vt3_field_draw)(CPU *);
static int g_vt3_stands = 1;
static void h_vt3_field_draw(CPU *c)
{
    uint32_t self = A32(0);
    if (!g_vt3_stands || !world_mode_immersive() || self < 0x08000000u) { push32(c, 0xDEADBEEFu); g_vt3_field_draw(c); return; }
    uint32_t types = rd32(self + 0x3c), db = rd32(self + 0x2c);
    uint32_t n = db ? guest_call(c, 0x081A7718u, &db, 1) : 0;   /* the scene database's tree count, as field::draw counts */
    uint8_t save[64];
    if (n > 64) n = 64;
    if (types < 0x08000000u) n = 0;
    for (uint32_t i = 0; i < n; i++) { save[i] = rd8(types + i); wr8(types + i, 0); }
    {   static int told; extern unsigned g_frame;
        if (!told++) fprintf(stderr, "[vt3] frame %u: the whole stadium drawn (%u trees; LINDBERGH_VT3_STANDS=0: the court camera's own cut)\n", g_frame, n); }
    push32(c, 0xDEADBEEFu); g_vt3_field_draw(c);
    for (uint32_t i = 0; i < n; i++) wr8(types + i, save[i]);
}
/* the game can draw its match as a world (Virtua Tennis 3's camera symbols
 * resolve): the headset panel's Mode row and Advanced pages (host_xr.c) */
int g_world_capable;
static void vt3_jumps(void)
{
    /* Classic or Immersive, and the values that go with the mode (env >
     * vr.cfg > the mode's default), before any hook below is chosen by it */
    g_world_capable = guest_symbol("_ZN8_sRender22setProjectCameraMatrixEv") && guest_symbol("_ZN6camera11gameRoutineEv");
    world_mode_init(g_world_capable);
    g_replays = world_mode_replays();
    g_arcade_overlays = world_mode_overlays();
    g_cutaways = world_mode_cutaways();
    const char *r = getenv("LINDBERGH_VT3_ROUND"), *p = getenv("LINDBERGH_VT3_POINTS"), *l = getenv("LINDBERGH_VT3_LOG");
    g_vt3_log = l && *l == '1';
    if (r && *r) g_vt3_round = atoi(r);
    if (p && *p && sscanf(p, "%d,%d", &g_vt3_pts[0], &g_vt3_pts[1]) != 2) g_vt3_pts[0] = g_vt3_pts[1] = -1;
    if ((g_vt3_round >= 0 || g_vt3_log) && vt3_wrap("_ZN5match11setMatchingEv", &g_vt3_set_matching, h_vt3_set_matching))
        fprintf(stderr, "[sega] Virtua Tennis 3: %s%d\n", g_vt3_round >= 0 ? "the run starts at round " : "rounds logged ", g_vt3_round);
    if (g_vt3_log) vt3_wrap("_ZN5match13setNext_roundEv", &g_vt3_next_round, h_vt3_next_round);
    {
        const char *only = getenv("LINDBERGH_VT3_OVERLAY_HOOK");   /* test: "credit" or "card" alone */
        int nh = 0;
        if (!only || !strcmp(only, "credit")) nh += vt3_wrap("_ZN6credit4drawEv", &g_vt3_credit_draw, h_vt3_credit_draw);
        if (!only || !strcmp(only, "card")) {
            nh += vt3_wrap("_ZN22CCardSlotWindowManager11window_openEl", &g_vt3_slot_open, h_vt3_slot_open);
            nh += vt3_wrap("_ZN22CCardSlotWindowManager11window_slotElb", &g_vt3_slot, h_vt3_slot);
            nh += vt3_wrap("_ZN22CCardSlotWindowManager19window_slot_usecardElb", &g_vt3_slot_card, h_vt3_slot_card);
            nh += vt3_wrap("_ZN12ab_card_info4initEv", &g_vt3_card_init, h_vt3_card_init);
        }
        if (nh) { g_arcade_overlay_hooks = 1; fprintf(stderr, "[sega] Virtua Tennis 3: arcade overlays %s (%d draws hooked)\n", g_arcade_overlays ? "shown" : "hidden", nh); }
        const char *pt = getenv("LINDBERGH_VT3_PRIZE_TRACE");
        if (pt && *pt) {
            extern long g_arb_trace_from;
            g_vt3_prize_trace = atol(pt);
            g_arb_trace_from = 0x7fffffffL;     /* nothing traced before the slate */
            if (!vt3_wrap("_ZN5sound17playSe_prize_coinEv", &g_vt3_prize_coin, h_vt3_prize_coin)) g_arb_trace_from = -1;
        }
        {   /* LINDBERGH_VT3_IMMERSIVE=1: the match as a world around the head
             * (arbprog.c keeps its camera whole). The game culls its scene to
             * its own camera (_sCamera::isVisible, from _sTree::addScene: a
             * bounding sphere against the frustum), and the headset sees far
             * wider - outside the broadcast camera's view was black. Every
             * node is visible, as House of the Dead 4's culling is widened. */
            vt3_camlog_install();
            vt3_flowlog_install();
            if (!g_vt3_flowlog && g_world_capable && vt3_hold_on() &&
                vt3_wrap("_ZN17usrAcEntranceLoop4initEv", &g_vt3_entrance, h_vt3_entrance))
                fprintf(stderr, "[sega] Virtua Tennis 3: the frame after each match's intro starts held (LINDBERGH_VT3_HOLD=0 off)\n");
            vt3_hud_anchor_install();
            vt3_routines_install();
            { const char *u = getenv("LINDBERGH_VT3_ATTACHLOG");
              if (u && *u == '1' && vt3_wrap("_ZN14_sSceneManager12attachCameraEP8_sCamera", &g_vt3_attach_cam, h_vt3_attach_cam))
                  fprintf(stderr, "[sega] Virtua Tennis 3: camera attaches logged\n"); }
            { const char *u = getenv("LINDBERGH_VT3_CAMFIELDS");
              if (((u && *u) || g_rout_on || world_mode_immersive()) && vt3_wrap("_ZN6camera11gameRoutineEv", &g_vt3_game_routine, h_vt3_game_routine))
                  fprintf(stderr, "[sega] Virtua Tennis 3: camera routine hooked (fields %s)\n", u ? u : "-"); }
            { const char *u = getenv("LINDBERGH_VT3_SKIP_UVA");
              int skip_uva = u && *u ? *u == '1' : world_mode_immersive();    /* default: on in world mode */
              if (skip_uva && vt3_wrap("_ZN6camera7initUvaEi", &g_vt3_init_uva, h_vt3_init_uva))
                  fprintf(stderr, "[sega] Virtua Tennis 3: intro fly-by camera skipped\n"); }
            const int im = world_mode_immersive();
            const char *cull = getenv("LINDBERGH_VT3_CULL");    /* 0 off, 1 the game's, 2 (default) widened to the eyes */
            const char *cm = getenv("LINDBERGH_VT3_CULL_MARGIN"), *cl = getenv("LINDBERGH_VT3_CULLLOG");
            int cmode = cull && *cull ? atoi(cull) : 2;
            if (cm && *cm) g_vt3_cull_margin = (float)atof(cm);
            g_vt3_cull_log = cl && *cl == '1';
            if (im && cmode == 0 && guest_override("_ZN8_sCamera9isVisibleEPKN8_sSerial10_sVector4dE", h_true))
                fprintf(stderr, "[sega] Virtua Tennis 3: immersive - scene culling off\n");
            else if (im && cmode == 2 && vt3_wrap("_ZN8_sCamera9isVisibleEPKN8_sSerial10_sVector4dE", &g_vt3_is_visible, h_vt3_is_visible))
                fprintf(stderr, "[sega] Virtua Tennis 3: immersive - scene culled to the eyes' view, %.0f deg margin\n", g_vt3_cull_margin);
            {   const char *u = getenv("LINDBERGH_VT3_STANDS");
                g_vt3_stands = !(u && *u == '0');
                /* installed whenever the game can be Immersive; gated per call by the mode */
                if (g_world_capable && vt3_wrap("_ZN5field4drawEv", &g_vt3_field_draw, h_vt3_field_draw))
                    fprintf(stderr, "[sega] Virtua Tennis 3: stadium draw wrapped (Immersive: %s)\n",
                            g_vt3_stands ? "upper stands, both ends and the sky dome drawn" : "the game's court-camera cut kept"); }
            if (im && vt3_wrap("_ZN8_sRender22setProjectCameraMatrixEv", &g_vt3_proj_cam, h_vt3_proj_cam))
                fprintf(stderr, "[sega] Virtua Tennis 3: immersive - the camera's own projection read\n");
        }
        {   if (vt3_wrap("_ZN13playerManager24check_skip_button_winnerEv", &g_vt3_skip_winner, h_vt3_skip_winner)) {
                g_cutaway_hook = 1;
                if (!vt3_wrap("_ZN5match8isHeatupEl", &g_vt3_is_heatup, h_vt3_is_heatup))
                    fprintf(stderr, "[sega] Virtua Tennis 3: match::isHeatup not wrapped - big-point serve close-ups stay\n");
                fprintf(stderr, "[sega] Virtua Tennis 3: point cutaways %s\n", g_cutaways ? "on" : "off (skipped)");
            } }
        const char *w = getenv("LINDBERGH_VT3_WIN");
        if (w && *w == '1' && vt3_wrap("_ZN5match9calcPointEv", &g_vt3_calc_point, h_vt3_calc_point))
            fprintf(stderr, "[sega] Virtua Tennis 3: 1P wins every point\n");
        const char *sp = getenv("LINDBERGH_VT3_SPLIT");
        g_vt3_split = sp && *sp == '1';
        /* the close-up's pick: forced split (test), or no split while the cutaways
         * are in the world (Immersive: installed always, gated per call - live) */
        if ((g_vt3_split || world_mode_immersive()) && vt3_wrap("_ZN6camera11initWinloseEv", &g_vt3_init_winlose, h_vt3_init_winlose) &&
            vt3_wrap("_ZN8_sRandom13getRandMT_U32Ev", &g_vt3_rand, h_vt3_rand))
            fprintf(stderr, "[sega] Virtua Tennis 3: %s\n", g_vt3_split ? "every point ends on the split-screen camera (none with the cutaways in the world)"
                                                                    : "the close-up's split screen left out while the cutaways are in the world");
        { const char *rt = getenv("LINDBERGH_VT3_REPLAY_TYPE");
          if (rt && *rt && vt3_wrap("_ZN6camera13setReplayTypeEi", &g_vt3_set_replay_type, h_vt3_set_replay_type))
              fprintf(stderr, "[sega] Virtua Tennis 3: replay type forced (%s)\n", rt); }
        /* a replay shot's rewind (replay::reverseFrame): a new shot for the Camera view */
        if (world_mode_immersive() && !vt3_wrap("_ZN6replay12reverseFrameEl", &g_vt3_reverse, h_vt3_reverse))
            fprintf(stderr, "[sega] Virtua Tennis 3: replay::reverseFrame not wrapped - a replay's rewind is found by the camera's jump alone\n");
    }
    if ((g_vt3_pts[0] >= 0 || g_vt3_log) &&
        vt3_wrap("_ZN5match4initEv", &g_vt3_match_init, h_vt3_match_init) &
        vt3_wrap("_ZN5match8calcGameEv", &g_vt3_calc_game, h_vt3_calc_game) && g_vt3_pts[0] >= 0)
        fprintf(stderr, "[sega] Virtua Tennis 3: every game starts at points %d-%d\n", g_vt3_pts[0], g_vt3_pts[1]);
}

void hle_register_sega(void)
{
    int n = 0;
    if (watch_route()) fprintf(stderr, "[sega] route log/jump on\n");
    vt3_jumps();
    if (watch_qte()) fprintf(stderr, "[sega] sniping events watched%s\n", g_qte_log ? ", CQC and sniping logged" : "");
    {
        const char *g = getenv("LINDBERGH_GOD");
        if (g && *g == '1' && (guest_override("_Z26check_player_damage_enablei", h_no_damage) |
                               guest_override("_Z21PlMgr_CanDamagePlayeri", h_no_damage)))  /* The House of the Dead 4 */
            fprintf(stderr, "[sega] the player takes no damage (LINDBERGH_GOD)\n");
    }
    if (widen_culling()) fprintf(stderr, "[sega] model culling widened to %.0f degrees\n", g_cull_fov);
    if (guest_symbol("_ZN6replay14isReplay_pointEv")) {   /* Virtua Tennis 3 */
        /* LINDBERGH_FORCE_REPLAY=1: every point earns a replay - the replay
         * camera reached in seconds, for tests nobody watches */
        const char *r = getenv("LINDBERGH_FORCE_REPLAY");
        if (r && *r == '1') {
            if (guest_override("_ZN6replay14isReplay_pointEv", h_true))
                fprintf(stderr, "[sega] every point is replayed (LINDBERGH_FORCE_REPLAY)\n");
            else
                fprintf(stderr, "[sega] Virtua Tennis 3: WARNING replay::isReplay_point not overridden (override table full?) - LINDBERGH_FORCE_REPLAY has no effect\n");
        }
        /* the headset menu's Replays switch: off, no point earns one
         * (isReplay_point is the only way into the replay loop) */
        else if (vt3_wrap("_ZN6replay14isReplay_pointEv", &g_vt3_is_replay, h_vt3_is_replay)) {
            g_replay_hook = 1; fprintf(stderr, "[sega] Virtua Tennis 3: replays %s (headset menu)\n", g_replays ? "on" : "off");
        } else
            fprintf(stderr, "[sega] Virtua Tennis 3: WARNING replay::isReplay_point not wrapped (override table full?) - "
                            "the Replays switch does nothing, every replay plays\n");
        /* the match's end with replays off, point cutaways off, the flow log -
         * installed whether or not isReplay_point is forced */
        if (!vt3_wrap("_ZN14mtrLoopManager7setModeEl", &g_vt3_set_mode, h_vt3_set_mode))
            fprintf(stderr, "[sega] Virtua Tennis 3: WARNING mtrLoopManager::setMode not wrapped - a match will not end with Replays off, "
                            "point cutaways stay\n");
    }
#if defined(__ANDROID__)
    if (pace_by_display()) fprintf(stderr, "[sega] idle_wait_func: paced by the headset, not the wall clock\n");
#endif

    /* Ghost Squad Evolution gates its whole boot on this one predicate;
     * a game without these symbols simply does not get the override. */
    n += guest_override("_Z18is_dongle_validatev", h_dongle_valid);
    n += guest_override("_Z21check_dongle_validatev", h_dongle_valid);

    /* The base board. amLibInit failing is what produces "SEGA BaseBD not
     * available", and everything else is gated behind it. */
    n += guest_override("amLibInit", h_ok);
    n += guest_override("amLibExit", h_ok);
    n += guest_override("amLibIsBasebdAvailable", h_true);

    /* The JVS link itself. amJvsCheckInit is a predicate, not a status code:
     * amJvsGetNodes reads it as a boolean and answers -5 when it is false,
     * which is how answering it with the library's success value of 0 turned
     * into "-5 JVS node(s) found". */
    n += guest_override("amJvsInit", h_ok);
    n += guest_override("amJvsExit", h_ok);
    n += guest_override("amJvsCheckInit", h_true);

    /* One I/O board on the link - the cabinet's own, with the buttons, the
     * coin slots and the guns. */
    n += guest_override("amJvsGetNodes", h_one);

    /* The security dongle. Nothing here reads a real one, and the game only
     * asks whether it is present before carrying on. */
    n += guest_override("amDongleInit", h_ok);
    n += guest_override("amDongleExit", h_ok);
    n += guest_override("amDongleIsAvailable", h_true);

    /* The dipswitches and the bookkeeping the game reads at start up. It
     * asks for these by name in its console when they are missing. */
    n += guest_override("amDipswInit", h_ok);
    n += guest_override("amDipswExit", h_ok);
    n += guest_override("amDipswGetData", h_dipsw_get);

    /* acpSystem::checkDongle polls amDongleUpdate and raises "Error 15 -
     * Game Program Not Found" the moment it answers anything but zero. */
    n += guest_override("amDongleUpdate", h_ok);

    /* The base board's serial. The game prints it and stores it with its
     * bookkeeping; nothing here depends on the value, only on having one. */
    n += guest_override("amOsinfoGetBaseBoardSerialId", h_serial_id);

    /* The battery-backed store the bookkeeping lives in. */
    n += guest_override("amEepromInit", h_ok);
    n += guest_override("amEepromExit", h_ok);
    n += guest_override("amBackupWrapper_BbBuSramRead", h_sram_read);
    n += guest_override("amBackupWrapper_BbBuSramWrite", h_sram_write);
    n += guest_override("amBackupWrapper_KeyEepromRead", h_eeprom_read);
    n += guest_override("amBackupWrapper_KeyEepromWrite", h_eeprom_write);
    n += guest_override("amEepromRead", h_eeprom_read_api);
    n += guest_override("amEepromWrite", h_eeprom_write_api);

    /* LINDBERGH_NO_JVS=1 leaves the board unanswered, so the game finds no
     * I/O and says so. A test for whether a fault lives in this transport or
     * merely happens to be near it. */
    if (!getenv("LINDBERGH_NO_JVS")) {
        n += guest_override("amJvsSendRequest", h_jvs_send);
        n += guest_override("amJvsRecvAcknowledge", h_jvs_recv);
    }

    {   /* Only worth installing if the two globals can be found; without
         * them this would swallow every error instead of one. */
        uint32_t va = guest_symbol("_ZN9acpSystem8setErrorEl");
        if (va && find_error_globals(va))
            n += guest_override("_ZN9acpSystem8setErrorEl", h_set_error);
    }

    /* The country, and with it the language: the game reads the region byte
     * of the cabinet's system data (0, Japan, on a Japanese board) and shows
     * Japanese for country 0, English otherwise. The Language menu picks it
     * (default English); it applies from the next start. */
    {
#if defined(_WIN32)
        { const char *v = getenv("LINDBERGH_COUNTRY"); g_country = v && *v ? atoi(v) : 1; }
#else
        g_country = settings()->country;     /* LINDBERGH_COUNTRY overrides */
#endif
        if (guest_override("_Z25get_arcade_region_countryv", h_country)) {
            n++;
            fprintf(stderr, "[sega] country %d (%s)\n", g_country, g_country ? "English" : "Japanese");
        }
        /* The House of the Dead 4 asks CConfigMgr::GetRegion - 0 Japan,
         * 1 USA, 2 export - and shows English for anything but Japan. */
        if (guest_override("_ZN10CConfigMgr9GetRegionEv", h_region)) {
            n++;
            fprintf(stderr, "[sega] region %d\n", region_value());
        }
    }

    fprintf(stderr, "[sega] %d base board entry points answered\n", n);
}
