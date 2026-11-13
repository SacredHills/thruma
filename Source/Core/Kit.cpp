#include "Kit.h"
#include <functional>

#include "DspUtils.h"
#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace thruma
{

namespace
{
    void analyse (SampleData& s)
    {
        const int len = s.getLength();
        if (len == 0) return;
        const int nch = s.getNumChannels();
        const int attackLen = std::min (len, msToSamples (50.0, s.playRate));

        float peak = 0.0f;
        double sumSq = 0.0;
        for (int i = 0; i < attackLen; ++i)
        {
            float m = 0.0f;
            for (int c = 0; c < nch; ++c) m = std::max (m, std::abs (s.play[(size_t) c][(size_t) i]));
            peak = std::max (peak, m);
            sumSq += (double) m * m;
        }
        s.peakDb = gainToDb (peak);
        s.attackRmsDb = gainToDb ((float) std::sqrt (sumSq / std::max (1, attackLen)));

        // Transient position: first sample above 10% of peak, minus 0.25 ms of pre-roll (never negative).
        int onset = 0;
        const float thr = peak * 0.1f;
        for (int i = 0; i < len; ++i)
        {
            float m = 0.0f;
            for (int c = 0; c < nch; ++c) m = std::max (m, std::abs (s.play[(size_t) c][(size_t) i]));
            if (m >= thr) { onset = i; break; }
        }
        s.onsetOffset = std::max (0, onset - msToSamples (0.25, s.playRate));
    }
}

std::shared_ptr<SampleData> SampleData::create (std::string path, std::string name,
                                                std::vector<std::vector<float>> channels, double rate)
{
    auto s = std::make_shared<SampleData>();
    s->path = std::move (path);
    s->pathHash = (uint64_t) std::hash<std::string>{} (s->path);
    s->name = std::move (name);
    s->sourceRate = rate;
    if (channels.size() > 2) channels.resize (2);
    s->source = std::move (channels);
    s->play = s->source;
    s->playRate = rate;
    analyse (*s);
    return s;
}

std::shared_ptr<SampleData> SampleData::withPlaybackRate (double rate) const
{
    auto s = std::make_shared<SampleData>();
    s->path = path;
    s->pathHash = pathHash;
    s->name = name;
    s->sourceRate = sourceRate;
    s->source = source;
    s->playRate = rate;
    s->play.reserve (source.size());
    for (auto& ch : source)
        s->play.push_back (resampleHighQuality (ch, sourceRate, rate));
    analyse (*s);
    return s;
}

bool InstrumentSlot::isEmpty() const noexcept
{
    for (auto& l : layers)
        if (! l.samples.empty()) return false;
    return true;
}

void InstrumentSlot::finalise()
{
    layers.erase (std::remove_if (layers.begin(), layers.end(), [] (const VelocityLayer& l) { return l.samples.empty(); }),
                  layers.end());
    std::stable_sort (layers.begin(), layers.end(), [] (const VelocityLayer& a, const VelocityLayer& b) { return a.velHi < b.velHi; });
    maxPeakDb = -150.0f;
    for (auto& l : layers)
        for (auto& s : l.samples)
            maxPeakDb = std::max (maxPeakDb, s->peakDb);
}

void InstrumentSlot::autoDistributeVelocities()
{
    const auto n = layers.size();
    for (size_t i = 0; i < n; ++i)
    {
        layers[i].velLo = (float) i / (float) n;
        layers[i].velHi = (float) (i + 1) / (float) n;
    }
}

Kit::Kit()
{
    for (int i = 0; i < kNumInstruments; ++i)
        slots[(size_t) i].midiNote = getProfile ((Instrument) i).defaultMidiNote;
    for (int i = kNumInstruments; i < kNumKitSlots; ++i)
        slots[(size_t) i].midiNote = -1;
}

std::unique_ptr<Kit> Kit::resampledTo (double rate) const
{
    auto k = std::make_unique<Kit> (*this);
    k->playRate = rate;
    std::map<const SampleData*, SamplePtr> done;
    for (auto& slot : k->slots)
    {
        for (auto& layer : slot.layers)
        {
            for (auto& s : layer.samples)
            {
                if (s == nullptr || std::abs (s->playRate - rate) < 1e-6) continue;
                auto it = done.find (s.get());
                if (it == done.end())
                    it = done.emplace (s.get(), s->withPlaybackRate (rate)).first;
                s = it->second;
            }
        }
        slot.finalise();
    }
    return k;
}

int Kit::findInstrumentForNote (int note) const noexcept
{
    for (int i = 0; i < kNumKitSlots; ++i)
        if (slots[(size_t) i].midiNote == note && ! (isPadSlot (i) && slots[(size_t) i].isEmpty())) return i;
    return -1;
}

std::vector<VelocityLayer> buildLayersByLoudness (std::vector<SamplePtr> samples, float toleranceDb)
{
    std::vector<VelocityLayer> layers;
    samples.erase (std::remove (samples.begin(), samples.end(), nullptr), samples.end());
    if (samples.empty()) return layers;

    std::sort (samples.begin(), samples.end(), [] (const SamplePtr& a, const SamplePtr& b) { return a->attackRmsDb < b->attackRmsDb; });

    VelocityLayer cur;
    float groupStart = samples.front()->attackRmsDb;
    for (auto& s : samples)
    {
        if (! cur.samples.empty() && s->attackRmsDb - groupStart > toleranceDb)
        {
            layers.push_back (std::move (cur));
            cur = {};
            groupStart = s->attackRmsDb;
        }
        cur.samples.push_back (s);
    }
    layers.push_back (std::move (cur));

    const auto n = layers.size();
    for (size_t i = 0; i < n; ++i)
    {
        layers[i].velLo = (float) i / (float) n;
        layers[i].velHi = (float) (i + 1) / (float) n;
    }
    return layers;
}

} // namespace thruma
