// Small real-time-safe DSP building blocks. Header-only, no allocation after prepare().
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace thruma
{

constexpr double kPi = 3.14159265358979323846;

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g, float floorDb = -150.0f) noexcept
{
    return g > 0.0f ? std::max (floorDb, 20.0f * std::log10 (g)) : floorDb;
}
inline int msToSamples (double ms, double sr) noexcept { return (int) std::lround (ms * 0.001 * sr); }

/** RBJ biquad, transposed direct form II, double-precision state for low-frequency stability. */
class Biquad
{
public:
    void setHighPass (double sr, double hz, double q = 0.70710678) noexcept { design (sr, hz, q, 0); }
    void setLowPass  (double sr, double hz, double q = 0.70710678) noexcept { design (sr, hz, q, 1); }
    void setBandPass (double sr, double hz, double q) noexcept { design (sr, hz, q, 2); }
    void setBypass() noexcept { b0 = 1; b1 = b2 = a1 = a2 = 0; }
    void reset() noexcept { z1 = z2 = 0; }

    inline float process (float xIn) noexcept
    {
        const double x = xIn;
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }

private:
    void design (double sr, double hz, double q, int type) noexcept
    {
        hz = std::clamp (hz, 1.0, sr * 0.49);
        const double w0 = 2.0 * kPi * hz / sr;
        const double cw = std::cos (w0), sw = std::sin (w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha;
        if (type == 0)
        {
            b0 = (1.0 + cw) * 0.5 / a0; b1 = -(1.0 + cw) / a0; b2 = b0;
        }
        else if (type == 1)
        {
            b0 = (1.0 - cw) * 0.5 / a0; b1 = (1.0 - cw) / a0; b2 = b0;
        }
        else   // band-pass, 0 dB peak gain
        {
            b0 = alpha / a0; b1 = 0.0; b2 = -alpha / a0;
        }
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
};

/** Sliding-window maximum (monotonic deque in a fixed ring). O(1) amortised per sample. */
class SlidingMax
{
public:
    void prepare (int maxWindow)
    {
        cap = 1;
        while (cap < maxWindow + 2) cap <<= 1;
        vals.assign ((size_t) cap, 0.0f);
        idx.assign ((size_t) cap, 0);
        reset();
    }
    void setWindow (int w) noexcept { window = std::clamp (w, 1, cap - 2); }
    void reset() noexcept { head = tail = 0; n = 0; }

    inline float push (float v) noexcept
    {
        const int mask = cap - 1;
        while (tail != head && vals[(size_t) ((tail - 1) & mask)] <= v) --tail;
        vals[(size_t) (tail & mask)] = v;
        idx[(size_t) (tail & mask)] = n;
        ++tail;
        while (idx[(size_t) (head & mask)] <= n - window) ++head;
        ++n;
        return vals[(size_t) (head & mask)];
    }

private:
    std::vector<float> vals;
    std::vector<int64_t> idx;
    int cap = 0, window = 1;
    int64_t head = 0, tail = 0, n = 0;
};

/** Fixed-capacity multi-purpose ring buffer of floats with integer delay reads. */
class DelayLine
{
public:
    void prepare (int maxDelay)
    {
        cap = 1;
        while (cap < maxDelay + 1) cap <<= 1;
        buf.assign ((size_t) cap, 0.0f);
        pos = 0;
    }
    void reset() noexcept { std::fill (buf.begin(), buf.end(), 0.0f); pos = 0; }
    inline void push (float v) noexcept { buf[(size_t) (pos & (cap - 1))] = v; ++pos; }
    /** delay 0 = most recently pushed sample. */
    inline float get (int delay) const noexcept { return buf[(size_t) ((pos - 1 - delay) & (cap - 1))]; }
    /** Push then return sample delayed by d (d = 0 returns the input). */
    inline float process (float v, int d) noexcept { push (v); return get (d); }
    int capacity() const noexcept { return cap; }

private:
    std::vector<float> buf;
    int64_t pos = 0;
    int cap = 1;
};

/** One-pole smoothing coefficient for a time constant in samples. */
inline float onePoleCoeff (double timeSamples) noexcept
{
    return timeSamples <= 0.0 ? 0.0f : (float) std::exp (-1.0 / timeSamples);
}

} // namespace thruma
