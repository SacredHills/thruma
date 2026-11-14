#include "SampleEngine.h"

#include "DspUtils.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace thruma
{

void SampleEngine::prepare (double sampleRate)
{
    sr = sampleRate;
    fadeOutSamples = std::max (16, msToSamples (3.0, sr));
    fadeInSamples  = std::max (8, msToSamples (1.0, sr));
    reset();
}

void SampleEngine::reset() noexcept
{
    for (auto& v : voices) v = Voice {};
    for (auto& a : rrCounter) a.fill (0);
    for (auto& a : lastRandom) a.fill (-1);
    ageCounter = 0;
    rng = 0x9E3779B9u;
    lastSample = nullptr;
    lastLayer = -1;
}

uint32_t SampleEngine::nextRandom() noexcept
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

SampleEngine::Voice* SampleEngine::allocateVoice (int instrument) noexcept
{
    // Enforce per-instrument polyphony: fade the oldest voice of this instrument if at the limit.
    if (instrument >= 0)
    {
        int count = 0;
        Voice* oldest = nullptr;
        for (auto& v : voices)
        {
            if (v.active && v.instrument == instrument && v.fadeOutLeft < 0)
            {
                ++count;
                if (oldest == nullptr || v.age < oldest->age) oldest = &v;
            }
        }
        if (count >= kMaxVoicesPerInstrument && oldest != nullptr)
        {
            oldest->fadeOutLeft = oldest->fadeOutLen = fadeOutSamples;
        }
    }

    for (auto& v : voices)
        if (! v.active) return &v;

    // Steal: prefer voices already fading, then the oldest.
    Voice* best = nullptr;
    for (auto& v : voices)
    {
        if (best == nullptr) { best = &v; continue; }
        const bool vf = v.fadeOutLeft >= 0, bf = best->fadeOutLeft >= 0;
        if (vf != bf) { if (vf) best = &v; continue; }
        if (v.age < best->age) best = &v;
    }
    // A hard steal would click; instead we reuse it immediately but it was the least audible candidate.
    return best;
}

void SampleEngine::trigger (const Kit& kit, uint64_t kitSeq, Instrument inst, float velocity,
                            int startDelay, const PlaybackSettings& ps) noexcept
{
    const int ii = (int) inst;
    if (ii < 0 || ii >= kNumKitSlots) return;
    const auto& slot = kit.slots[(size_t) ii];
    if (slot.layers.empty()) return;

    velocity = std::clamp (velocity, 0.0f, 1.0f);

    // --- velocity layer ---
    int layerIdx = -1;
    const int nLayers = std::min ((int) slot.layers.size(), kMaxLayers);
    for (int l = 0; l < nLayers; ++l)
    {
        const auto& L = slot.layers[(size_t) l];
        if (velocity >= L.velLo && velocity <= L.velHi && ! L.samples.empty()) { layerIdx = l; break; }
    }
    if (layerIdx < 0)
    {
        float bestDist = std::numeric_limits<float>::max();
        for (int l = 0; l < nLayers; ++l)
        {
            const auto& L = slot.layers[(size_t) l];
            if (L.samples.empty()) continue;
            const float d = std::min (std::abs (velocity - L.velLo), std::abs (velocity - L.velHi));
            if (d < bestDist) { bestDist = d; layerIdx = l; }
        }
    }
    if (layerIdx < 0) return;
    const auto& layer = slot.layers[(size_t) layerIdx];
    const int n = (int) layer.samples.size();

    // --- round robin / random ---
    int pick = 0;
    if (n > 1)
    {
        if (slot.rrMode == RoundRobinMode::Cycle)
        {
            pick = (int) (rrCounter[(size_t) ii][(size_t) layerIdx]++ % (uint32_t) n);
        }
        else
        {
            const int prev = lastRandom[(size_t) ii][(size_t) layerIdx];
            pick = (int) (nextRandom() % (uint32_t) (n - 1));
            if (prev >= 0 && pick >= prev) ++pick;   // never repeat the previous sample
            lastRandom[(size_t) ii][(size_t) layerIdx] = pick;
        }
    }
    const SampleData* s = layer.samples[(size_t) pick].get();
    if (s == nullptr || s->getLength() == 0) return;

    lastSample = s;
    lastLayer = layerIdx;
    if (monitor != nullptr)
    {
        auto& m = monitor->slots[(size_t) ii];
        m.velocity.store (velocity, std::memory_order_relaxed);
        m.layer.store (layerIdx, std::memory_order_relaxed);
        m.sampleHash.store (s->pathHash, std::memory_order_relaxed);
        m.count.fetch_add (1, std::memory_order_release);
    }

    // --- level ---
    const float targetDb = slot.maxPeakDb + (velocity - 1.0f) * ps.dynamicRangeDb * std::clamp (ps.velocityResponse, 0.0f, 1.0f);
    const float relDb = std::clamp (targetDb - s->peakDb, -80.0f, 12.0f);
    float g = dbToGain (relDb + ps.gainDb);
    if (ps.invertPolarity) g = -g;

    Voice* v = allocateVoice (ii);
    if (v == nullptr) return;

    const int userTrim = std::max (0, ps.startOffset);
    const int skip = std::clamp ((ps.autoAlign ? s->onsetOffset : 0) + userTrim, 0, s->getLength() - 1);
    *v = Voice {};
    v->sample = s;
    v->seq = kitSeq;
    v->instrument = ii;
    v->readPos = (int64_t) skip - (int64_t) std::max (0, startDelay);
    v->end = s->getLength();
    v->gainL = v->gainR = g;
    // Only a user trim cuts into the sound and needs a de-click fade; auto-align skips silence only.
    v->fadeInLen = v->fadeInLeft = userTrim > 0 ? fadeInSamples : 0;
    v->age = ++ageCounter;
    v->active = true;
}

void SampleEngine::triggerSample (const SampleData* s, int domain, uint64_t seq, float gain, int startDelay, int instrumentTag) noexcept
{
    if (s == nullptr || s->getLength() == 0) return;
    Voice* v = allocateVoice (instrumentTag);
    *v = Voice {};
    v->sample = s;
    v->seq = seq;
    v->domain = domain;
    v->instrument = instrumentTag;
    v->readPos = -(int64_t) std::max (0, startDelay);
    v->end = s->getLength();
    v->gainL = v->gainR = gain;
    v->age = ++ageCounter;
    v->active = true;
}

void SampleEngine::render (float* const* outMain, int numChannelsMain, int n, const AuxTargets* aux) noexcept
{
    std::array<float, kNumKitSlots> peaks {};
    const bool anySolo = mixer != nullptr && mixer->anySolo.load (std::memory_order_relaxed);
    for (auto& v : voices)
    {
        if (! v.active) continue;

        int start = 0;
        if (v.readPos < 0)
        {
            if (-v.readPos >= n) { v.readPos += n; continue; }
            start = (int) -v.readPos;
            v.readPos = 0;
        }

        const SampleData& s = *v.sample;
        const float* srcL = s.play[0].data();
        const float* srcR = s.getNumChannels() > 1 ? s.play[1].data() : srcL;
        const bool stereoSrc = s.getNumChannels() > 1;

        // mixer (kit voices only): volume / mute / solo, and pan (balance for stereo samples, constant power
        // for mono ones, 0 dB in the centre); ramped over the block so moves never click
        float tL = 1.0f, tR = 1.0f;
        const bool mixed = mixer != nullptr && v.domain == 0 && v.instrument >= 0 && v.instrument < kNumKitSlots;
        // where it plays: the main output, or the extra output its drum / pad is routed to (if that one is on)
        float* const* out = outMain;
        int numChannels = numChannelsMain;
        if (mixed && aux != nullptr)
            if (const int o = mixer->slots[(size_t) v.instrument].output.load (std::memory_order_relaxed); o > 0 && o <= AuxTargets::kMax && aux->numCh[o - 1] > 0)
            {
                out = aux->ch[o - 1];
                numChannels = aux->numCh[o - 1];
            }
        if (mixed)
        {
            const auto& m = mixer->slots[(size_t) v.instrument];
            float g = m.gain.load (std::memory_order_relaxed);
            if (m.mute.load (std::memory_order_relaxed) || (anySolo && ! m.solo.load (std::memory_order_relaxed))) g = 0.0f;
            const float pan = std::clamp (m.pan.load (std::memory_order_relaxed), -1.0f, 1.0f);
            if (numChannels == 1) tL = tR = g;
            else if (stereoSrc) { tL = g * std::min (1.0f, 1.0f - pan); tR = g * std::min (1.0f, 1.0f + pan); }
            else
            {
                const float th = (pan + 1.0f) * 0.25f * 3.14159265f;
                tL = g * 1.41421356f * std::cos (th);
                tR = g * 1.41421356f * std::sin (th);
            }
        }
        if (v.mixL < 0.0f) { v.mixL = tL; v.mixR = tR; }
        const float dL = (tL - v.mixL) / (float) std::max (1, n - start), dR = (tR - v.mixR) / (float) std::max (1, n - start);
        float mL = v.mixL, mR = v.mixR, pk = 0.0f;

        for (int i = start; i < n; ++i)
        {
            if (v.readPos >= v.end) { v.active = false; break; }

            float env = 1.0f;
            if (v.fadeInLeft > 0)
            {
                env = 1.0f - (float) v.fadeInLeft / (float) v.fadeInLen;
                --v.fadeInLeft;
            }
            if (v.fadeOutLeft >= 0)
            {
                if (v.fadeOutLeft == 0) { v.active = false; break; }
                env *= (float) v.fadeOutLeft / (float) v.fadeOutLen;
                --v.fadeOutLeft;
            }
            mL += dL;
            mR += dR;

            const auto p = (size_t) v.readPos;
            const float l = srcL[p], r = srcR[p];
            if (numChannels == 1)
            {
                const float o = (stereoSrc ? 0.5f * (l + r) : l) * v.gainL * env * mL;
                out[0][i] += o;
                pk = std::max (pk, std::abs (o));
            }
            else
            {
                const float oL = l * v.gainL * env * mL, oR = r * v.gainR * env * mR;
                out[0][i] += oL;
                out[1][i] += oR;
                pk = std::max (pk, std::max (std::abs (oL), std::abs (oR)));
            }
            ++v.readPos;
        }
        v.mixL = tL;
        v.mixR = tR;
        if (mixed) peaks[(size_t) v.instrument] = std::max (peaks[(size_t) v.instrument], pk);
    }
    if (mixer != nullptr)
        for (int i = 0; i < kNumKitSlots; ++i)
            if (peaks[(size_t) i] > 0.0f)
            {
                auto& m = mixer->slots[(size_t) i].meter;
                if (peaks[(size_t) i] > m.load (std::memory_order_relaxed)) m.store (peaks[(size_t) i], std::memory_order_relaxed);
            }
}

void SampleEngine::releaseAll (int tag) noexcept
{
    for (auto& v : voices)
        if (v.active && (tag == -2 || v.instrument == tag))
        {
            if (v.readPos < 0) v.active = false;              // not started yet: just drop it
            else if (v.fadeOutLeft < 0) v.fadeOutLeft = v.fadeOutLen = fadeOutSamples;
        }
}

int SampleEngine::getNumActiveVoices() const noexcept
{
    int c = 0;
    for (auto& v : voices) c += v.active ? 1 : 0;
    return c;
}

uint64_t SampleEngine::getOldestSeqInUse (int domain, uint64_t fallback) const noexcept
{
    uint64_t m = fallback;
    for (auto& v : voices)
        if (v.active && v.domain == domain) m = std::min (m, v.seq);
    return m;
}

} // namespace thruma
