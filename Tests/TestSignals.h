// Synthetic drum recordings with ground truth for automated tests.
//
// Real multitrack drum recordings with hand-annotated onsets are the gold standard and can be
// dropped into Tests/data (see README); these synthetic sessions make the suite self-contained
// and deterministic. Each drum is synthesised physically-plausibly (pitch-swept kick with beater
// click, modal snare with band-limited wire noise, pitched toms, filtered-noise hats, inharmonic
// crash), then mixed into five close mics with frequency-dependent, delayed bleed and a noise floor.
#pragma once

#include "../Source/Core/DspUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

namespace sig
{

using Buf = std::vector<float>;
using thruma::Biquad;

struct Rng
{
    explicit Rng (unsigned seed) : g (seed) {}
    float uni() { return std::uniform_real_distribution<float> (-1.0f, 1.0f) (g); }
    float range (float a, float b) { return std::uniform_real_distribution<float> (a, b) (g); }
    std::mt19937 g;
};

inline void normalise (Buf& b, float peak)
{
    float m = 0.0f;
    for (auto v : b) m = std::max (m, std::abs (v));
    if (m > 0.0f) for (auto& v : b) v *= peak / m;
}

inline Buf kick (double sr, unsigned seed)
{
    Rng r (seed);
    Buf b ((size_t) (0.6 * sr));
    double ph = 0.0;
    Biquad hp; hp.setHighPass (sr, 1500.0);
    for (size_t i = 0; i < b.size(); ++i)
    {
        const double t = (double) i / sr;
        const double f = 50.0 + 110.0 * std::exp (-t / 0.03);
        ph += 2.0 * thruma::kPi * f / sr;
        const double body = std::sin (ph) * std::exp (-t / 0.18);
        const double click = hp.process (r.uni()) * std::exp (-t / 0.0025) * 1.2;
        b[i] = (float) (body + click);
    }
    normalise (b, 1.0f);
    return b;
}

inline Buf snare (double sr, unsigned seed)
{
    Rng r (seed);
    Buf b ((size_t) (0.7 * sr));
    Biquad hp, lp; hp.setHighPass (sr, 1500.0); lp.setLowPass (sr, 9000.0);
    for (size_t i = 0; i < b.size(); ++i)
    {
        const double t = (double) i / sr;
        const double modes = 0.6 * std::sin (2.0 * thruma::kPi * 185.0 * t) * std::exp (-t / 0.08)
                           + 0.3 * std::sin (2.0 * thruma::kPi * 330.0 * t + 0.7) * std::exp (-t / 0.06);
        const double wires = lp.process (hp.process (r.uni())) * (0.8 * std::exp (-t / 0.12) + 1.5 * std::exp (-t / 0.004));
        b[i] = (float) (modes + wires);
    }
    normalise (b, 1.0f);
    return b;
}

inline Buf tom (double sr, double freq, unsigned seed)
{
    Rng r (seed);
    Buf b ((size_t) (1.3 * sr));
    Biquad hp; hp.setHighPass (sr, 1000.0);
    double ph = 0.0;
    for (size_t i = 0; i < b.size(); ++i)
    {
        const double t = (double) i / sr;
        const double f = freq * (1.0 + 0.15 * std::exp (-t / 0.05));
        ph += 2.0 * thruma::kPi * f / sr;
        b[i] = (float) (std::sin (ph) * std::exp (-t / 0.35) + hp.process (r.uni()) * std::exp (-t / 0.003) * 0.8);
    }
    normalise (b, 1.0f);
    return b;
}

inline Buf hat (double sr, unsigned seed)
{
    Rng r (seed);
    Buf b ((size_t) (0.25 * sr));
    Biquad h1, h2; h1.setHighPass (sr, 7000.0, 0.54); h2.setHighPass (sr, 7000.0, 1.3);
    for (size_t i = 0; i < b.size(); ++i)
    {
        const double t = (double) i / sr;
        b[i] = (float) (h2.process (h1.process (r.uni())) * std::exp (-t / 0.035));
    }
    normalise (b, 1.0f);
    return b;
}

inline Buf crash (double sr, unsigned seed)
{
    Rng r (seed);
    Buf b ((size_t) (2.6 * sr));
    Biquad hp; hp.setHighPass (sr, 3000.0);
    struct P { double f, a, tau, ph; };
    std::vector<P> partials;
    for (int k = 0; k < 40; ++k)
        partials.push_back ({ r.range (400.0f, 9000.0f), r.range (0.2f, 1.0f), r.range (0.6f, 1.4f), r.range (0.0f, 6.28f) });
    for (size_t i = 0; i < b.size(); ++i)
    {
        const double t = (double) i / sr;
        double s = hp.process (r.uni()) * 3.0 * std::exp (-t / 0.9);
        for (auto& p : partials) s += p.a * 0.08 * std::sin (2.0 * thruma::kPi * p.f * t + p.ph) * std::exp (-t / p.tau);
        const double attack = std::min (1.0, t / 0.0008);
        b[i] = (float) (s * attack);
    }
    normalise (b, 1.0f);
    return b;
}

inline void addAt (Buf& dst, const Buf& src, int64_t pos, float gain)
{
    for (size_t i = 0; i < src.size(); ++i)
    {
        const int64_t p = pos + (int64_t) i;
        if (p < 0) continue;
        if (p >= (int64_t) dst.size()) break;
        dst[(size_t) p] += src[i] * gain;
    }
}

enum Src { Kick = 0, Snare, Tom, Hat, Crash, NumSrc };

struct Event
{
    int src;
    int64_t pos;
    float amp;
    bool ghost = false;
};

struct Session
{
    double sr = 48000.0;
    int64_t length = 0;
    std::vector<Event> events;
    std::array<Buf, NumSrc> clean;   // each source as heard in its own close mic (no bleed)
    std::array<Buf, NumSrc> mics;    // close mics with bleed: Kick, Snare, Tom, Hat, Crash/OH

