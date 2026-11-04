// Bare-metal platform layer for the NUPC engine on the ADSP-SC589 Cortex-A5 (replaces what the Linux build gets
// from <chrono>, std::thread, new[] and /proc/cpuinfo).
//
// Everything is guarded by NUPC_BARE_METAL, which the CMake build defines for the ARM core; the Linux build never
// sees any of this.
#ifndef NUPC_PLATFORM_H
#define NUPC_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef NUPC_BARE_METAL

// ---- Memory arena -------------------------------------------------------------------------------------------------
// One static block (placed in DDR by the linker, .bss) serves every NUPC allocation: the convolver's working block,
// the IR scratch conversion buffer and Ne10's FFT configurations. It is two-ended:
//   * nupc_arena_alloc()   persistent allocations, bump-allocated from the bottom. Individual frees are ignored
//                          (Ne10 frees configs one by one); nupc_arena_reset() releases them all at once.
//   * nupc_arena_scratch() temporary allocations from the top, all released by nupc_arena_scratch_release_all().
// All allocations are 16-byte aligned (Ne10's NEON loads fault on misalignment).
// Not thread safe: only prepare()/release() call it, and those must not run concurrently with each other.
#ifdef __cplusplus
extern "C" {
#endif

void   nupc_arena_init(void *base, size_t bytes);
void  *nupc_arena_alloc(size_t bytes);
void   nupc_arena_free(void *p);
void   nupc_arena_reset(void);
void  *nupc_arena_scratch(size_t bytes);
void   nupc_arena_scratch_release_all(void);   // frees every scratch block
size_t nupc_arena_used(void);
size_t nupc_arena_capacity(void);

// ---- Time ---------------------------------------------------------------------------------------------------------
// Set the core clock once at start-up (Hz); the PMU cycle counter is then used as a monotonic clock.
void     nupc_clock_init(uint32_t cpuHz);
uint32_t nupc_read_cycles(void);        // PMCCNTR, wraps about every 8.6 s at 500 MHz
uint64_t nupc_read_nanos(void);         // monotonic, extends the 32-bit counter (call at least every few seconds)

// Enable flush-to-zero in FPSCR for the calling context (NEON always flushes; this keeps VFP code consistent
// and avoids denormal stalls in the convolution tail).
void     nupc_enable_flush_to_zero(void);

#ifdef __cplusplus
}
#endif

#endif // NUPC_BARE_METAL

#endif // NUPC_PLATFORM_H
