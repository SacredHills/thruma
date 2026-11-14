// Lock-free buffer feeding the live detection display: decimated level / threshold / bleed-floor
// traces plus accepted and rejected hit events. Single writer (audio thread), any number of readers.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace thruma
{

struct DetectionScope
{
    static constexpr int kPoints = 4096;   // x kDecim samples (~5.5 s at 48 kHz)
    static constexpr int kDecim = 64;
    static constexpr int kEvents = 512;

    std::array<std::atomic<float>, kPoints> level {}, threshold {}, floor {};
    std::atomic<int64_t> pointsWritten { 0 };          // point i covers engine samples [i*kDecim, (i+1)*kDecim)

    std::array<std::atomic<int64_t>, kEvents> eventPos {};
    std::array<std::atomic<float>, kEvents> eventVel {};   // velocity (0..1)
    std::array<std::atomic<int>, kEvents> eventReason {};  // 0 = accepted hit, else RejectReason of a possible hit not used
    std::array<std::atomic<float>, kEvents> eventPeak {}, eventThr {};   // level and threshold (dB) of a rejected one
    std::atomic<int64_t> eventsWritten { 0 };
    std::atomic<float> referenceDb { -150.0f };

    // writer state (audio thread only)
    float accLevel = -180.0f;
    int accCount = 0;

    void reset() noexcept
    {
        pointsWritten.store (0);
        eventsWritten.store (0);
        accLevel = -180.0f;
        accCount = 0;
    }

    void push (float levelDb, float thresholdDb, float floorDb) noexcept
    {
        accLevel = levelDb > accLevel ? levelDb : accLevel;
        if (++accCount < kDecim) return;
        const auto i = pointsWritten.load (std::memory_order_relaxed);
        const auto k = (size_t) (i % kPoints);
        level[k].store (accLevel, std::memory_order_relaxed);
        threshold[k].store (thresholdDb, std::memory_order_relaxed);
        floor[k].store (floorDb, std::memory_order_relaxed);
        pointsWritten.store (i + 1, std::memory_order_release);
        accLevel = -180.0f;
        accCount = 0;
    }

    void event (int64_t position, float velocity, int reason = 0, float peakDb = 0.0f, float thrDb = 0.0f) noexcept
    {
        const auto i = eventsWritten.load (std::memory_order_relaxed);
        const auto k = (size_t) (i % kEvents);
        eventPos[k].store (position, std::memory_order_relaxed);
        eventVel[k].store (velocity, std::memory_order_relaxed);
        eventReason[k].store (reason, std::memory_order_relaxed);
        eventPeak[k].store (peakDb, std::memory_order_relaxed);
        eventThr[k].store (thrDb, std::memory_order_relaxed);
        eventsWritten.store (i + 1, std::memory_order_release);
    }
};

} // namespace thruma
