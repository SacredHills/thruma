// Detection on audio that went through other plugins first (gain, EQ, compressor, limiter / clipper, noise).
// Users often put a gate, EQ or compressor before the trigger; detection must not fall apart.
#include "TestFramework.h"
#include "TestSignals.h"

#include "DspUtils.h"
#include "TransientDetector.h"
#include "Triggers.h"

#include <cmath>
#include <functional>
#include <string>

using namespace thruma;

namespace
{
    using Fx = std::function<void (sig::Buf&, double)>;

    float peakOf (const sig::Buf& x) { float p = 0; for (auto v : x) p = std::max (p, std::abs (v)); return p; }

    void gainDb (sig::Buf& x, float db) { const float g = std::pow (10.0f, db / 20.0f); for (auto& v : x) v *= g; }

    // feed-forward peak compressor (threshold relative to the track's peak), with auto make-up to the original peak
    void compress (sig::Buf& x, double sr, float thrBelowPeakDb, float ratio, float attackMs, float releaseMs)
    {
        const float pk = peakOf (x);
        const float thr = 20.0f * std::log10 (pk) - thrBelowPeakDb;
        const float aA = std::exp (-1.0f / (float) (attackMs * 0.001 * sr));
        const float aR = std::exp (-1.0f / (float) (releaseMs * 0.001 * sr));
        float env = 0.0f;
        for (auto& v : x)
        {
            const float l = std::abs (v);
            env = l > env ? aA * env + (1 - aA) * l : aR * env + (1 - aR) * l;
            const float ldb = env > 1e-9f ? 20.0f * std::log10 (env) : -180.0f;
            const float over = std::max (0.0f, ldb - thr);
            v *= std::pow (10.0f, -(over - over / ratio) / 20.0f);
        }
        gainDb (x, 20.0f * std::log10 (pk / std::max (1e-9f, peakOf (x))));
    }

    void clip (sig::Buf& x, float belowPeakDb)
    {
        const float pk = peakOf (x), c = pk * std::pow (10.0f, -belowPeakDb / 20.0f);
        for (auto& v : x) v = std::tanh (v / c) * c;
        gainDb (x, 20.0f * std::log10 (pk / peakOf (x)));
    }

    void lowBoost (sig::Buf& x, double sr, float hz, float db)
    {
        Biquad lp; lp.setLowPass (sr, hz);
        const float g = std::pow (10.0f, db / 20.0f) - 1.0f;
        for (auto& v : x) v = v + g * lp.process (v);
    }

    void highCut (sig::Buf& x, double sr, float hz)
    {
        Biquad a, b; a.setLowPass (sr, hz); b.setLowPass (sr, hz);
        for (auto& v : x) v = b.process (a.process (v));
    }

    void noise (sig::Buf& x, float belowPeakDb)
    {
        const float a = peakOf (x) * std::pow (10.0f, -belowPeakDb / 20.0f);
        sig::Rng r (77);
        for (auto& v : x) v += a * r.uni();
    }

    sig::Score run (const sig::Session& s, int src, Instrument inst, const sig::Buf& audio)
    {
        TransientDetector d;
        d.prepare (s.sr);
        DetectorSettings ds; ds.instrument = inst;
        d.setSettings (ds);
        d.reset (0);
        std::vector<int64_t> found;
        for (size_t i = 0; i < audio.size(); i += 512)
            d.process (audio.data() + i, (int) std::min<size_t> (512, audio.size() - i), [&] (const DetectedHit& h) { found.push_back (h.position); });
        std::vector<int64_t> truth;
        for (auto& e : s.eventsOf (src, false)) truth.push_back (e.pos);
        return sig::scoreOnsets (found, truth, s.sr);
    }

    sig::Score runAnalyse (const sig::Session& s, int src, Instrument inst, const sig::Buf& audio)
    {
        DetectorSettings ds; ds.instrument = inst;
        std::vector<int64_t> found, truth;
        for (auto& t : analyseTake (audio.data(), (int64_t) audio.size(), s.sr, ds)) found.push_back (t.position);
        for (auto& e : s.eventsOf (src, false)) truth.push_back (e.pos);
        return sig::scoreOnsets (found, truth, s.sr);
    }
}

