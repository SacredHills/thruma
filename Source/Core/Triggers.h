// Editable trigger lists, offline take analysis and Standard MIDI File export.
#pragma once

#include "TransientDetector.h"

#include <cstdint>
#include <string>
#include <vector>

namespace thruma
{

struct Trigger
{
    int64_t  position = 0;   // sample index on the capture / host timeline
    float    velocity = 1.0f; // 0..1
    bool     ghost = false;
    uint32_t id = 0;

    bool operator== (const Trigger& o) const noexcept
    {
        return position == o.position && ! (velocity < o.velocity) && ! (o.velocity < velocity) && ghost == o.ghost && id == o.id;
    }
};

/** Sorted list of triggers with stable ids. Editing happens on the message thread only;
    the audio thread receives immutable snapshots (see TriggerSnapshot). */
class TriggerList
{
public:
    const std::vector<Trigger>& getAll() const noexcept { return triggers; }
    size_t size() const noexcept { return triggers.size(); }

    uint32_t add (int64_t position, float velocity, bool ghost = false);
    void     addWithId (const Trigger& t);
    bool     remove (uint32_t id);
    bool     move (uint32_t id, int64_t newPosition);
    bool     setVelocity (uint32_t id, float velocity);
    const Trigger* find (uint32_t id) const noexcept;
    /** Nearest trigger within +-tolerance samples, or nullptr. */
    const Trigger* findNearest (int64_t position, int64_t tolerance) const noexcept;
    void clear() { triggers.clear(); }
    void setAll (std::vector<Trigger> t);

    /** Index of the first trigger with position >= p. */
    size_t lowerBound (int64_t p) const noexcept;

    std::vector<uint8_t> serialise() const;
    bool deserialise (const uint8_t* data, size_t size);

private:
    void sort();
    std::vector<Trigger> triggers;
    uint32_t nextId = 1;
};

/** Immutable copy for the audio thread. */
struct TriggerSnapshot
{
    std::vector<Trigger> triggers;   // sorted by position
    size_t lowerBound (int64_t p) const noexcept;
};

/** Offline detection over a whole take with take-normalised velocities (two passes). */
std::vector<Trigger> analyseTake (const float* audio, int64_t length, double sampleRate,
                                  const DetectorSettings& settings, float* outVelocityRefDb = nullptr,
                                  SpectralTemplate* outTemplate = nullptr);

/** A minimal Standard MIDI File (format 0, channel 10) writer. */
struct MidiNoteEvent
{
    int64_t samplePosition;
    int note;
    int velocity;   // 1..127
};

std::vector<uint8_t> writeMidiFile (std::vector<MidiNoteEvent> notes, double sampleRate, double bpm,
                                    int ppq = 960, double noteLengthSeconds = 0.05,
                                    const std::string& trackName = "Thruma");

inline int velocityToMidi (float v) noexcept
{
    const int m = (int) (v * 127.0f + 0.5f);
    return m < 1 ? 1 : (m > 127 ? 127 : m);
}

} // namespace thruma
