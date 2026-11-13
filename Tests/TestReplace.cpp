#include "TestFramework.h"
#include "TestSignals.h"

#include "ReplaceEngine.h"

using namespace thruma;

namespace
{
    std::unique_ptr<Kit> makeClickKit (double sr, Instrument inst)
    {
        auto kit = std::make_unique<Kit>();
        // replacement sample: sharp decaying click, transient at sample 0
        std::vector<float> s ((size_t) (0.1 * sr));
        for (size_t i = 0; i < s.size(); ++i) s[i] = (float) (0.8 * std::exp (-(double) i / (0.004 * sr)) * (i % 2 == 0 ? 1.0 : 0.9));
        VelocityLayer l;
        l.samples.push_back (SampleData::create ("click", "click", { s }, sr));
        kit->slots[(size_t) inst].layers.push_back (l);
        kit->slots[(size_t) inst].finalise();
        kit->playRate = sr;
        return kit;
    }

    struct Run
    {
        std::vector<float> out;
        std::vector<ReplaceHitInfo> hits;
        int latency = 0;
    };

    Run runEngine (const sig::Buf& in, double sr, int block, const ReplaceSettings& rs, const Kit* kit,
                   const TriggerSnapshot* trig = nullptr, int64_t timelineStart = -1,
                   const std::vector<std::pair<int64_t, MidiTrigger>>* midi = nullptr)
    {
        ReplaceEngine e;
        e.prepare (sr, block, 1, 10.0f);
        e.setSettings (rs);
        e.reset();
        Run r;
        r.latency = e.getLatency();
        r.out = in;
        int64_t blockIdx = 0;
        for (size_t s = 0; s < in.size(); s += (size_t) block, ++blockIdx)
        {
            const int n = (int) std::min<size_t> ((size_t) block, in.size() - s);
            float* p[1] = { r.out.data() + s };
            std::vector<MidiTrigger> m;
            if (midi != nullptr)
                for (auto& [bi, t] : *midi) if (bi == blockIdx) m.push_back (t);
            const auto absStart = (int64_t) s;
            e.process (p, 1, n, timelineStart >= 0 ? timelineStart + (int64_t) s : -1, kit, 1, trig,
                       m.data(), (int) m.size(), [&] (const ReplaceHitInfo& h)
                       {
                           auto c = h;
                           c.outputOffset += (int) absStart;   // make absolute for checking
                           r.hits.push_back (c);
                       });
        }
        return r;
    }

    int firstAbove (const std::vector<float>& x, float thr, int from = 0)
    {
        for (size_t i = (size_t) from; i < x.size(); ++i) if (std::abs (x[i]) > thr) return (int) i;
        return -1;
    }
}

TEST_CASE ("Replace: replacement sample lands exactly on the detected onset (latency compensated)")
{
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        sig::Buf in ((size_t) sr, 0.0f);
        const int64_t onset = (int64_t) (0.3 * sr) + 7;
        sig::addAt (in, sig::snare (sr, 3), onset, 0.8f);
        const auto kit = makeClickKit (sr, Instrument::Snare);

        ReplaceSettings rs;
        rs.detector.instrument = Instrument::Snare;
        rs.blend = 1.0f;
        const auto r = runEngine (in, sr, 256, rs, kit.get());
        const int first = firstAbove (r.out, 0.1f);
        const double errMs = (first - (onset + r.latency)) * 1000.0 / sr;
        tf::metric (("replacement timing error @ " + std::to_string ((int) sr) + " Hz").c_str(), errMs, "ms");
        CHECK (r.hits.size() == 1);
        CHECK (std::abs (errMs) <= 0.25);
    }
}

