// Real-time drum replacement chain: detection (live or from an edited trigger list or MIDI),
// sample-accurate triggering with lookahead compensation, dry/wet blending.
//
// Timing model: the dry signal is delayed by the lookahead L, the detector reports an onset o
// at most L samples late, so the replacement sample can start exactly at o on the output
// timeline. The engine therefore has a constant latency of L samples, which the plugin reports.
#pragma once

#include "SampleEngine.h"
#include "TransientDetector.h"
#include "Triggers.h"

#include <array>
#include <vector>

namespace thruma
{

enum class ReplaceMode : int { Replace = 0, Layer = 1 };
enum class TriggerSource : int { Live = 0, Edited = 1, MidiOnly = 2 };

struct ReplaceSettings
{
    bool enabled = true;
    ReplaceMode mode = ReplaceMode::Replace;
    TriggerSource source = TriggerSource::Live;
    int sampleSlot = -1;               // kit slot to play for detected hits, -1 = same as detected instrument
    DetectorSettings detector;
    PlaybackSettings playback;
    float blend = 1.0f;                // Replace: dry/wet crossfade; Layer: wet level
    float outputDb = 0.0f;
    float alignMs = 0.0f;              // shift replacement vs. original (+ = later); clamped to [-lookahead, +20 ms]
};

struct ReplaceHitInfo
{
    int instrument;
    float velocity;
    int outputOffset;    // samples from the start of the current output block (may be >= block size)
    bool fromMidi;
    int onsetOffset = 0; // live hits: onset relative to the start of this input block (usually negative)
    bool ghost = false;
};

struct MidiTrigger
{
    int offset;          // within the block
    int instrument;      // kit slot
    float velocity;      // 0..1
};

class ReplaceEngine
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double sampleRate, int maxBlockSize, int numChannels, float lookaheadMs);
    void reset() noexcept;
    void setSettings (const ReplaceSettings& s) noexcept;
    const ReplaceSettings& getSettings() const noexcept { return settings; }

    int getLatency() const noexcept { return lookahead; }
    const TransientDetector& getDetector() const noexcept { return detector; }
    TransientDetector& getDetector() noexcept { return detector; }
    SampleEngine& getSampleEngine() noexcept { return samples; }

    /** io: in/out audio. timelinePos: host timeline position of this input block (for Edited triggers),
        -1 if unknown. kit/triggers may be null. onHit(const ReplaceHitInfo&) is called for each scheduled hit. */
    template <typename OnHit>
    void process (float* const* io, int numCh, int n, int64_t timelinePos,
                  const Kit* kit, uint64_t kitSeq, const TriggerSnapshot* triggers,
                  const MidiTrigger* midi, int numMidi, OnHit&& onHit) noexcept;

private:
    void scheduleHit (const Kit* kit, uint64_t kitSeq, int slot, float velocity, int offset) noexcept;

    ReplaceSettings settings;
    double sr = 48000.0;
    int maxBlock = 512, channels = 1, lookahead = 480, alignShift = 0;
    TransientDetector detector;
    SampleEngine samples;
    std::array<DelayLine, kMaxChannels> dryDelay;
    std::array<std::vector<float>, kMaxChannels> wet;
    std::vector<float> mono;
    float dryGainCur = 0.0f, wetGainCur = 1.0f, outGainCur = 1.0f;
    int64_t inPos = 0;
    bool prepared = false;
};

