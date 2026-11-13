#include "LinkHub.h"

#include <algorithm>

namespace thruma
{

LinkHub& LinkHub::getInstance()
{
    static LinkHub hub;
    return hub;
}

int LinkHub::registerSource (const std::string& name)
{
    std::lock_guard<std::mutex> lock (mutex);
    for (int i = 0; i < kMaxSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        if (s.used.load()) continue;
        if (s.buffer == nullptr)
        {
            s.buffer.reset (new std::atomic<float>[kCapacity]);
            for (int k = 0; k < kCapacity; ++k) s.buffer[k].store (0.0f, std::memory_order_relaxed);
        }
        s.name = name;
        s.writtenEnd.store (-1);
        s.segStart.store (0);
        s.generation.fetch_add (1);
        s.used.store (true);
        return i;
    }
    return -1;
}

void LinkHub::unregisterSource (int id)
{
    if (id < 0 || id >= kMaxSlots) return;
    std::lock_guard<std::mutex> lock (mutex);
    auto& s = slots[(size_t) id];
    s.used.store (false);
    s.writtenEnd.store (-1);
    s.generation.fetch_add (1);
    s.name.clear();
}

void LinkHub::renameSource (int id, const std::string& name)
{
    if (id < 0 || id >= kMaxSlots) return;
    std::lock_guard<std::mutex> lock (mutex);
    slots[(size_t) id].name = name;
}

std::vector<LinkHub::SourceInfo> LinkHub::listSources() const
{
    std::lock_guard<std::mutex> lock (mutex);
    std::vector<SourceInfo> out;
    for (int i = 0; i < kMaxSlots; ++i)
        if (slots[(size_t) i].used.load()) out.push_back ({ i, slots[(size_t) i].name });
    return out;
}

int LinkHub::findSource (const std::string& name) const
{
    std::lock_guard<std::mutex> lock (mutex);
    for (int i = 0; i < kMaxSlots; ++i)
        if (slots[(size_t) i].used.load() && slots[(size_t) i].name == name) return i;
    return -1;
}

uint32_t LinkHub::getGeneration (int id) const noexcept
{
    return (id >= 0 && id < kMaxSlots) ? slots[(size_t) id].generation.load() : 0u;
}

void LinkHub::write (int id, int64_t pos, const float* data, int n) noexcept
{
    if (id < 0 || id >= kMaxSlots || n <= 0) return;
    auto& s = slots[(size_t) id];
    if (! s.used.load (std::memory_order_acquire) || s.buffer == nullptr) return;

    const int64_t end = s.writtenEnd.load (std::memory_order_relaxed);
    if (pos != end)
        s.segStart.store (pos, std::memory_order_release);   // timeline jump: start a new contiguous segment

    for (int i = 0; i < n; ++i)
        s.buffer[(size_t) ((pos + i) & (kCapacity - 1))].store (data[i], std::memory_order_relaxed);
    s.writtenEnd.store (pos + n, std::memory_order_release);
}

bool LinkHub::read (int id, int64_t pos, float* dest, int n) const noexcept
{
    std::fill (dest, dest + n, 0.0f);
    if (id < 0 || id >= kMaxSlots || n <= 0) return false;
    const auto& s = slots[(size_t) id];
    if (! s.used.load (std::memory_order_acquire) || s.buffer == nullptr) return false;

    const int64_t end = s.writtenEnd.load (std::memory_order_acquire);
    const int64_t start = std::max (s.segStart.load (std::memory_order_acquire), end - (int64_t) kCapacity + 8192);
    if (end < 0) return false;

    const int64_t lo = std::max (pos, start), hi = std::min (pos + n, end);
    for (int64_t p = lo; p < hi; ++p)
        dest[p - pos] = s.buffer[(size_t) (p & (kCapacity - 1))].load (std::memory_order_relaxed);
    return lo == pos && hi == pos + n;
}

} // namespace thruma