TEST_CASE ("Replace: output is bit-identical for any host buffer size")
{
    sig::SessionOptions o;
    o.seconds = 5.0;
    const auto s = sig::makeMetalSession (o);
    const auto kit = makeClickKit (s.sr, Instrument::Snare);
    ReplaceSettings rs;
    rs.detector.instrument = Instrument::Snare;
    rs.mode = ReplaceMode::Layer;
    rs.blend = 0.7f;
    const auto ref = runEngine (s.mics[sig::Snare], s.sr, 512, rs, kit.get());
    for (int bs : { 1, 32, 61, 128, 1024, 2048 })
    {
        const auto r = runEngine (s.mics[sig::Snare], s.sr, bs, rs, kit.get());
        double maxDiff = 0.0;
        for (size_t i = 0; i < r.out.size(); ++i) maxDiff = std::max (maxDiff, (double) std::abs (r.out[i] - ref.out[i]));
        CHECK_MSG (maxDiff == 0.0, "block " + std::to_string (bs) + " diff " + std::to_string (maxDiff));
    }
}

TEST_CASE ("Replace: dry path is an exact delay when nothing is replaced (null test)")
{
    sig::SessionOptions o;
    o.seconds = 3.0;
    const auto s = sig::makeMetalSession (o);
    Kit empty;
    ReplaceSettings rs;
    rs.mode = ReplaceMode::Layer;
    const auto r = runEngine (s.mics[sig::Snare], s.sr, 300, rs, &empty);
    double maxErr = 0.0;
    for (size_t i = (size_t) r.latency; i < r.out.size(); ++i)
        maxErr = std::max (maxErr, (double) std::abs (r.out[i] - s.mics[sig::Snare][i - (size_t) r.latency]));
    CHECK (maxErr < 1e-7);
}

TEST_CASE ("Replace: edited triggers and MIDI notes fire at the right output sample")
{
    const double sr = 48000.0;
    sig::Buf in ((size_t) sr, 0.0f);
    const auto kit = makeClickKit (sr, Instrument::Kick);

    TriggerSnapshot snap;
    snap.triggers.push_back ({ 100000 + 5000, 1.0f, false, 1 });
    snap.triggers.push_back ({ 100000 + 20003, 0.5f, false, 2 });

    ReplaceSettings rs;
    rs.detector.instrument = Instrument::Kick;
    rs.source = TriggerSource::Edited;
    rs.blend = 1.0f;
    auto r = runEngine (in, sr, 512, rs, kit.get(), &snap, 100000);
    CHECK (r.hits.size() == 2);
    CHECK (firstAbove (r.out, 0.1f) == 5000 + r.latency);
    CHECK (firstAbove (r.out, 0.01f, 15000) == 20003 + r.latency);

    // MIDI-only: a note in block 3 at offset 100
    rs.source = TriggerSource::MidiOnly;
    std::vector<std::pair<int64_t, MidiTrigger>> midi { { 3, MidiTrigger { 100, (int) Instrument::Kick, 1.0f } } };
    r = runEngine (in, sr, 512, rs, kit.get(), nullptr, -1, &midi);
    CHECK (firstAbove (r.out, 0.1f) == 3 * 512 + 100 + r.latency);
    CHECK (r.hits.size() == 1 && r.hits[0].fromMidi);
}

TEST_CASE ("Replace: dense double-bass take replaced hit-for-hit")
{
    sig::SessionOptions o;
    o.seconds = 8.0;
    const auto s = sig::makeMetalSession (o);
    const auto kit = makeClickKit (s.sr, Instrument::Kick);
    ReplaceSettings rs;
    rs.detector.instrument = Instrument::Kick;
    rs.blend = 1.0f;
    const auto r = runEngine (s.mics[sig::Kick], s.sr, 480, rs, kit.get());
    std::vector<int64_t> starts;
    for (auto& h : r.hits) starts.push_back (h.outputOffset - r.latency);
    std::vector<int64_t> truth;
    for (auto& e : s.eventsOf (sig::Kick)) truth.push_back (e.pos);
    const auto sc = sig::scoreOnsets (starts, truth, s.sr, 5.0);
    tf::metric ("kick replacement F1", sc.f1());
    tf::metric ("kick replacement mean |timing error|", sc.meanAbsErrMs, "ms");
    CHECK (sc.f1() >= 0.98);
    CHECK (sc.meanAbsErrMs < 0.3);
}
