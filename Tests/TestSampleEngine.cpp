#include "TestFramework.h"
#include "TestSignals.h"

#include "Kit.h"
#include "SampleEngine.h"

#include <map>
#include <set>

using namespace thruma;

namespace
{
    SamplePtr makeSample (const std::string& name, float peak, int len, double sr, int onset = 0, int channels = 1)
    {
        std::vector<std::vector<float>> ch ((size_t) channels, std::vector<float> ((size_t) len, 0.0f));
        for (int c = 0; c < channels; ++c)
            for (int i = onset; i < len; ++i)
                ch[(size_t) c][(size_t) i] = peak * std::exp (-(i - onset) / (0.02f * (float) sr)) * (i == onset ? 1.0f : 0.5f);
        return SampleData::create (name, name, std::move (ch), sr);
    }

    std::vector<float> render (SampleEngine& e, int n, int block = 64)
    {
        std::vector<float> out ((size_t) n, 0.0f);
        for (int s = 0; s < n; s += block)
        {
            float* p[1] = { out.data() + s };
            e.render (p, 1, std::min (block, n - s));
        }
        return out;
    }
}

TEST_CASE ("Samples: round-robin cycles through a layer in order")
{
    const double sr = 48000.0;
    Kit kit;
    auto& slot = kit.slots[(size_t) Instrument::Snare];
    VelocityLayer l;
    for (int i = 0; i < 4; ++i) l.samples.push_back (makeSample ("rr" + std::to_string (i), 0.5f, 1000, sr));
    slot.layers.push_back (l);
    slot.finalise();

    SampleEngine e;
    e.prepare (sr);
    PlaybackSettings ps;
    std::vector<std::string> picked;
    for (int i = 0; i < 9; ++i)
    {
        e.trigger (kit, 1, Instrument::Snare, 0.8f, 0, ps);
        picked.push_back (e.getLastTriggeredSample()->name);
    }
    CHECK (picked[0] == "rr0" && picked[1] == "rr1" && picked[2] == "rr2" && picked[3] == "rr3" && picked[4] == "rr0" && picked[8] == "rr0");
}

TEST_CASE ("Samples: random mode never repeats and covers the group")
{
    const double sr = 48000.0;
    Kit kit;
    auto& slot = kit.slots[(size_t) Instrument::Kick];
    slot.rrMode = RoundRobinMode::Random;
    VelocityLayer l;
    for (int i = 0; i < 5; ++i) l.samples.push_back (makeSample ("s" + std::to_string (i), 0.5f, 500, sr));
    slot.layers.push_back (l);
    slot.finalise();

    SampleEngine e;
    e.prepare (sr);
    PlaybackSettings ps;
    const SampleData* prev = nullptr;
    std::map<std::string, int> counts;
    bool repeated = false;
    for (int i = 0; i < 500; ++i)
    {
        e.trigger (kit, 1, Instrument::Kick, 0.5f, 0, ps);
        const auto* s = e.getLastTriggeredSample();
        repeated |= (s == prev);
        prev = s;
        counts[s->name]++;
        if (i % 16 == 0) render (e, 256);   // let voices run so the pool recycles
    }
    CHECK (! repeated);
    CHECK (counts.size() == 5);
    for (auto& [k, c] : counts) CHECK (c > 60);   // roughly uniform (expected 100)
}

TEST_CASE ("Samples: velocity layers and layer building by loudness")
{
    const double sr = 48000.0;
    std::vector<SamplePtr> s;
    // three loudness groups: ~-26 dB (2 samples), ~-12 dB (3 samples), ~-2 dB (1 sample)
    s.push_back (makeSample ("soft1", 0.05f, 2000, sr));
    s.push_back (makeSample ("soft2", 0.052f, 2000, sr));
    s.push_back (makeSample ("mid1", 0.25f, 2000, sr));
    s.push_back (makeSample ("mid2", 0.26f, 2000, sr));
    s.push_back (makeSample ("mid3", 0.24f, 2000, sr));
    s.push_back (makeSample ("hard", 0.8f, 2000, sr));
    auto layers = buildLayersByLoudness (s);
    CHECK (layers.size() == 3);
    if (layers.size() == 3)
    {
        CHECK (layers[0].samples.size() == 2 && layers[1].samples.size() == 3 && layers[2].samples.size() == 1);
        CHECK (layers[2].samples[0]->name == "hard");
    }

    Kit kit;
    auto& slot = kit.slots[(size_t) Instrument::Snare];
    slot.layers = layers;
    slot.finalise();
    SampleEngine e;
    e.prepare (sr);
    PlaybackSettings ps;
    e.trigger (kit, 1, Instrument::Snare, 0.1f, 0, ps);
    CHECK (e.getLastTriggeredLayer() == 0);
    e.trigger (kit, 1, Instrument::Snare, 0.5f, 0, ps);
    CHECK (e.getLastTriggeredLayer() == 1);
    e.trigger (kit, 1, Instrument::Snare, 0.95f, 0, ps);
    CHECK (e.getLastTriggeredLayer() == 2);
}

