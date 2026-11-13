#include "HostBridge.h"
#include <juce_core/juce_core.h>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif

namespace thruma
{
HostBridge::HostBridge()
{
#if JUCE_WINDOWS
    auto fn = reinterpret_cast<RavenyrHostApiV1Fn> (reinterpret_cast<void*> (GetProcAddress (GetModuleHandleW (nullptr), "ravenyr_host_api_v1")));
#else
    auto fn = reinterpret_cast<RavenyrHostApiV1Fn> (dlsym (RTLD_DEFAULT, "ravenyr_host_api_v1"));
#endif
    if (fn != nullptr)
        if (const auto* a = fn(); a != nullptr && a->version >= 1 && a->size >= (int32_t) sizeof (RavenyrHostApiV1))
            api = a;
}

void HostBridge::noteCurrentTrack() noexcept
{
    if (api != nullptr)
        if (const auto k = api->currentTrack(); k != 0) trackKey.store (k, std::memory_order_relaxed);
}

bool HostBridge::locate (int64_t s) const noexcept { if (api == nullptr) return false; api->locate (std::max<int64_t> (0, s)); return true; }
bool HostBridge::play() const noexcept { if (api == nullptr) return false; api->play(); return true; }
bool HostBridge::stop() const noexcept { if (api == nullptr) return false; api->stop(); return true; }
bool HostBridge::isHostPlaying() const noexcept { return api != nullptr && api->isPlaying() != 0; }
double HostBridge::hostSampleRate() const noexcept { return api != nullptr ? api->sampleRate() : 0.0; }
void HostBridge::keepAwake() const noexcept
{
    if (api != nullptr && knowsTrack()) api->wakeTrack (trackKey.load (std::memory_order_relaxed));
}

std::vector<float> HostBridge::readWholeTrack (int64_t maxSamples) const
{
    const auto key = trackKey.load (std::memory_order_relaxed);
    if (api == nullptr || key == 0) return {};
    const auto len = std::min<int64_t> (api->trackLength (key), maxSamples);
    if (len <= 0) return {};
    std::vector<float> mono ((size_t) len, 0.0f);
    const auto got = api->readTrack (key, 0, len, mono.data(), nullptr);
    mono.resize ((size_t) std::max<int64_t> (0, got));
    return mono;
}
} // namespace thruma
