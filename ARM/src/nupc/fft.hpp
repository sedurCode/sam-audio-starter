// A real-input FFT for power-of-two sizes, in float: a thin, non-owning view over an Ne10 r2c/c2r
// configuration (ne10_fft_alloc_r2c_float32), which this project already links and initialises via
// ne10_init() in ARMAudioEngine's constructor.
//
// Conventions, verified against the Ne10 sources rather than its docs:
//
//  - Spectrum layout is (N/2)+1 interleaved (re, im) complex bins = N+2 floats, i.e. exactly
//    spectrumFloats(N). DC and Nyquist imaginary parts are zero.
//
//  - NEITHER direction scales. NE10_dsp.h claims a 1/N on the inverse, but that applies to the
//    complex-to-complex functions; the r2c/c2r float32 path contains no scaling whatsoever (grep
//    one_by_nfft in NE10_rfft_float32.c -- no hits). So a forward/inverse round trip yields x*N and
//    the caller must fold 1/N in somewhere. PartitionedConvolutionCore::prepare() already does this
//    when it builds the filter spectra, which is why nothing here normalises.
//
//  - Both calls are out-of-place: fout and fin must not overlap.
//
//  - A cfg holds internal scratch (cfg->buffer), so a cfg MUST NOT be shared between transforms
//    running concurrently. One cfg per convolver stage; see PartitionedConvolutionCore::cfgs_.
//
// This class deliberately does not own its cfg: it is copied around inside Stage, and ownership
// lives in the core so that resetting a Stage can never leak or double-free.
#ifndef NEURAL_ARM_FFT_HPP
#define NEURAL_ARM_FFT_HPP

#include <cstddef>

#define __NE10__
#include "NE10.h"

namespace neural {
namespace arm {

class RealFft
{
public:
    RealFft() : size_(0), cfg_(0) {}

    static bool isPowerOfTwo(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

    // Floats in a spectrum of an n-point real transform: (n/2 + 1) complex bins, interleaved.
    static std::size_t spectrumFloats(std::size_t n) { return n + 2; }

    // Ne10 allocates and owns its own twiddle tables inside the cfg, so no caller-provided table
    // memory is needed. Kept (returning 0) so the core's memory-carving arithmetic stays unchanged.
    static std::size_t tableFloats(std::size_t) { return 0; }

    // Point this view at an n-point transform using `cfg`, which the caller continues to own and
    // must keep alive for as long as this view is used.
    void attach(std::size_t n, ne10_fft_r2c_cfg_float32_t cfg) { size_ = n; cfg_ = cfg; }

    bool ready() const { return cfg_ != 0 && size_ != 0; }
    std::size_t size() const { return size_; }

    // size() real samples in `input` -> spectrumFloats(size()) floats in `spectrum`. Unnormalised.
    // `input` is const here but Ne10's prototype takes a non-const pointer, hence the cast; the
    // r2c path treats it as read-only.
    void forward(const float* input, float* spectrum) const
    {
        if (!ready())
            return;
        ne10_fft_r2c_1d_float32(reinterpret_cast<ne10_fft_cpx_float32_t*>(spectrum),
                                const_cast<ne10_float32_t*>(input), cfg_);
    }

    // spectrumFloats(size()) floats in `spectrum` -> size() real samples in `output`. Unnormalised.
    // `spectrum` is NOT const: c2r temporarily rewrites its first bin before restoring it.
    void inverse(float* spectrum, float* output) const
    {
        if (!ready())
            return;
        ne10_fft_c2r_1d_float32(output, reinterpret_cast<ne10_fft_cpx_float32_t*>(spectrum), cfg_);
    }

private:
    std::size_t size_;
    ne10_fft_r2c_cfg_float32_t cfg_; // not owned -- see class comment
};

} // namespace arm
} // namespace neural

#endif // NEURAL_ARM_FFT_HPP