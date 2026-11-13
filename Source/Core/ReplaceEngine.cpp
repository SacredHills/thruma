#include "ReplaceEngine.h"

namespace thruma
{

void ReplaceEngine::prepare (double sampleRate, int maxBlockSize, int numChannels, float lookaheadMs)
{
    sr = sampleRate;
    maxBlock = std::max (1, maxBlockSize);
    channels = std::clamp (numChannels, 1, kMaxChannels);
    lookahead = std::max (1, msToSamples (lookaheadMs, sr));

    detector.prepare (sr);
    samples.prepare (sr);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        dryDelay[(size_t) c].prepare (lookahead + 1);
        wet[(size_t) c].assign ((size_t) maxBlock, 0.0f);
    }
    mono.assign ((size_t) maxBlock, 0.0f);
    prepared = true;
    setSettings (settings);
    reset();
}

void ReplaceEngine::reset() noexcept
{
    if (! prepared) return;
    for (auto& d : dryDelay) d.reset();
    samples.reset();
    scope.reset();
    inPos = 0;
    detector.reset (0);
    const bool en = settings.enabled;
    dryGainCur = (! en || settings.mode == ReplaceMode::Layer) ? 1.0f : 1.0f - settings.blend;
    wetGainCur = en ? settings.blend : 1.0f;
    outGainCur = dbToGain (settings.outputDb);
}

void ReplaceEngine::setSettings (const ReplaceSettings& s) noexcept
{
    settings = s;
    // The detector must report within the lookahead budget for exact alignment.
    settings.detector.maxReportDelayMs = (float) ((lookahead - 1) * 1000.0 / sr);
    detector.setSettings (settings.detector);
    alignShift = std::clamp (msToSamples (settings.alignMs, sr), -(lookahead - detector.getMaxReportDelay()), msToSamples (20.0, sr));
}

void ReplaceEngine::scheduleHit (const Kit* kit, uint64_t kitSeq, int slot, float velocity, int offset) noexcept
{
    if (kit == nullptr || slot < 0 || slot >= kNumKitSlots) return;
    samples.trigger (*kit, kitSeq, (Instrument) slot, velocity, offset, settings.playback);
}

} // namespace thruma
