/* solib.h - shared objects lifted ahead of time (tools/recomp/solib.py).
 *
 * Their code is lifted to C; their bytes (data, rodata, GOT, tables) are read
 * at start-up from the player's own copy of the library - the app carries none
 * of them. solib.py records where the file is, its size and SHA-256, and its
 * relocations; solib_prepare() reads, checks and relocates it. */
#ifndef SOLIB_H
#define SOLIB_H
#include <stdint.h>

typedef struct {
    uint32_t off;                        /* in the image */
    uint32_t type;                       /* R_386_RELATIVE, _32, _PC32, _GLOB_DAT, _JMP_SLOT */
    uint32_t sym;                        /* the resolved symbol's address (S), 0 for RELATIVE */
} SolibReloc;

typedef struct {
    const char          *name;           /* what the game dlopens */
    const char          *path;           /* the player's file, relative to the executable's folder */
    uint32_t             base, size;
    uint32_t             file_size;
    const char          *file_sha256;    /* hex, of the file solib.py read */
    const char          *image_sha256;   /* hex, of the image it relocated */
    const SolibReloc    *relocs;
    int                  reloc_count;
    uint32_t             init;           /* DT_INIT, or 0 */
    uint32_t             init_array;     /* DT_INIT_ARRAY, or 0 */
    int                  init_count;
} SolibImage;

extern const SolibImage g_solib_images[];
extern const int        g_solib_count;

/* Read, check and relocate every library from the executable's folder
 * (LINDBERGH_SOLIB_DIR=<dir> reads <dir>/<name> instead, for tests). 0 when
 * all are ready; otherwise prints what is missing or wrong and returns -1. */
int solib_prepare(const char *elf_path);

/* why the last solib_prepare() failed ("" when it did not) */
const char *solib_error(void);

#endif
