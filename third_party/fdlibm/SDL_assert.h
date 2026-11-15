/* lindberghrecomp: stands in for SDL's assertions in k_rem_pio2.c */
#ifndef LG_FDLIBM_SDL_ASSERT_H
#define LG_FDLIBM_SDL_ASSERT_H
#include <assert.h>
#include <string.h>
#define SDL_assert(x) assert(x)
#define SDL_arraysize(a) (sizeof(a) / sizeof((a)[0]))
#define SDL_memset memset
#endif
