// Host-test stand-in for Ne10's real FFT (float32 r2c / c2r), just enough for nupc/fft.hpp.
//
// Same conventions as the vendored Ne10 built with NE10_DSP_RFFT_SCALING (see
// ARM/src/oss-services/ne10/MODIFICATIONS.md): N real samples <-> N/2+1 interleaved complex bins; forward is
// unscaled, inverse applies 1/N. Plain double-precision radix-2 so it can be trusted as the reference. This tests the
// NUPC engine and schedulers, NOT Ne10's NEON code (that is covered by the on-target self-test, "nupc test").
#ifndef HOST_SHIM_NE10_H
#define HOST_SHIM_NE10_H

#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

typedef float ne10_float32_t;
typedef int ne10_int32_t;
typedef int ne10_result_t;
#define NE10_OK 0

struct ne10_fft_cpx_float32_t { ne10_float32_t r, i; };

struct HostRfftCfg
{
    int n;
    std::vector<double> re, im;      // scratch, size n
    std::vector<double> cosT, sinT;  // twiddles, size n/2
};
typedef HostRfftCfg* ne10_fft_r2c_cfg_float32_t;

inline ne10_fft_r2c_cfg_float32_t ne10_fft_alloc_r2c_float32(ne10_int32_t n)
{
    HostRfftCfg* c = new HostRfftCfg();
    c->n = n;
    c->re.assign(n, 0.0);
    c->im.assign(n, 0.0);
    c->cosT.resize(n / 2);
    c->sinT.resize(n / 2);
    for (int k = 0; k < n / 2; ++k)
    {
        c->cosT[k] = std::cos(-2.0 * 3.14159265358979323846 * k / n);
        c->sinT[k] = std::sin(-2.0 * 3.14159265358979323846 * k / n);
    }
    return c;
}

inline void ne10_fft_destroy_r2c_float32(ne10_fft_r2c_cfg_float32_t c) { delete c; }

// In-place iterative radix-2; sign = -1 forward, +1 inverse (no scaling).
inline void hostFft(HostRfftCfg* c, int sign)
{
    const int n = c->n;
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
        {
            std::swap(c->re[i], c->re[j]);
            std::swap(c->im[i], c->im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const int step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; ++k)
            {
                const double wr = c->cosT[k * step];
                const double wi = sign < 0 ? c->sinT[k * step] : -c->sinT[k * step];
                const int a = i + k, b = i + k + len / 2;
                const double xr = c->re[b] * wr - c->im[b] * wi;
                const double xi = c->re[b] * wi + c->im[b] * wr;
                c->re[b] = c->re[a] - xr;
                c->im[b] = c->im[a] - xi;
                c->re[a] += xr;
                c->im[a] += xi;
            }
    }
}

inline void ne10_fft_r2c_1d_float32(ne10_fft_cpx_float32_t* fout, ne10_float32_t* fin, ne10_fft_r2c_cfg_float32_t c)
{
    for (int i = 0; i < c->n; ++i) { c->re[i] = fin[i]; c->im[i] = 0.0; }
    hostFft(c, -1);
    for (int k = 0; k <= c->n / 2; ++k)
    {
        fout[k].r = static_cast<float>(c->re[k]);
        fout[k].i = (k == 0 || k == c->n / 2) ? 0.0f : static_cast<float>(c->im[k]);
    }
}

inline void ne10_fft_c2r_1d_float32(ne10_float32_t* fout, ne10_fft_cpx_float32_t* fin, ne10_fft_r2c_cfg_float32_t c)
{
    const int n = c->n;
    for (int k = 0; k <= n / 2; ++k) { c->re[k] = fin[k].r; c->im[k] = fin[k].i; }
    for (int k = 1; k < n / 2; ++k) { c->re[n - k] = fin[k].r; c->im[n - k] = -fin[k].i; }
    hostFft(c, +1);
    for (int i = 0; i < n; ++i)
        fout[i] = static_cast<float>(c->re[i] / n);
}

#endif