TEST_CASE ("Detection after other plugins (gain, EQ, compressor, clipper, noise)")
{
    const auto s = sig::makeMetalSession ({});
    struct Mic { const char* name; int src; Instrument inst; };
    const Mic mics[] = { { "kick", sig::Kick, Instrument::Kick }, { "snare", sig::Snare, Instrument::Snare }, { "tom", sig::Tom, Instrument::Tom1 } };
    struct Case { const char* name; Fx fx; };
    const Case cases[] = {
        { "dry",                 [] (sig::Buf&, double) {} },
        { "-18 dB gain",         [] (sig::Buf& x, double) { gainDb (x, -18.0f); } },
        { "-36 dB gain",         [] (sig::Buf& x, double) { gainDb (x, -36.0f); } },
        { "+12 dB gain (clips)", [] (sig::Buf& x, double) { gainDb (x, 12.0f); for (auto& v : x) v = std::clamp (v, -1.0f, 1.0f); } },
        { "low boost +9 dB",     [] (sig::Buf& x, double sr) { lowBoost (x, sr, 200.0f, 9.0f); } },
        { "high cut 3 kHz",      [] (sig::Buf& x, double sr) { highCut (x, sr, 3000.0f); } },
        { "comp 4:1 fast",       [] (sig::Buf& x, double sr) { compress (x, sr, 18.0f, 4.0f, 0.3f, 60.0f); } },
        { "comp 10:1 fast",      [] (sig::Buf& x, double sr) { compress (x, sr, 24.0f, 10.0f, 0.1f, 40.0f); } },
        { "comp 6:1 slow atk",   [] (sig::Buf& x, double sr) { compress (x, sr, 20.0f, 6.0f, 10.0f, 120.0f); } },
        { "clipper 12 dB",       [] (sig::Buf& x, double) { clip (x, 12.0f); } },
        { "noise -60 dB",        [] (sig::Buf& x, double) { noise (x, 60.0f); } },
    };
    struct Res { std::string mic, fx; sig::Score live, an; };
    std::vector<Res> results;
    for (auto& m : mics)
        for (auto& c : cases)
        {
            auto x = s.mics[(size_t) m.src];
            c.fx (x, s.sr);
            const auto sc = run (s, m.src, m.inst, x);
            const auto an = runAnalyse (s, m.src, m.inst, x);
            std::printf ("    %-6s %-20s live R=%.3f P=%.3f  |  analyse R=%.3f P=%.3f\n", m.name, c.name, sc.recall(), sc.precision(), an.recall(), an.precision());
            results.push_back ({ m.name, c.name, sc, an });
        }
    auto get = [&] (const char* mic, const char* fx) -> const Res&
    {
        for (auto& r : results) if (r.mic == mic && r.fx == fx) return r;
        return results.front();
    };
    // a quieter track (or a plugin turning it down) must not lose hits: live catches up within ~1.5 s,
    // Analyze gets everything
    for (auto* fx : { "-18 dB gain", "-36 dB gain" })
    {
        CHECK (get ("kick", fx).live.recall() >= 0.83);
        CHECK (get ("kick", fx).an.recall() >= 0.98);
        CHECK (get ("snare", fx).live.recall() >= 0.95);
        CHECK (get ("snare", fx).an.recall() >= 0.95);
        CHECK (get ("tom", fx).an.recall() >= 0.95);
    }
    // a compressor before Thruma holds up the kick's tail: fast kicks must still be found
    CHECK (get ("kick", "comp 4:1 fast").live.recall() >= 0.95);
    CHECK (get ("kick", "comp 6:1 slow atk").live.recall() >= 0.95);
    CHECK (get ("kick", "comp 10:1 fast").live.recall() >= 0.88);
    for (auto& r : results)
        if (r.mic == "kick") CHECK (r.live.precision() >= 0.98);
}
