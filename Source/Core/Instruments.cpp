#include "Instruments.h"

#include <algorithm>
#include <cctype>

namespace thruma
{

namespace
{
    // name, short, bodyLo, bodyHi, atkLo, atkHi, hold, riseWin, velWin, retrig, focusMin, range, ghostW, decay, midi, lookahead
    const std::array<DetectionProfile, kNumInstruments> profiles {{
        { "Kick",   "KD",   30.0f,  180.0f, 1200.0f, 6000.0f, 9.0f, 4.0f, 6.0f, 28.0f, -18.0f, 24.0f, 0.3f, 120.0f, 36, 8.0f },
        { "Snare",  "SD",  140.0f, 2500.0f, 1200.0f, 5000.0f, 3.5f, 3.0f, 4.0f, 30.0f, -14.0f, 28.0f, 1.0f, 90.0f, 38, 6.0f },
        { "Tom 1",  "T1",   80.0f, 1500.0f,  800.0f, 4000.0f, 6.0f, 3.5f, 5.0f, 40.0f, -14.0f, 13.0f, 0.2f, 250.0f, 48, 8.0f },
        { "Tom 2",  "T2",   70.0f, 1300.0f,  800.0f, 4000.0f, 6.5f, 3.5f, 5.0f, 40.0f, -14.0f, 13.0f, 0.2f, 280.0f, 47, 8.0f },
        { "Tom 3",  "T3",   55.0f, 1200.0f,  700.0f, 3500.0f, 8.0f, 3.5f, 5.5f, 45.0f, -14.0f, 13.0f, 0.2f, 320.0f, 45, 8.0f },
        { "Tom 4",  "T4",   40.0f, 1000.0f,  700.0f, 3500.0f, 9.0f, 3.5f, 6.0f, 45.0f, -14.0f, 13.0f, 0.2f, 360.0f, 43, 8.0f },
        { "Hi-Hat", "HH", 5000.0f,    0.0f, 6000.0f,    0.0f, 1.0f, 2.0f, 2.5f, 35.0f, -20.0f, 24.0f, 0.7f, 50.0f, 42, 4.5f },
        { "Crash",  "CR", 2500.0f,    0.0f, 3000.0f,    0.0f, 1.5f, 3.0f, 4.0f, 90.0f, -18.0f, 20.0f, 0.2f, 900.0f, 49, 5.5f },
        { "Ride",   "RD", 2500.0f,    0.0f, 3000.0f,    0.0f, 1.5f, 3.0f, 3.5f, 45.0f, -18.0f, 24.0f, 0.5f, 800.0f, 51, 5.5f },
        { "China",  "CH", 2000.0f,    0.0f, 2500.0f,    0.0f, 1.5f, 3.0f, 4.0f, 80.0f, -18.0f, 20.0f, 0.2f, 700.0f, 52, 5.5f },
        { "Splash", "SP", 3500.0f,    0.0f, 4000.0f,    0.0f, 1.5f, 3.0f, 3.0f, 60.0f, -18.0f, 20.0f, 0.2f, 400.0f, 55, 5.5f },
    }};
}

const DetectionProfile& getProfile (Instrument i) noexcept
{
    auto idx = std::clamp (static_cast<int> (i), 0, kNumInstruments - 1);
    return profiles[(size_t) idx];
}

const char* getInstrumentName (Instrument i) noexcept { return getProfile (i).name; }

Instrument guessInstrumentFromName (const std::string& raw) noexcept
{
    std::string s;
    s.reserve (raw.size());
    for (auto c : raw)
        s.push_back ((char) std::tolower ((unsigned char) c));

    auto has = [&s] (const char* k) { return s.find (k) != std::string::npos; };

    if (has ("china"))                                  return Instrument::China;
    if (has ("splash"))                                 return Instrument::Splash;
    if (has ("crash"))                                  return Instrument::Crash;
    if (has ("ride"))                                   return Instrument::Ride;
    if (has ("hihat") || has ("hi-hat") || has ("hi hat") || has ("hat") || has ("hh")) return Instrument::HiHat;
    if (has ("kick") || has ("bassdrum") || has ("bass drum") || has ("bd_") || has ("kd")) return Instrument::Kick;
    if (has ("snare") || has ("sd_") || has ("snr"))    return Instrument::Snare;
    if (has ("floor") || has ("tom4") || has ("tom 4") || has ("ft"))  return Instrument::Tom4;
    if (has ("tom3") || has ("tom 3"))                  return Instrument::Tom3;
    if (has ("tom2") || has ("tom 2") || has ("mid"))   return Instrument::Tom2;
    if (has ("tom"))                                    return Instrument::Tom1;
    return Instrument::Count;
}

} // namespace thruma
