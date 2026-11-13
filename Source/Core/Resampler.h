// High-quality offline sample-rate conversion (Kaiser-windowed sinc, ~100 dB stopband).
// Used when a WAV is loaded or the host rate changes, so playback in the audio thread is a
// straight copy with no run-time interpolation artifacts.
#pragma once

#include <vector>

namespace thruma
{

/** Resample one channel. Returns the input unchanged if rates match. */
std::vector<float> resampleHighQuality (const std::vector<float>& in, double inRate, double outRate);

} // namespace thruma
