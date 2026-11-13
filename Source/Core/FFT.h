// Real FFT (power-of-two sizes) built on an iterative radix-2 complex FFT.
// Tables are built in prepare(); transforms are allocation-free.
#pragma once

#include <complex>
#include <vector>

namespace thruma
{

class RealFFT
{
public:
    void prepare (int size);   // size must be a power of two >= 8
    int  getSize() const noexcept { return n; }
    int  getNumBins() const noexcept { return n / 2 + 1; }

    /** time[n] -> spec[n/2+1] (unnormalised). */
    void forward (const float* time, std::complex<float>* spec) noexcept;
    /** spec[n/2+1] -> time[n], scaled by 1/n so inverse(forward(x)) == x. */
    void inverse (const std::complex<float>* spec, float* time) noexcept;

private:
    void complexFFT (std::complex<float>* data, bool inverse) noexcept;

    int n = 0, half = 0;
    std::vector<int> bitrev;
    std::vector<std::complex<float>> twiddle;  // for half-size complex FFT
    std::vector<std::complex<float>> post;     // real-split twiddles
    std::vector<std::complex<float>> work;
};

} // namespace thruma
