# fdlibm subset (deterministic maths)

The elementary functions `det_math.c` uses in deterministic mode
(`LINDBERGH_DETERMINISTIC=1`, implied by input recording/replay and netplay),
so two hosts with different system libms (Apple's, Android's Bionic) compute
the same bits: sin, cos, tan, atan, atan2, exp, log, log10, pow, with their
kernels and argument reduction.

Source: SDL 2.32.10 `src/libm/` (itself uClibc's copy of Sun's fdlibm 5.3),
taken from the SDL release this project already builds against. Altered for
lindberghrecomp (2026-11-15): `math_private.h` renames every function to
`lg_fd_*` and fixes the little-endian word order; `math_libm.h` and
`SDL_assert.h` are small stand-ins for SDL's headers. The `.c` files are
SDL's unchanged.

Licences:

- fdlibm: "Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
  Developed at SunPro, a Sun Microsystems, Inc. business. Permission to use,
  copy, modify, and distribute this software is freely granted, provided that
  this notice is preserved." (kept at the top of every file)
- SDL's changes: zlib licence, "Copyright (C) 1997-2025 Sam Lantinga"
  (SDL's LICENSE.txt). This copy is altered as described above.
