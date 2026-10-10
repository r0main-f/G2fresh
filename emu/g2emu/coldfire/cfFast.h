// G2fresh's speed-ups to Gearmulator's ColdFire core (re/notes/g2-hardware-and-emulation.md §3.9.7), included by the
// core's cfCpu.h as emu/CMakeLists.txt patches it. They change how fast the core runs, not what it computes: the
// same instructions with the same results, flags, cycle counts and bus accesses in the same order.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define G2_CF_INLINE __attribute__((always_inline))
#ifdef G2_CF_NO_FORCE
#undef G2_CF_INLINE
#define G2_CF_INLINE
#endif
#else
#define G2_CF_INLINE
#endif
