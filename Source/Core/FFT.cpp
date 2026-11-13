#include "FFT.h"

#include <cassert>
#include <cmath>

namespace thruma
{

void RealFFT::prepare (int size)
{
    assert (size >= 8 && (size & (size - 1)) == 0);
    n = size;
    half = n / 2;

    int bits = 0;
    while ((1 << bits) < half) ++bits;
    bitrev.resize ((size_t) half);
    for (int i = 0; i < half; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        bitrev[(size_t) i] = r;
    }

    twiddle.resize ((size_t) half / 2 + 1);
    for (int i = 0; i <= half / 2; ++i)
    {
        const double a = -2.0 * 3.14159265358979323846 * i / half;
        twiddle[(size_t) i] = { (float) std::cos (a), (float) std::sin (a) };
    }

    post.resize ((size_t) half + 1);
    for (int k = 0; k <= half; ++k)
    {
        const double a = -2.0 * 3.14159265358979323846 * k / n;
        post[(size_t) k] = { (float) std::cos (a), (float) std::sin (a) };
    }
    work.assign ((size_t) half, {});
}

void RealFFT::complexFFT (std::complex<float>* d, bool inv) noexcept
{
    for (int i = 0; i < half; ++i)
    {
        const int j = bitrev[(size_t) i];
        if (j > i) std::swap (d[i], d[j]);
    }
    for (int len = 2; len <= half; len <<= 1)
    {
        const int step = half / len;
        const int hl = len / 2;
        for (int start = 0; start < half; start += len)
        {
            for (int k = 0; k < hl; ++k)
            {
                auto w = twiddle[(size_t) (k * step)];
                if (inv) w = std::conj (w);
                const auto u = d[start + k];
                const auto v = d[start + k + hl] * w;
                d[start + k] = u + v;
                d[start + k + hl] = u - v;
            }
        }
    }
}

// Standard "two-for-one" real FFT: pack even/odd samples into one half-size complex FFT.
void RealFFT::forward (const float* time, std::complex<float>* spec) noexcept
{
    for (int i = 0; i < half; ++i)
        work[(size_t) i] = { time[2 * i], time[2 * i + 1] };
    complexFFT (work.data(), false);

    const auto z0 = work[0];
    spec[0]    = { z0.real() + z0.imag(), 0.0f };
    spec[half] = { z0.real() - z0.imag(), 0.0f };
    for (int k = 1; k < half; ++k)
    {
        const auto a = work[(size_t) k];
        const auto b = std::conj (work[(size_t) (half - k)]);
        const auto even = 0.5f * (a + b);
        const auto odd  = std::complex<float> (0.0f, -0.5f) * (a - b);
        spec[k] = even + post[(size_t) k] * odd;
    }
}

void RealFFT::inverse (const std::complex<float>* spec, float* time) noexcept
{
    for (int k = 0; k < half; ++k)
    {
        const auto a = spec[k];
        const auto b = std::conj (spec[half - k]);
        const auto even = a + b;
        const auto odd  = (a - b) * std::conj (post[(size_t) k]);
        work[(size_t) k] = even + std::complex<float> (0.0f, 1.0f) * odd;
    }
    complexFFT (work.data(), true);
    const float scale = 1.0f / (float) n;
    for (int i = 0; i < half; ++i)
    {
        time[2 * i]     = work[(size_t) i].real() * scale;
        time[2 * i + 1] = work[(size_t) i].imag() * scale;
    }
}

} // namespace thruma