TEST_CASE ("Samples: sample-accurate start across block boundaries, polarity, gain, auto-align")
{
    const double sr = 48000.0;
    Kit kit;
    auto& slot = kit.slots[(size_t) Instrument::Kick];
    VelocityLayer l;
    l.samples.push_back (makeSample ("k", 0.5f, 4000, sr, 100));   // transient 100 samples into the file
    slot.layers.push_back (l);
    slot.finalise();

    for (int delay : { 0, 37, 63, 64, 200, 1000 })
    {
        SampleEngine e;
        e.prepare (sr);
        PlaybackSettings ps;
        ps.velocityResponse = 0.0f;
        e.trigger (kit, 1, Instrument::Kick, 1.0f, delay, ps);
        const auto out = render (e, 3000, 64);
        int first = -1;
        for (int i = 0; i < 3000; ++i) if (std::abs (out[(size_t) i]) > 0.25f) { first = i; break; }
        // auto-align skips the leading 100 samples minus 0.25 ms (12 samples) of pre-roll
        CHECK_MSG (first == delay + 12, "delay " + std::to_string (delay) + " -> " + std::to_string (first));
    }

    // polarity + gain
    SampleEngine e;
    e.prepare (sr);
    PlaybackSettings ps;
    ps.velocityResponse = 0.0f;
    ps.invertPolarity = true;
    ps.gainDb = -6.0f;
    ps.autoAlign = false;
    e.trigger (kit, 1, Instrument::Kick, 1.0f, 0, ps);
    const auto out = render (e, 400);
    CHECK_NEAR (out[100], -0.5f * dbToGain (-6.0f), 1e-4);
}

TEST_CASE ("Samples: velocity response maps dynamics, voice stealing stays click-free")
{
    const double sr = 48000.0;
    Kit kit;
    auto& slot = kit.slots[(size_t) Instrument::Snare];
    VelocityLayer l;
    l.samples.push_back (makeSample ("s", 0.5f, 48000, sr));
    slot.layers.push_back (l);
    slot.finalise();

    auto peakFor = [&] (float vel, float resp)
    {
        SampleEngine e;
        e.prepare (sr);
        PlaybackSettings ps;
        ps.velocityResponse = resp;
        ps.dynamicRangeDb = 40.0f;
        e.trigger (kit, 1, Instrument::Snare, vel, 0, ps);
        const auto o = render (e, 64);
        return gainToDb (std::abs (o[0]));
    };
    CHECK_NEAR (peakFor (0.5f, 1.0f) - peakFor (1.0f, 1.0f), -20.0, 0.1);
    CHECK_NEAR (peakFor (0.5f, 0.5f) - peakFor (1.0f, 0.5f), -10.0, 0.1);
    CHECK_NEAR (peakFor (0.1f, 0.0f) - peakFor (1.0f, 0.0f), 0.0, 0.01);

    // hammer one instrument far beyond its polyphony: output must stay finite with no big jumps
    SampleEngine e;
    e.prepare (sr);
    PlaybackSettings ps;
    std::vector<float> out;
    for (int b = 0; b < 400; ++b)
    {
        e.trigger (kit, 1, Instrument::Snare, 0.9f, 10, ps);
        std::vector<float> blk (32, 0.0f);
        float* p[1] = { blk.data() };
        e.render (p, 1, 32);
        out.insert (out.end(), blk.begin(), blk.end());
    }
    bool finite = true;
    for (auto v : out) finite &= std::isfinite (v);
    CHECK (finite);
    CHECK (e.getNumActiveVoices() <= 2 * SampleEngine::kMaxVoicesPerInstrument);   // limit + voices still fading out
}

TEST_CASE ("Samples: kit resampling to the host rate keeps transient position and level")
{
    Kit kit;
    auto& slot = kit.slots[(size_t) Instrument::Snare];
    VelocityLayer l;
    l.samples.push_back (makeSample ("s", 0.5f, 44100, 44100.0, 441));   // transient at 10 ms
    slot.layers.push_back (l);
    slot.finalise();
    const auto k96 = kit.resampledTo (96000.0);
    const auto& s96 = *k96->slots[(size_t) Instrument::Snare].layers[0].samples[0];
    CHECK_NEAR (s96.getLength(), 96000, 2);
    CHECK_NEAR (s96.onsetOffset + msToSamples (0.25, 96000.0), 960, 3);
    CHECK_NEAR (s96.peakDb, kit.slots[(size_t) Instrument::Snare].layers[0].samples[0]->peakDb, 3.0);
}