    std::vector<Event> eventsOf (int s, bool includeGhosts = true) const
    {
        std::vector<Event> v;
        for (auto& e : events)
            if (e.src == s && (includeGhosts || ! e.ghost)) v.push_back (e);
        return v;
    }
};

struct BleedSpec { float gainDb; float delayMs; float lowpassHz; };

/** bleed[mic][src]; diagonal ignored. */
inline std::array<std::array<BleedSpec, NumSrc>, NumSrc> defaultBleed (float scaleDb = 0.0f)
{
    std::array<std::array<BleedSpec, NumSrc>, NumSrc> m {};
    auto set = [&] (int mic, int src, float g, float d, float lp) { m[(size_t) mic][(size_t) src] = { g + scaleDb, d, lp }; };
    //           mic    src     gain   delay  lowpass
    set (Kick,  Snare, -22.0f, 1.8f, 5000.0f); set (Kick,  Tom, -24.0f, 1.5f, 4000.0f);
    set (Kick,  Hat,   -32.0f, 2.2f, 6000.0f); set (Kick,  Crash, -28.0f, 2.5f, 6000.0f);
    set (Snare, Kick,  -20.0f, 1.5f, 4000.0f); set (Snare, Tom, -14.0f, 1.0f, 8000.0f);
    set (Snare, Hat,   -14.0f, 0.8f, 12000.0f); set (Snare, Crash, -16.0f, 2.0f, 12000.0f);
    set (Tom,   Kick,  -20.0f, 1.5f, 4000.0f); set (Tom,   Snare, -10.0f, 1.2f, 9000.0f);
    set (Tom,   Hat,   -20.0f, 1.6f, 10000.0f); set (Tom,   Crash, -14.0f, 1.4f, 12000.0f);
    set (Hat,   Kick,  -26.0f, 2.0f, 3000.0f); set (Hat,   Snare, -10.0f, 0.9f, 10000.0f);
    set (Hat,   Tom,   -18.0f, 1.6f, 9000.0f); set (Hat,   Crash, -12.0f, 1.5f, 14000.0f);
    set (Crash, Kick,  -14.0f, 3.0f, 6000.0f); set (Crash, Snare, -8.0f, 2.6f, 14000.0f);
    set (Crash, Tom,   -9.0f, 2.4f, 14000.0f); set (Crash, Hat,   -8.0f, 2.0f, 16000.0f);
    return m;
}

struct SessionOptions
{
    double sampleRate = 48000.0;
    double seconds = 12.0;
    double bpm = 190.0;
    unsigned seed = 1;
    float bleedScaleDb = 0.0f;
    bool ghostNotes = true;
    bool flams = true;
    bool blastSection = true;
    double snareBreakFrom = -1.0, snareBreakTo = -1.0;   // seconds without snare (rest of the kit keeps playing)
};

inline Session makeMetalSession (const SessionOptions& o)
{
    Session s;
    s.sr = o.sampleRate;
    s.length = (int64_t) (o.seconds * s.sr);
    Rng r (o.seed);

    const double sixteenth = 60.0 / o.bpm / 4.0 * s.sr;
    const int steps = (int) ((o.seconds - 2.5) * s.sr / sixteenth);
    auto at = [&] (int step, double jitterMs = 0.0) { return (int64_t) (s.sr * 0.25 + step * sixteenth + jitterMs * 0.001 * s.sr); };

    for (int st = 0; st < steps; ++st)
    {
        const int bar = st / 16, beatStep = st % 16;
        const bool blast = o.blastSection && (bar % 3 == 1);
        const bool fill = (st >= steps - 8);
        const double j = r.range (-1.5f, 1.5f);

        if (fill)
        {
            if (st % 2 == 0) s.events.push_back ({ Tom, at (st, j), r.range (0.6f, 0.9f) });
            s.events.push_back ({ Kick, at (st, j * 0.5), r.range (0.6f, 0.75f) });
            continue;
        }

        // kick: 16ths in blast bars (double bass), 8ths otherwise
        if (blast || beatStep % 2 == 0)
            s.events.push_back ({ Kick, at (st, j * 0.3), r.range (0.55f, 0.75f) });

        // snare
        if (blast)
        {
            if (beatStep % 2 == 1) s.events.push_back ({ Snare, at (st, j), r.range (0.6f, 0.8f) });
        }
        else
        {
            if (beatStep == 4 || beatStep == 12)
            {
                s.events.push_back ({ Snare, at (st, j), r.range (0.75f, 0.95f) });
                if (o.flams && bar % 2 == 1 && beatStep == 12)  // drag: second hit 35 ms later
                    s.events.push_back ({ Snare, at (st, j + 35.0), r.range (0.55f, 0.7f) });
            }
            else if (o.ghostNotes && (beatStep == 7 || beatStep == 10 || beatStep == 15) && (bar % 2 == 0))
            {
                Event e { Snare, at (st, j), r.range (0.09f, 0.13f) };
                e.ghost = true;
                s.events.push_back (e);
            }
        }

        // hats on 8ths, crash on bar starts
        if (beatStep % 2 == 0 && beatStep != 0) s.events.push_back ({ Hat, at (st, j * 0.5), r.range (0.3f, 0.5f) });
        if (beatStep == 0) s.events.push_back ({ Crash, at (st, j * 0.5), r.range (0.55f, 0.7f) });
    }

    if (o.snareBreakTo > o.snareBreakFrom)
        s.events.erase (std::remove_if (s.events.begin(), s.events.end(), [&] (const Event& e)
        {
            const double t = (double) e.pos / s.sr;
            return e.src == Snare && t >= o.snareBreakFrom && t < o.snareBreakTo;
        }), s.events.end());
    std::sort (s.events.begin(), s.events.end(), [] (const Event& a, const Event& b) { return a.pos < b.pos; });

    // render clean sources with per-hit sample variation (a few synthesised variants per drum)
    std::array<std::vector<Buf>, NumSrc> variants;
    for (unsigned v = 0; v < 4; ++v)
    {
        variants[Kick].push_back (kick (s.sr, o.seed * 31 + v));
        variants[Snare].push_back (snare (s.sr, o.seed * 37 + v));
        variants[Tom].push_back (tom (s.sr, 110.0, o.seed * 41 + v));
        variants[Hat].push_back (hat (s.sr, o.seed * 43 + v));
        variants[Crash].push_back (crash (s.sr, o.seed * 47 + v));
    }
    for (auto& b : s.clean) b.assign ((size_t) s.length, 0.0f);
    for (auto& e : s.events)
    {
        const auto& vs = variants[(size_t) e.src];
        addAt (s.clean[(size_t) e.src], vs[(size_t) (std::abs (e.pos) % (int64_t) vs.size())], e.pos, e.amp);
    }

    // mix mics with bleed
    const auto bleed = defaultBleed (o.bleedScaleDb);
    Rng noise (o.seed * 1000 + 7);
    for (int mic = 0; mic < NumSrc; ++mic)
    {
        Buf m = s.clean[(size_t) mic];
        for (int src = 0; src < NumSrc; ++src)
        {
            if (src == mic) continue;
            const auto& b = bleed[(size_t) mic][(size_t) src];
            Biquad lp; lp.setLowPass (s.sr, b.lowpassHz);
            const auto d = (int64_t) (b.delayMs * 0.001 * s.sr);
            const float g = thruma::dbToGain (b.gainDb);
            const auto& c = s.clean[(size_t) src];
            for (int64_t i = 0; i < s.length; ++i)
            {
                const float v = lp.process (c[(size_t) i]);
                if (i + d < s.length) m[(size_t) (i + d)] += v * g;
            }
        }
        for (auto& v : m) v += noise.uni() * 6.0e-5f;   // ~ -84 dBFS noise floor
        s.mics[(size_t) mic] = std::move (m);
    }
    return s;
}

/** Precision / recall / timing statistics of detected onsets vs. ground truth. */
struct Score
{
    int truePos = 0, falsePos = 0, missed = 0;
    double meanAbsErrMs = 0.0, maxAbsErrMs = 0.0;
    double precision() const { return truePos + falsePos > 0 ? (double) truePos / (truePos + falsePos) : 1.0; }
    double recall() const { return truePos + missed > 0 ? (double) truePos / (truePos + missed) : 1.0; }
    double f1() const { const double p = precision(), r = recall(); return p + r > 0 ? 2 * p * r / (p + r) : 0.0; }
};

inline Score scoreOnsets (const std::vector<int64_t>& detected, const std::vector<int64_t>& truth, double sr, double toleranceMs = 8.0)
{
    Score sc;
    const auto tol = (int64_t) (toleranceMs * 0.001 * sr);
    std::vector<char> used (detected.size(), 0);
    double sumErr = 0.0;
    for (auto t : truth)
    {
        int best = -1;
        int64_t bestD = tol + 1;
        for (size_t i = 0; i < detected.size(); ++i)
        {
            if (used[i]) continue;
            const int64_t d = std::llabs (detected[i] - t);
            if (d < bestD) { bestD = d; best = (int) i; }
        }
        if (best >= 0)
        {
            used[(size_t) best] = 1;
            ++sc.truePos;
            const double e = (double) bestD * 1000.0 / sr;
            sumErr += e;
            sc.maxAbsErrMs = std::max (sc.maxAbsErrMs, e);
        }
        else ++sc.missed;
    }
    for (auto u : used) if (! u) ++sc.falsePos;
    sc.meanAbsErrMs = sc.truePos > 0 ? sumErr / sc.truePos : 0.0;
    return sc;
}

inline double energyDb (const float* x, int64_t n)
{
    double e = 0.0;
    for (int64_t i = 0; i < n; ++i) e += (double) x[i] * x[i];
    return 10.0 * std::log10 (e / std::max<int64_t> (1, n) + 1e-20);
}

} // namespace sig