//==============================================================================
template <typename OnHit>
void ReplaceEngine::process (float* const* io, int numCh, int n, int64_t timelinePos,
                             const Kit* kit, uint64_t kitSeq, const TriggerSnapshot* triggers,
                             const MidiTrigger* midi, int numMidi, OnHit&& onHit) noexcept
{
    if (! prepared) return;
    numCh = std::clamp (numCh, 1, channels);
    n = std::min (n, maxBlock);
    const int detectedSlot = settings.sampleSlot >= 0 ? settings.sampleSlot : (int) settings.detector.instrument;
    const int64_t blockStart = inPos;

    // 1) detection always runs (keeps state continuous); it only triggers in Live mode
    for (int i = 0; i < n; ++i)
    {
        float m = 0.0f;
        for (int c = 0; c < numCh; ++c) m += io[c][i];
        mono[(size_t) i] = m / (float) numCh;
    }
    for (int i = 0; i < n; ++i)
    {
        DetectedHit h;
        if (detector.processSample (mono[(size_t) i], h) && settings.enabled && settings.source == TriggerSource::Live)
        {
            const int off = (int) (h.position - blockStart) + lookahead + alignShift;
            scheduleHit (kit, kitSeq, detectedSlot, h.velocity, std::max (0, off));
            onHit (ReplaceHitInfo { detectedSlot, h.velocity, std::max (0, off), false, (int) (h.position - blockStart), h.ghost });
        }
    }
    inPos += n;

    // 2) edited triggers on the host timeline
    if (settings.enabled && settings.source == TriggerSource::Edited && triggers != nullptr && timelinePos >= 0)
    {
        const int64_t from = timelinePos - lookahead - alignShift;
        for (size_t t = triggers->lowerBound (from); t < triggers->triggers.size(); ++t)
        {
            const auto& tr = triggers->triggers[t];
            if (tr.position >= from + n) break;
            const int off = (int) (tr.position - from);
            scheduleHit (kit, kitSeq, detectedSlot, tr.velocity, off);
            onHit (ReplaceHitInfo { detectedSlot, tr.velocity, off, false });
        }
    }

    // 3) MIDI-triggered hits (delayed by the lookahead to stay aligned with the delayed dry signal)
    for (int m = 0; m < numMidi; ++m)
    {
        const int off = midi[m].offset + lookahead;
        scheduleHit (kit, kitSeq, midi[m].instrument, midi[m].velocity, off);
        onHit (ReplaceHitInfo { midi[m].instrument, midi[m].velocity, off, true });
    }

    // 4) render and mix
    float* wetPtrs[kMaxChannels] = { wet[0].data(), wet[1].data() };
    for (int c = 0; c < numCh; ++c) std::fill (wet[(size_t) c].begin(), wet[(size_t) c].begin() + n, 0.0f);
    samples.render (wetPtrs, numCh, n);

    const float blend = std::clamp (settings.blend, 0.0f, 1.0f);
    float dryT, wetT;
    // When replacement is off only MIDI-triggered voices can sound, at unity, on top of the untouched dry signal.
    // An empty kit slot never mutes the original: replacing with nothing would silence the track.
    const bool slotEmpty = kit == nullptr || kit->slots[(size_t) std::clamp (detectedSlot, 0, kNumInstruments - 1)].isEmpty();
    if (! settings.enabled || (slotEmpty && settings.source != TriggerSource::MidiOnly)) { dryT = 1.0f; wetT = 1.0f; }
    else if (settings.mode == ReplaceMode::Layer)  { dryT = 1.0f; wetT = blend; }
    else                                           { dryT = 1.0f - blend; wetT = blend; }
    const float outT = dbToGain (settings.outputDb);

    const float inv = 1.0f / (float) std::max (1, n);
    const float dStep = (dryT - dryGainCur) * inv, wStep = (wetT - wetGainCur) * inv, oStep = (outT - outGainCur) * inv;
    for (int c = 0; c < numCh; ++c)
    {
        float dg = dryGainCur, wg = wetGainCur, og = outGainCur;
        auto& dl = dryDelay[(size_t) c];
        float* x = io[c];
        const float* w = wet[(size_t) c].data();
        for (int i = 0; i < n; ++i)
        {
            dg += dStep; wg += wStep; og += oStep;
            const float dry = dl.process (x[i], lookahead);
            x[i] = (dry * dg + w[i] * wg) * og;
        }
    }
    dryGainCur = dryT; wetGainCur = wetT; outGainCur = outT;
}

} // namespace thruma
