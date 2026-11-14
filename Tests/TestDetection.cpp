#include "TestFramework.h"
#include "TestSignals.h"

#include "TransientDetector.h"
#include "DspUtils.h"
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

TEST_CASE ("Detection: loud bleed and a long snare break do not cause false triggers")
{
    // Real snare tracks: hi-hat/kick bleed 6 dB louder than the default session, and 12 s where the
    // snare does not play while the rest of the kit continues (the classic false-trigger situation).
    sig::SessionOptions o;
    o.seconds = 30.0;
    o.bleedScaleDb = 6.0f;
    o.snareBreakFrom = 8.0;
    o.snareBreakTo = 20.0;
    o.seed = 5;
    const auto s = sig::makeMetalSession (o);
    for (float sens : { 0.5f, 0.3f })
    {
        DetectorSettings ds;
        ds.instrument = Instrument::Snare;
        ds.sensitivity = sens;
        const auto hits = detect (s.mics[sig::Snare], s.sr, ds);
        const auto sc = sig::scoreOnsets (positions (hits), truthOf (s, sig::Snare, true), s.sr);
        int inBreak = 0;
        for (auto& h : hits)
            if (h.position >= (int64_t) (8.0 * s.sr) && h.position < (int64_t) (20.0 * s.sr)) ++inBreak;
        char label[64];
        std::snprintf (label, sizeof label, "snare +6 dB bleed, sens %.0f%%", sens * 100);
        printScore (label, sc);
        tf::metric ("  false triggers during the 12 s break", inBreak);
        // default sensitivity trades a few bleed hits (dragging the threshold up removes them) for fast-playing accuracy
        CHECK (sc.precision() >= (sens > 0.4f ? 0.8 : 0.95));
        CHECK (inBreak <= (sens > 0.4f ? 7 : 0));
        CHECK (truthOf (s, sig::Snare, false).size() > 0);
    }
}

TEST_CASE ("Detection: fast playing - snare rolls and fast double bass with hi-hat bleed")
{
    const double sr = 48000.0;
    struct Case { bool snare; double spacingMs; double minRecall; double maxMeanErrMs; };
    for (auto c : { Case { true, 83, 0.97, 0.3 }, Case { true, 62, 0.94, 0.4 }, Case { true, 47, 0.92, 0.5 }, Case { true, 37, 0.82, 0.7 },
                    Case { false, 62, 0.95, 0.1 }, Case { false, 47, 0.92, 0.1 }, Case { false, 37, 0.92, 0.1 } })
    {
        sig::Rng r (7);
        const auto N = (int64_t) (8 * sr);
        sig::Buf x ((size_t) N, 0.0f);
        std::vector<sig::Buf> v;
        for (unsigned k = 0; k < 4; ++k) v.push_back (c.snare ? sig::snare (sr, k + 3) : sig::kick (sr, k + 3));
        std::vector<int64_t> truth;
        for (double t = 0.3; t < 7.5; t += c.spacingMs / 1000.0)
        {
            const auto p = (int64_t) (t * sr + r.range (-1, 1) * 0.0005 * sr);
            sig::addAt (x, v[truth.size() % 4], p, r.range (0.45f, 0.9f));
            truth.push_back (p);
        }
        const auto hv = sig::hat (sr, 9);
        for (double t = 0.31; t < 7.5; t += 0.125) sig::addAt (x, hv, (int64_t) (t * sr), 0.5f * dbToGain (-12.0f));

        DetectorSettings ds;
        ds.instrument = c.snare ? Instrument::Snare : Instrument::Kick;
        const auto sc = sig::scoreOnsets (positions (detect (x, sr, ds)), truth, sr, std::min (8.0, c.spacingMs * 0.4));
        char label[64];
        std::snprintf (label, sizeof label, "%s every %.0f ms", c.snare ? "snare roll" : "double kick", c.spacingMs);
        printScore (label, sc);
        CHECK (sc.recall() >= c.minRecall);
        CHECK (sc.precision() >= 0.94);
        CHECK (sc.meanAbsErrMs <= c.maxMeanErrMs);
    }
}

TEST_CASE ("Detection: Amount and Match controls, rejected hits report a reason")
{
    const auto& s = session48();
    auto runWith = [&] (float amount, float match, float sens = 0.5f)
    {
        DetectorSettings ds;
        ds.instrument = Instrument::Snare;
        ds.amount = amount; ds.match = match; ds.sensitivity = sens;
        return sig::scoreOnsets (positions (detect (s.mics[sig::Snare], s.sr, ds)), truthOf (s, sig::Snare, true), s.sr);
    };
    const auto base = runWith (0.5f, 0.5f), moreAmount = runWith (1.0f, 0.5f), lessAmount = runWith (0.0f, 0.5f);
    const auto looseMatch = runWith (0.5f, 0.0f), strictMatch = runWith (0.5f, 1.0f);
    printScore ("default", base);
    printScore ("Amount 100%", moreAmount);
    printScore ("Amount 0%", lessAmount);
    printScore ("Match 0%", looseMatch);
    printScore ("Match 100%", strictMatch);
    CHECK (moreAmount.truePos + moreAmount.falsePos >= base.truePos + base.falsePos);
    CHECK (lessAmount.truePos + lessAmount.falsePos <= base.truePos + base.falsePos);
    CHECK (looseMatch.truePos >= base.truePos);
    CHECK (strictMatch.falsePos <= base.falsePos);
    CHECK (strictMatch.truePos <= base.truePos);

    // with Sensitivity at 0 (Amount up, so quiet hits are still noticed) some real hits fall under the threshold;
    // they come back as "too quiet" trigger points near the true positions
    DetectorSettings ds;
    ds.instrument = Instrument::Snare;
    ds.sensitivity = 0.0f;
    ds.amount = 1.0f;
    std::vector<Rejection> rej;
    const auto hits = analyseTake (s.mics[sig::Snare].data(), (int64_t) s.mics[sig::Snare].size(), s.sr, ds, nullptr, nullptr, nullptr, nullptr, 64, &rej);
    std::vector<int64_t> quiet;
    for (auto& r : rej) if (r.reason == RejectReason::TooQuiet) quiet.push_back (r.position);
    const auto truth = truthOf (s, sig::Snare, true);
    std::vector<int64_t> found;
    for (auto& h : hits) found.push_back (h.position);
    const auto missedByThreshold = sig::scoreOnsets (found, truth, s.sr).missed;
    const auto recovered = sig::scoreOnsets (quiet, truth, s.sr).truePos;
    std::printf ("    sensitivity 0: %d hits under the threshold, %d of them shown as 'too quiet' points (%d rejected total)\n",
                 missedByThreshold, recovered, (int) rej.size());
    CHECK (missedByThreshold > 0);
    CHECK (recovered >= missedByThreshold * 6 / 10);
}

