// In-process sharing of audio between plugin instances ("linked instances").
//
// Each instance can publish its input under a name; other instances read it as a debleed
// reference. Data is keyed by host timeline position, so instances processed on different
// threads or in any order stay sample-aligned as long as the reader adds a safety delay of
// at least one host block (it then reads data the writer produced in a previous cycle).
// Wait-free for the audio threads; slots are allocated once and never freed.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace thruma
{

class LinkHub
{
public:
    static constexpr int kMaxSlots = 32;
    static constexpr int kCapacity = 1 << 18;   // ~5.4 s at 48 kHz

    static LinkHub& getInstance();

    /** Message thread. Returns a slot id (>= 0) or -1 if full. */
    int  registerSource (const std::string& name);
    void unregisterSource (int id);
    void renameSource (int id, const std::string& name);

    struct SourceInfo { int id; std::string name; };
    std::vector<SourceInfo> listSources() const;
    int findSource (const std::string& name) const;   // -1 if not found

    /** Audio thread (owner of the slot). */
    void write (int id, int64_t timelinePos, const float* data, int n) noexcept;

    /** Audio thread (any reader). Missing samples are zero. Returns true if the full range was valid. */
    bool read (int id, int64_t timelinePos, float* dest, int n) const noexcept;

    uint32_t getGeneration (int id) const noexcept;

private:
    LinkHub() = default;

    struct Slot
    {
        std::atomic<bool> used { false };
        std::atomic<uint32_t> generation { 0 };
        std::atomic<int64_t> segStart { 0 }, writtenEnd { -1 };
        std::unique_ptr<std::atomic<float>[]> buffer;
        std::string name;    // guarded by mutex
    };

    mutable std::mutex mutex;
    std::array<Slot, kMaxSlots> slots;
};

} // namespace thruma
