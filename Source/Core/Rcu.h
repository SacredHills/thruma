// Lock-free publication of immutable objects from a non-real-time thread to the audio thread.
//
// The writer (message/background thread) publishes a new immutable object; the audio thread
// picks up the newest pointer at the start of each block and reports the oldest sequence
// number it still references (e.g. a sample still ringing from a previous kit). The writer
// frees only objects older than that, so the audio thread never allocates, frees or blocks.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace thruma
{

template <typename T>
class RcuPublisher
{
public:
    struct Holder
    {
        explicit Holder (std::unique_ptr<T> o, uint64_t s) : object (std::move (o)), seq (s) {}
        std::unique_ptr<T> object;
        uint64_t seq;
    };

    RcuPublisher() = default;
    RcuPublisher (const RcuPublisher&) = delete;
    RcuPublisher& operator= (const RcuPublisher&) = delete;

    /** Writer side. Thread-safe against other writers (guarded by a mutex, never taken by the audio thread). */
    void publish (std::unique_ptr<T> obj)
    {
        std::lock_guard<std::mutex> lock (writerMutex);
        auto h = std::make_unique<Holder> (std::move (obj), nextSeq++);
        current.store (h.get(), std::memory_order_release);
        holders.push_back (std::move (h));
        collectLocked();
    }

    /** Writer side: free objects the audio thread no longer references. Call periodically. */
    void collectGarbage()
    {
        std::lock_guard<std::mutex> lock (writerMutex);
        collectLocked();
    }

    /** Writer side: newest object (may be null). */
    const T* getLatest() const noexcept
    {
        auto* h = current.load (std::memory_order_acquire);
        return h != nullptr ? h->object.get() : nullptr;
    }

    /** Audio side: newest holder (may be null). */
    const Holder* acquire() const noexcept { return current.load (std::memory_order_acquire); }

    /** Audio side: report the oldest sequence still referenced. */
    void reportOldestInUse (uint64_t seq) noexcept { oldestInUse.store (seq, std::memory_order_release); }

    /** Use when the audio thread is known to be stopped (e.g. in releaseResources()). */
    void markAudioIdle() noexcept
    {
        auto* h = current.load (std::memory_order_acquire);
        oldestInUse.store (h != nullptr ? h->seq : 0, std::memory_order_release);
    }

    size_t getNumRetained() const
    {
        std::lock_guard<std::mutex> lock (writerMutex);
        return holders.size();
    }

private:
    void collectLocked()
    {
        const auto inUse = oldestInUse.load (std::memory_order_acquire);
        const auto* latest = current.load (std::memory_order_acquire);
        for (auto it = holders.begin(); it != holders.end();)
        {
            if (it->get() != latest && (*it)->seq < inUse)
                it = holders.erase (it);
            else
                ++it;
        }
    }

    mutable std::mutex writerMutex;
    std::vector<std::unique_ptr<Holder>> holders;
    std::atomic<Holder*> current { nullptr };
    std::atomic<uint64_t> oldestInUse { 0 };
    uint64_t nextSeq = 1;
};

} // namespace thruma
