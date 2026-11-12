/*
 * vt3_save.h - Virtua Tennis 3's IC card as a save game: the file side
 * (ai_docs/research/ic-card-save-emulation.md, option C).
 *
 * The game keeps a card player's data, decrypted, as one 612-byte record
 * (CARD_DATA, player+0x490). With Save progress on, hle_sega.c puts 1P's saved
 * record into the player at character select and takes it back after each
 * match's result; this file keeps one record per character in vt3_save.bin.
 * No guest memory here: plain bytes in and out (unit-tested).
 *
 * File (little-endian): header 32 bytes - "VT3SAVE\0", version 1, record size
 * 0x264, count, CRC-32 of the entries, "VT3LHD\0\0"; then per entry 24 + 612
 * bytes - character, 0, saved time (64-bit), CRC-32 of the record, 0, record.
 * Written whole to <file>.tmp, flushed and synced, the old file renamed to
 * <file>.bak, the new one renamed into place. A file that cannot be read is
 * never overwritten: <file>.bak is tried, else it is renamed
 * <file>.unreadable-<time> and the save starts fresh (logged).
 */
#ifndef VT3_SAVE_H
#define VT3_SAVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VT3_SAVE_REC 0x264
#define VT3_SAVE_MAX 64              /* characters (the game has far fewer) */

/* where results came from */
enum { VT3_SAVE_FRESH, VT3_SAVE_LOADED, VT3_SAVE_FROM_BAK, VT3_SAVE_BAD_FRESH };

/* reads the file (path; NULL: LINDBERGH_VT3_SAVE_FILE, else "vt3_save.bin")
 * into memory; returns VT3_SAVE_* and logs what happened. Never fails: an
 * unreadable save means a fresh one. */
int vt3_save_open(const char *path);
const char *vt3_save_path(void);
int vt3_save_count(void);
/* the character's record, or NULL (none saved yet) */
const uint8_t *vt3_save_find(int character);
/* keep the record for the character and write the file; 0 written, -1 not
 * (logged; memory keeps it, the next store tries again) */
int vt3_save_store(int character, const uint8_t rec[VT3_SAVE_REC]);
uint32_t vt3_save_crc32(const uint8_t *p, size_t n);
/* Reset save (panel row / desktop menu): the save file is renamed
 * <file>.reset-<time>.bak and its backup <file>.bak to
 * <file>.reset-<time>.prev.bak (both kept, nothing deleted), the records in
 * memory are dropped and the generation goes up, so a record loaded before
 * is not written back (hle_sega.c) - the next character select starts a
 * fresh one. Any thread. Returns the files moved (0: there was none), -1
 * when one could not be moved (then nothing is dropped). */
int vt3_save_reset(void);
/* bumped by each reset */
unsigned vt3_save_generation(void);

#ifdef __cplusplus
}
#endif
#endif
