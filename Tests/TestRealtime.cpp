// Real-time safety and performance tests.
#include "TestFramework.h"
#include "TestSignals.h"

#include "Debleed.h"
#include "LinkHub.h"
#include "ReplaceEngine.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>

// --- global allocation counter (counts every operator new in the test binary) ---
namespace
{
    std::atomic<long long> gAllocs { 0 };
}

void* operator new (std::size_t n)
{
    gAllocs.fetch_add (1, std::memory_order_relaxed);
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n)
{
    gAllocs.fetch_add (1, std::memory_order_relaxed);
    if (void* p = std::malloc (n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

using namespace thruma;

namespace
{
    std::unique_ptr<Kit> makeKit (double sr)
    {
        auto kit = std::make_unique<Kit>();
        for (auto inst : { Instrument::Snare, Instrument::Kick })
        {
            VelocityLayer l;
            for (int v = 0; v < 3; ++v)
                l.samples.push_back (SampleData::create ("s", "s", { inst == Instrument::Snare ? sig::snare (sr, (unsigned) v) : sig::kick (sr, (unsigned) v) }, sr));
            kit->slots[(size_t) inst].layers.push_back (l);
            kit->slots[(size_t) inst].finalise();
        }
        return kit;
    }
}

TEST_CASE ("Realtime: no heap allocation inside the audio processing path")
{
    sig::SessionOptions o;
    o.seconds = 6.0;
    const auto s = sig::makeMetalSession (o);
    const auto kit = makeKit (s.sr);
    constexpr int block = 256;

    ReplaceEngine rep;
    rep.prepare (s.sr, block, 2, 10.0f);
    ReplaceSettings rs;
    rs.detector.instrument = Instrument::Snare;
    rep.setSettings (rs);
    DebleedProcessor deb;
    deb.prepare (s.sr, 2);
    DebleedSettings ds;
    ds.useReference = true;
    deb.setSettings (ds);
    auto& hub = LinkHub::getInstance();
    const int linkId = hub.registerSource ("rt-test");

    std::vector<float> L (block), R (block), ref (block), linkBuf (block);
    MidiTrigger midi { 10, (int) Instrument::Kick, 0.7f };

    const long long before = gAllocs.load();
    for (int64_t pos = 0; pos + block < s.length; pos += block)
    {
        std::copy_n (s.mics[sig::Snare].begin() + pos, block, L.begin());
        std::copy_n (s.mics[sig::Snare].begin() + pos, block, R.begin());
        std::copy_n (s.mics[sig::Hat].begin() + pos, block, ref.begin());
        float* io[2] = { L.data(), R.data() };
        hub.write (linkId, pos, L.data(), block);
        hub.read (linkId, pos - block, linkBuf.data(), block);
        deb.process (io, 2, ref.data(), block);
        // parameter changes from the "UI" between blocks must also be allocation-free
        rs.detector.sensitivity = 0.4f + 0.2f * (float) ((pos / block) % 2);
        rep.setSettings (rs);
        ds.amount = 0.5f + 0.1f * (float) ((pos / block) % 3);
        deb.setSettings (ds);
        rep.process (io, 2, block, pos, kit.get(), 1, nullptr, &midi, (pos / block) % 50 == 0 ? 1 : 0, [] (const ReplaceHitInfo&) {});
    }
    const long long allocs = gAllocs.load() - before;
    hub.unregisterSource (linkId);
    tf::metric ("heap allocations during processing", (double) allocs);
    CHECK (allocs == 0);
}

TEST_CASE ("Realtime: CPU load of the full chain (stereo debleed + reference + replacement)")
{
    sig::SessionOptions o;
    o.seconds = 10.0;
    const auto s = sig::makeMetalSession (o);
    const auto kit = makeKit (s.sr);
    constexpr int block = 128;

    ReplaceEngine rep;
    rep.prepare (s.sr, block, 2, 10.0f);
    ReplaceSettings rs;
    rs.detector.instrument = Instrument::Snare;
    rs.mode = ReplaceMode::Layer;
    rep.setSettings (rs);
    DebleedProcessor deb;
    deb.prepare (s.sr, 2);
    DebleedSettings ds;
    ds.useReference = true;
    deb.setSettings (ds);

    std::vector<float> L (block), R (block);
    const auto t0 = std::chrono::steady_clock::now();
    for (int64_t pos = 0; pos + block < s.length; pos += block)
    {
        std::copy_n (s.mics[sig::Snare].begin() + pos, block, L.begin());
        std::copy_n (s.mics[sig::Snare].begin() + pos, block, R.begin());
        float* io[2] = { L.data(), R.data() };
        deb.process (io, 2, s.mics[sig::Hat].data() + pos, block);
        rep.process (io, 2, block, pos, kit.get(), 1, nullptr, nullptr, 0, [] (const ReplaceHitInfo&) {});
    }
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    const double load = secs / o.seconds;
    tf::metric ("real-time load (fraction of one core)", load * 100.0, "%");
    // Generous bound so CI on slow/shared runners or debug builds does not flake; typical is ~1-3 %.
    CHECK (load < 0.5);
}
