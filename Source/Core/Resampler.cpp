#include "Resampler.h"

#include <algorithm>
#include <cmath>

namespace thruma
{

namespace
{
    double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        const double q = x * x * 0.25;
        for (int k = 1; k < 50; ++k)
        {
            term *= q / ((double) k * k);
            sum += term;
            if (term < 1e-12 * sum) break;
        }
        return sum;
    }
}

std::vector<float> resampleHighQuality (const std::vector<float>& in, double inRate, double outRate)
{
    if (in.empty() || inRate <= 0 || outRate <= 0 || std::abs (inRate - outRate) < 1e-6)
        return in;

    const double ratio = outRate / inRate;              // output samples per input sample
    const double cutoff = std::min (1.0, ratio) * 0.97;  // normalised to input Nyquist, slight guard band
    const int halfTaps = 48;                             // per side, in units of input samples at cutoff 1
    const double beta = 9.0;                             // Kaiser beta (~90-100 dB attenuation)
    const double i0beta = besselI0 (beta);

    // Precompute a finely oversampled kernel for speed.
    const int oversample = 512;
    const double support = halfTaps / cutoff;            // kernel half-width in input samples
    const int tableSize = (int) std::ceil (support * oversample) + 2;
    std::vector<double> table ((size_t) tableSize);
    for (int i = 0; i < tableSize; ++i)
    {
        const double t = (double) i / oversample;        // distance in input samples
        if (t > support) { table[(size_t) i] = 0.0; continue; }
        const double xs = t * cutoff;
        const double sinc = xs < 1e-9 ? 1.0 : std::sin (3.14159265358979323846 * xs) / (3.14159265358979323846 * xs);
        const double r = t / support;
        const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0beta;
        table[(size_t) i] = cutoff * sinc * w;
    }

    auto kernel = [&] (double t)
    {
        t = std::abs (t) * oversample;
        const int i = (int) t;
        if (i + 1 >= tableSize) return 0.0;
        const double f = t - i;
        return table[(size_t) i] + (table[(size_t) i + 1] - table[(size_t) i]) * f;
    };

    const auto outLen = (size_t) std::max (1.0, std::floor ((double) in.size() * ratio));
    std::vector<float> out (outLen);
    const int inN = (int) in.size();
    const int reach = (int) std::ceil (support);

    for (size_t o = 0; o < outLen; ++o)
    {
        const double centre = (double) o / ratio;
        const int c = (int) std::floor (centre);
        double acc = 0.0;
        const int lo = std::max (0, c - reach), hi = std::min (inN - 1, c + reach + 1);
        for (int k = lo; k <= hi; ++k)
            acc += in[(size_t) k] * kernel (centre - k);
        out[o] = (float) acc;
    }
    return out;
}

} // namespace thruma
