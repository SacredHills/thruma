// Optional extras from hosts that offer them beyond VST3: moving the host's play position and reading the
// track's whole audio. Today: RAVENYR (ext/ravenyr_host_api.h). In every other host nothing is available and
// each call is a no-op.
#pragma once
#include "ext/ravenyr_host_api.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace thruma
{
class HostBridge
{
public:
    HostBridge();

    bool isAvailable() const noexcept { return api != nullptr; }
    /** Audio thread, inside processBlock: remembers which host track this instance is on. */
    void noteCurrentTrack() noexcept;
    bool knowsTrack() const noexcept { return trackKey.load (std::memory_order_relaxed) != 0; }

    bool locate (int64_t samples) const noexcept;
    bool play() const noexcept;
    bool stop() const noexcept;
    bool isHostPlaying() const noexcept;
    double hostSampleRate() const noexcept;
    void keepAwake() const noexcept;

    /** Message thread: the track's own audio (its clips, mono), from the song start, at the host's rate.
        Empty if not available. */
    std::vector<float> readWholeTrack (int64_t maxSamples) const;

private:
    const RavenyrHostApiV1* api = nullptr;
    std::atomic<int64_t> trackKey { 0 };
};
} // namespace thruma
