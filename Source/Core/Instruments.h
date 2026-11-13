// Thruma - drum replacement & debleed
// Instrument definitions and per-instrument detection profiles.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace thruma
{

enum class Instrument : int
{
    Kick = 0,
    Snare,
    Tom1,
    Tom2,
    Tom3,
    Tom4,
    HiHat,
    Crash,
    Ride,
    China,
    Splash,
    Count
};

constexpr int kNumInstruments = static_cast<int> (Instrument::Count);

/** Tuned defaults for detecting one drum type in its close mic.
    These are the "instrument-specific detection profiles". All times in ms,
    frequencies in Hz, levels in dB. */
struct DetectionProfile
{
    const char* name;
    const char* shortName;

    float bandLowHz;      // "body" band used for level / velocity (high-pass corner)
    float bandHighHz;     // "body" band low-pass corner (0 = none)
    float attackLowHz;    // "attack" band used for onset rise detection
    float attackHighHz;   // (0 = none)
    float holdMs;         // body envelope peak-hold (>= half period of lowest relevant frequency)
    float riseWindowMs;   // window over which the envelope rise is measured
    float velocityWindowMs; // window after onset in which the peak is measured
    float retriggerMs;    // default minimum retrigger interval
    float focusMinDb;     // min band/full-band level ratio (rejects out-of-band bleed hits)
    float levelRangeDb;   // hits are accepted down to this far below the loudest hits (at sensitivity 0.5)
    float ghostWeight;    // how much Ghost Sensitivity extends that range (snares/hats have ghost notes)
    float decayMs;        // typical amplitude decay time constant (debleed envelope model default)
    int   defaultMidiNote; // General MIDI drum map
    float lookaheadMs;     // smallest replacement lookahead (latency) with full detection accuracy
};

const DetectionProfile& getProfile (Instrument i) noexcept;
const char* getInstrumentName (Instrument i) noexcept;

/** Best guess of an instrument from a file / folder name ("Kick_03.wav" -> Kick). Returns Count if unknown. */
Instrument guessInstrumentFromName (const std::string& name) noexcept;

} // namespace thruma
