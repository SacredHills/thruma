#include "TestFramework.h"
#include "TestSignals.h"

#include "TransientDetector.h"
#include "Triggers.h"

using namespace thruma;

namespace
{
    std::vector<DetectedHit> detect (const sig::Buf& x, double sr, DetectorSettings ds, int block = 512)
    {
        TransientDetector d;
        d.prepare (sr);
        d.setSettings (ds);
        d.reset (0);
        std::vector<DetectedHit> hits;
        for (size_t s = 0; s < x.size(); s += (size_t) block)
            d.process (x.data() + s, (int) std::min<size_t> ((size_t) block, x.size() - s), [&] (const DetectedHit& h) { hits.push_back (h); });
        return hits;
    }

    std::vector<int64_t> positions (const std::vector<DetectedHit>& h)
    {
        std::vector<int64_t> p;
        for (auto& x : h) p.push_back (x.position);
        return p;
    }

    std::vector<int64_t> truthOf (const sig::Session& s, int src, bool ghosts)
    {
        std::vector<int64_t> p;
        for (auto& e : s.eventsOf (src, ghosts)) p.push_back (e.pos);
        return p;
    }

    void printScore (const char* label, const sig::Score& sc)
    {
        std::printf ("    %-22s P=%.3f R=%.3f F1=%.3f  TP=%d FP=%d FN=%d  err mean=%.3f ms max=%.3f ms\n",
                     label, sc.precision(), sc.recall(), sc.f1(), sc.truePos, sc.falsePos, sc.missed, sc.meanAbsErrMs, sc.maxAbsErrMs);
    }

    const sig::Session& session48()
    {
        static const sig::Session s = sig::makeMetalSession ({});
        return s;
    }
}

TEST_CASE ("Detection: snare mic in a dense metal pattern (bleed from kick, hats, crash, toms)")
{
    const auto& s = session48();
    DetectorSettings ds;
    ds.instrument = Instrument::Snare;
    const auto hits = detect (s.mics[sig::Snare], s.sr, ds);
    const auto all = sig::scoreOnsets (positions (hits), truthOf (s, sig::Snare, true), s.sr);
    const auto main = sig::scoreOnsets (positions (hits), truthOf (s, sig::Snare, false), s.sr);
    printScore ("snare (all incl ghosts)", all);
    printScore ("snare (main hits)", main);
    CHECK (main.recall() >= 0.95);
    CHECK (all.precision() >= 0.85);   // remaining false hits: loud kick+crash bleed at ghost-note level
    CHECK (all.recall() >= 0.85);      // includes ghost notes ~20 dB below the backbeat
    CHECK (main.meanAbsErrMs < 0.5);
    CHECK (main.maxAbsErrMs < 3.0);
}

TEST_CASE ("Detection: kick mic with 16th-note double bass at 190 BPM")
{
    const auto& s = session48();
    DetectorSettings ds;
    ds.instrument = Instrument::Kick;
    const auto hits = detect (s.mics[sig::Kick], s.sr, ds);
    const auto sc = sig::scoreOnsets (positions (hits), truthOf (s, sig::Kick, true), s.sr);
    printScore ("kick", sc);
    CHECK (sc.recall() >= 0.98);
    CHECK (sc.precision() >= 0.98);
    CHECK (sc.meanAbsErrMs < 0.3);
    CHECK (sc.maxAbsErrMs < 3.0);
}

TEST_CASE ("Detection: tom mic rejects heavy snare bleed (-10 dB) with take analysis")
{
    // Toms only play in the final fill, so a live detector has nothing to learn from before the first
    // tom hit. The editor's Analyze pass (two passes over the whole take) handles this case.
    const auto& s = session48();
    DetectorSettings ds;
    ds.instrument = Instrument::Tom3;
    const auto trig = analyseTake (s.mics[sig::Tom].data(), s.length, s.sr, ds);
    std::vector<int64_t> pos;
    for (auto& t : trig) pos.push_back (t.position);
    const auto sc = sig::scoreOnsets (pos, truthOf (s, sig::Tom, true), s.sr);
    printScore ("tom", sc);
    CHECK (sc.recall() >= 0.99);
    CHECK (sc.precision() >= 0.9);
}

TEST_CASE ("Detection: hi-hat and crash mics")
{
    const auto& s = session48();
    DetectorSettings ds;
    ds.instrument = Instrument::HiHat;
    auto sc = sig::scoreOnsets (positions (detect (s.mics[sig::Hat], s.sr, ds)), truthOf (s, sig::Hat, true), s.sr);
    printScore ("hi-hat", sc);
    CHECK (sc.recall() >= 0.95);
    CHECK (sc.precision() >= 0.8);     // crash bleed at -12 dB is spectrally close to the hats

    ds.instrument = Instrument::Crash;
    sc = sig::scoreOnsets (positions (detect (s.mics[sig::Crash], s.sr, ds)), truthOf (s, sig::Crash, true), s.sr);
    printScore ("crash (overhead)", sc);
    CHECK (sc.recall() >= 0.95);
    CHECK (sc.precision() >= 0.8);
}

