// Polyphonic one-shot sample player for drum replacement.
// Velocity layers, round-robin / random selection, sample-accurate (and future) start times,
// click-free voice stealing. Real-time safe: no allocation after prepare().
#pragma once

#include "Kit.h"

#include <array>
#include <cstdint>

namespace thruma
{

struct PlaybackSettings
{
    float velocityResponse = 0.7f;  // 0 = every hit at full level, 1 = follow detected dynamics over dynamicRangeDb
    float dynamicRangeDb   = 40.0f;
    float gainDb           = 0.0f;
    bool  invertPolarity   = false;
    bool  autoAlign        = true;  // skip the sample's leading silence so its transient lands on the trigger
    int   startOffset      = 0;     // additional samples skipped at the start (>= 0)
};

class SampleEngine
{
public:
    static constexpr int kMaxVoices = 64;
    static constexpr int kMaxVoicesPerInstrument = 8;
    static constexpr int kMaxLayers = 32;

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Start a hit. 'startDelay' = samples from the start of the next render() call (may exceed the block).
        'kitSeq' identifies the kit snapshot the sample belongs to (for RCU lifetime tracking). */
    void trigger (const Kit& kit, uint64_t kitSeq, Instrument inst, float velocity,
                  int startDelay, const PlaybackSettings& ps) noexcept;

    /** Play a sample directly (preview). */
    void triggerSample (const SampleData* s, int domain, uint64_t seq, float gain, int startDelay, int instrumentTag = -1) noexcept;

    /** Adds output into out[ch][0..n). numChannels 1 or 2. */
    void render (float* const* out, int numChannels, int n) noexcept;

    /** Fade out all voices (optionally only those with instrumentTag == tag). */
    void releaseAll (int instrumentTag = -2) noexcept;

    int getNumActiveVoices() const noexcept;
    /** Oldest sequence number of 'domain' (0 = kit, others = caller-defined) referenced by any voice; 'fallback' if none. */
    uint64_t getOldestSeqInUse (int domain, uint64_t fallback) const noexcept;

    /** For tests: which sample (and layer index) the last trigger selected. */
    const SampleData* getLastTriggeredSample() const noexcept { return lastSample; }
    int getLastTriggeredLayer() const noexcept { return lastLayer; }

private:
    struct Voice
    {
        const SampleData* sample = nullptr;
        uint64_t seq = 0;
        int domain = 0;
        int instrument = -1;
        int64_t readPos = 0;     // negative = still waiting to start
        int end = 0;
        float gainL = 0, gainR = 0;
        int fadeInLeft = 0, fadeInLen = 0;
        int fadeOutLeft = -1, fadeOutLen = 0;  // -1 = not fading
        uint64_t age = 0;
        bool active = false;
    };

    Voice* allocateVoice (int instrument) noexcept;
    uint32_t nextRandom() noexcept;

    double sr = 48000.0;
    std::array<Voice, kMaxVoices> voices {};
    std::array<std::array<uint32_t, kMaxLayers>, kNumInstruments> rrCounter {};
    std::array<std::array<int, kMaxLayers>, kNumInstruments> lastRandom {};
    uint64_t ageCounter = 0;
    uint32_t rng = 0x9E3779B9u;
    int fadeOutSamples = 128, fadeInSamples = 48;
    const SampleData* lastSample = nullptr;
    int lastLayer = -1;
};

} // namespace thruma
