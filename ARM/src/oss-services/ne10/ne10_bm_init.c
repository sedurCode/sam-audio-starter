/*
 * Bare-metal replacement for Ne10's NE10_init.c / NE10_init_dsp.c (SAM/NUPC port).
 *
 * Upstream ne10_init() probes /proc/cpuinfo for NEON and then fills function-pointer tables for every
 * module. The Cortex-A5 in the ADSP-SC589 always has NEON, so this file defines only the four float32
 * FFT entry points NUPC uses and points them at the NEON implementations. Nothing else of Ne10 is built.
 */
#include <stddef.h>
#include "NE10_dsp.h"

ne10_fft_cfg_float32_t (*ne10_fft_alloc_c2c_float32) (ne10_int32_t nfft);

void (*ne10_fft_c2c_1d_float32) (ne10_fft_cpx_float32_t *fout,
                                 ne10_fft_cpx_float32_t *fin,
                                 ne10_fft_cfg_float32_t cfg,
                                 ne10_int32_t inverse_fft);

void (*ne10_fft_r2c_1d_float32) (ne10_fft_cpx_float32_t *fout,
                                 ne10_float32_t *fin,
                                 ne10_fft_r2c_cfg_float32_t cfg);

void (*ne10_fft_c2r_1d_float32) (ne10_float32_t *fout,
                                 ne10_fft_cpx_float32_t *fin,
                                 ne10_fft_r2c_cfg_float32_t cfg);

/* The int32 FFT is not built; ne10_fft_alloc_c2c_int32_neon (kept in the float32 NEON file) falls back to this. */
ne10_fft_cfg_int32_t ne10_fft_alloc_c2c_int32_c (ne10_int32_t nfft)
{
    (void)nfft;
    return NULL;
}

/* Call once before any other Ne10 call (replaces ne10_init()). */
ne10_result_t ne10_bm_init(void)
{
    ne10_fft_alloc_c2c_float32 = ne10_fft_alloc_c2c_float32_neon;
    ne10_fft_c2c_1d_float32    = ne10_fft_c2c_1d_float32_neon;
    ne10_fft_r2c_1d_float32    = ne10_fft_r2c_1d_float32_neon;
    ne10_fft_c2r_1d_float32    = ne10_fft_c2r_1d_float32_neon;
    return NE10_OK;
}
