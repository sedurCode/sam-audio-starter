# Ne10 (vendored subset)

Upstream: https://github.com/projectNe10/Ne10 at commit 1f059a7 (2018-11-15), BSD-3-Clause (see `LICENSE`).

Only the float32 real/complex FFT is included, built with NEON intrinsics (no assembly):
`NE10_fft*.c`, `NE10_rfft_float32*.c`, `NE10_fft_generic_float32.neonintrinsic.cpp`, `NE10_mask_table.c` and headers.

Changes from upstream:
- `NE10_macros.h`: `NE10_MALLOC` / `NE10_FREE` can be redirected to `nupc_arena_alloc` / `nupc_arena_free`
  with `-DNE10_USE_NUPC_ARENA` (set by the CMake build).
- `ne10_bm_init.c` (new): replaces `NE10_init.c` + `NE10_init_dsp.c`. No `/proc/cpuinfo`; wires the four float32
  FFT function pointers to the NEON versions; call `ne10_bm_init()` instead of `ne10_init()`.

Required compile definitions: `NE10_ENABLE_DSP`, `NE10_UNROLL_LEVEL=1`, `NE10_DSP_RFFT_SCALING`.
`NE10_DSP_RFFT_SCALING` makes the c2r transform apply 1/N, which `nupc/Convolution.h` relies on
(without it neither direction scales).