TEST_CASE ("Detection: velocity follows hit level")
{
    const auto& s = session48();
    DetectorSettings ds;
    ds.instrument = Instrument::Snare;
    float ref = 0;
    const auto trig = analyseTake (s.mics[sig::Snare].data(), (int64_t) s.mics[sig::Snare].size(), s.sr, ds, &ref);
    const auto truth = s.eventsOf (sig::Snare, true);
    std::vector<double> a, b;
    const auto tol = (int64_t) (0.008 * s.sr);
    for (auto& e : truth)
        for (auto& t : trig)
            if (std::llabs (t.position - e.pos) <= tol) { a.push_back (20.0 * std::log10 (e.amp)); b.push_back (t.velocity); break; }
    double ma = 0, mb = 0;
    for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
    ma /= (double) a.size(); mb /= (double) b.size();
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < a.size(); ++i) { sab += (a[i] - ma) * (b[i] - mb); saa += (a[i] - ma) * (a[i] - ma); sbb += (b[i] - mb) * (b[i] - mb); }
    const double corr = sab / std::sqrt (saa * sbb + 1e-30);
    tf::metric ("velocity correlation (dB level vs velocity)", corr);
    CHECK (corr >= 0.9);

    int ghostsFlagged = 0, ghosts = 0;
    for (auto& e : truth)
        if (e.ghost)
        {
            ++ghosts;
            for (auto& t : trig)
                if (std::llabs (t.position - e.pos) <= tol) { ghostsFlagged += t.ghost ? 1 : 0; break; }
        }
    tf::metric ("ghost notes flagged as ghost", ghosts > 0 ? (double) ghostsFlagged / ghosts : 1.0);
    CHECK (ghosts == 0 || ghostsFlagged >= ghosts * 0.7);
}

TEST_CASE ("Detection: double hits vs. minimum retrigger interval")
{
    const double sr = 48000.0;
    sig::Buf x ((size_t) sr, 0.0f);
    const auto sn = sig::snare (sr, 5);
    sig::addAt (x, sn, 9600, 0.8f);
    sig::addAt (x, sn, 9600 + (int64_t) (0.035 * sr), 0.6f);   // 35 ms drag

    DetectorSettings ds;
    ds.instrument = Instrument::Snare;
    ds.doubleHits = true;
    CHECK (detect (x, sr, ds).size() == 2);

    ds.retriggerMs = 60.0f;
    CHECK (detect (x, sr, ds).size() == 1);

    // a single ringing hit must not retrigger, even at high sensitivity
    sig::Buf y ((size_t) sr, 0.0f);
    sig::addAt (y, sn, 4800, 0.9f);
    ds.retriggerMs = 0.0f;
    ds.sensitivity = 0.75f;
    CHECK (detect (y, sr, ds).size() == 1);
}

TEST_CASE ("Detection: block-size independent and identical offline/real-time results")
{
    const auto& s = session48();
    DetectorSettings ds;
    ds.instrument = Instrument::Snare;
    const auto a = positions (detect (s.mics[sig::Snare], s.sr, ds, 17));
    const auto b = positions (detect (s.mics[sig::Snare], s.sr, ds, 4096));
    CHECK (a == b);
}

TEST_CASE ("Detection: consistent across 44.1 / 48 / 96 kHz")
{
    for (double sr : { 44100.0, 96000.0 })
    {
        sig::SessionOptions o;
        o.sampleRate = sr;
        o.seconds = 8.0;
        const auto s = sig::makeMetalSession (o);
        for (auto [src, inst] : { std::pair<int, Instrument> { sig::Snare, Instrument::Snare }, { sig::Kick, Instrument::Kick } })
        {
            DetectorSettings ds;
            ds.instrument = inst;
            const auto sc = sig::scoreOnsets (positions (detect (s.mics[(size_t) src], sr, ds)), truthOf (s, src, true), sr);
            char label[64];
            std::snprintf (label, sizeof label, "%s @ %.0f Hz", src == sig::Kick ? "kick" : "snare", sr);
            printScore (label, sc);
            CHECK (sc.recall() >= (src == sig::Kick ? 0.97 : 0.85));
            CHECK (sc.precision() >= (src == sig::Kick ? 0.97 : 0.8));
            CHECK (sc.meanAbsErrMs < 0.7);
        }
    }
}
