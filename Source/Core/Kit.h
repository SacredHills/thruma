// Sample kit data model.
//
// SampleData is immutable once built. A Kit is an immutable snapshot (built off the audio thread,
// resampled to the playback rate) that the audio thread plays from. Kits are published with
// RcuPublisher so swapping kits while voices ring is safe.
#pragma once

#include "Instruments.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace thruma
{

struct SampleData
{
    std::string path;           // original file (never modified)
    std::string name;
    double sourceRate = 48000.0;
    std::vector<std::vector<float>> source;   // native-rate channels (1 or 2)

    double playRate = 0.0;
    std::vector<std::vector<float>> play;     // channels at playRate
    int   onsetOffset = 0;      // samples (at playRate) before the transient; skipped when auto-aligning
    float peakDb = -150.0f;     // peak level of the attack (first 50 ms)
    float attackRmsDb = -150.0f;

    int getNumChannels() const noexcept { return (int) play.size(); }
    int getLength() const noexcept { return play.empty() ? 0 : (int) play[0].size(); }

    /** Build from native-rate audio. Computes analysis values; call preparePlayback() afterwards. */
    static std::shared_ptr<SampleData> create (std::string path, std::string name,
                                               std::vector<std::vector<float>> channels, double rate);

    /** Returns a copy resampled for 'rate' (shares nothing mutable with this). */
    std::shared_ptr<SampleData> withPlaybackRate (double rate) const;
};

using SamplePtr = std::shared_ptr<const SampleData>;

enum class RoundRobinMode : int { Cycle = 0, Random = 1 };

struct VelocityLayer
{
    float velLo = 0.0f, velHi = 1.0f;     // 0..1
    std::vector<SamplePtr> samples;        // round-robin group
};

struct InstrumentSlot
{
    std::vector<VelocityLayer> layers;     // sorted by velHi ascending
    RoundRobinMode rrMode = RoundRobinMode::Cycle;
    int midiNote = 36;
    float maxPeakDb = -150.0f;             // loudest sample in this slot (for velocity normalisation)

    bool isEmpty() const noexcept;
    void finalise();                       // sort layers, compute maxPeakDb
    /** Spread velocity ranges evenly over the layers in their current order. */
    void autoDistributeVelocities();
};

struct Kit
{
    std::string name = "Default";
    double playRate = 48000.0;
    std::array<InstrumentSlot, kNumInstruments> slots;

    Kit();
    /** Copy with every sample converted to 'rate' (reuses samples already at that rate). */
    std::unique_ptr<Kit> resampledTo (double rate) const;
    int findInstrumentForNote (int note) const noexcept;  // -1 if none
};

/** Group samples of one instrument into velocity layers by attack loudness:
    samples within 'toleranceDb' of each other become one round-robin group. */
std::vector<VelocityLayer> buildLayersByLoudness (std::vector<SamplePtr> samples, float toleranceDb = 1.5f);

} // namespace thruma
