#include "Triggers.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace thruma
{

//==============================================================================
void TriggerList::sort()
{
    std::stable_sort (triggers.begin(), triggers.end(),
                      [] (const Trigger& a, const Trigger& b) { return a.position < b.position; });
}

uint32_t TriggerList::add (int64_t position, float velocity, bool ghost)
{
    Trigger t;
    t.position = std::max<int64_t> (0, position);
    t.velocity = std::clamp (velocity, 0.0f, 1.0f);
    t.ghost = ghost;
    t.id = nextId++;
    triggers.insert (triggers.begin() + (long) lowerBound (t.position), t);
    return t.id;
}

void TriggerList::addWithId (const Trigger& t)
{
    triggers.insert (triggers.begin() + (long) lowerBound (t.position), t);
    nextId = std::max (nextId, t.id + 1);
}

bool TriggerList::remove (uint32_t id)
{
    auto it = std::find_if (triggers.begin(), triggers.end(), [id] (const Trigger& t) { return t.id == id; });
    if (it == triggers.end()) return false;
    triggers.erase (it);
    return true;
}

bool TriggerList::move (uint32_t id, int64_t newPosition)
{
    auto it = std::find_if (triggers.begin(), triggers.end(), [id] (const Trigger& t) { return t.id == id; });
    if (it == triggers.end()) return false;
    it->position = std::max<int64_t> (0, newPosition);
    sort();
    return true;
}

bool TriggerList::setVelocity (uint32_t id, float velocity)
{
    for (auto& t : triggers)
        if (t.id == id) { t.velocity = std::clamp (velocity, 0.0f, 1.0f); return true; }
    return false;
}

const Trigger* TriggerList::find (uint32_t id) const noexcept
{
    for (auto& t : triggers)
        if (t.id == id) return &t;
    return nullptr;
}

const Trigger* TriggerList::findNearest (int64_t position, int64_t tolerance) const noexcept
{
    const auto i = lowerBound (position);
    const Trigger* best = nullptr;
    int64_t bestD = tolerance + 1;
    for (size_t j = (i > 0 ? i - 1 : 0); j < std::min (triggers.size(), i + 1); ++j)
    {
        const int64_t d = std::llabs (triggers[j].position - position);
        if (d < bestD) { bestD = d; best = &triggers[j]; }
    }
    return best;
}

void TriggerList::setAll (std::vector<Trigger> t)
{
    triggers = std::move (t);
    for (auto& x : triggers)
        if (x.id == 0) x.id = nextId++;
        else nextId = std::max (nextId, x.id + 1);
    sort();
}

size_t TriggerList::lowerBound (int64_t p) const noexcept
{
    return (size_t) (std::lower_bound (triggers.begin(), triggers.end(), p,
                                       [] (const Trigger& t, int64_t v) { return t.position < v; }) - triggers.begin());
}

size_t TriggerSnapshot::lowerBound (int64_t p) const noexcept
{
    return (size_t) (std::lower_bound (triggers.begin(), triggers.end(), p,
                                       [] (const Trigger& t, int64_t v) { return t.position < v; }) - triggers.begin());
}

// Binary format: "THT1" u32 count, then per trigger: i64 pos, f32 vel, u8 ghost, u32 id (little endian).
std::vector<uint8_t> TriggerList::serialise() const
{
    std::vector<uint8_t> out;
    out.reserve (8 + triggers.size() * 17);
    auto put = [&out] (const void* p, size_t n)
    {
        auto* b = static_cast<const uint8_t*> (p);
        out.insert (out.end(), b, b + n);  // host is little-endian on all supported targets
    };
    out.insert (out.end(), { 'T', 'H', 'T', '1' });
    const auto count = (uint32_t) triggers.size();
    put (&count, 4);
    for (auto& t : triggers)
    {
        put (&t.position, 8);
        put (&t.velocity, 4);
        const uint8_t g = t.ghost ? 1 : 0;
        put (&g, 1);
        put (&t.id, 4);
    }
    return out;
}

bool TriggerList::deserialise (const uint8_t* data, size_t size)
{
    if (size < 8 || std::memcmp (data, "THT1", 4) != 0) return false;
    uint32_t count;
    std::memcpy (&count, data + 4, 4);
    if (size < 8 + (size_t) count * 17) return false;
    std::vector<Trigger> v;
    v.reserve (count);
    const uint8_t* p = data + 8;
    for (uint32_t i = 0; i < count; ++i)
    {
        Trigger t;
        std::memcpy (&t.position, p, 8); p += 8;
        std::memcpy (&t.velocity, p, 4); p += 4;
        t.ghost = *p++ != 0;
        std::memcpy (&t.id, p, 4); p += 4;
        v.push_back (t);
    }
    setAll (std::move (v));
    return true;
}

//==============================================================================
std::vector<Trigger> analyseTake (const float* audio, int64_t length, double sampleRate,
                                  const DetectorSettings& settings, float* outVelocityRefDb,
                                  SpectralTemplate* outTemplate)
{
    TransientDetector det;
    det.prepare (sampleRate);
    det.setSettings (settings);

    // pass 1: adaptive reference, collect peaks
    det.reset (0);
    std::vector<DetectedHit> hits;
    constexpr int block = 4096;
    for (int64_t s = 0; s < length; s += block)
        det.process (audio + s, (int) std::min<int64_t> (block, length - s), [&] (const DetectedHit& h) { hits.push_back (h); });

    float ref = -12.0f;
    SpectralTemplate tmpl;
    if (! hits.empty())
    {
        std::vector<float> peaks;
        for (auto& h : hits) peaks.push_back (h.peakDb);
        std::sort (peaks.begin(), peaks.end());
        ref = peaks[std::min (peaks.size() - 1, (size_t) (peaks.size() * 0.98))];

        // fingerprint from the confident (loud) hits of the whole take
        for (auto& h : hits)
        {
            if (h.peakDb < ref - 6.0f) continue;
            for (int i = 0; i < SpectralTemplate::kBands; ++i) tmpl.shape[(size_t) i] += h.shape[(size_t) i];
            ++tmpl.count;
        }
        if (tmpl.count > 0)
            for (auto& v : tmpl.shape) v /= (float) tmpl.count;
        tmpl.learnedAtRefDb = ref;
    }

    // pass 2: fixed take reference and fingerprint -> consistent velocities and decisions over the whole take
    det.setVelocityReference (ref);
    det.setSpectralTemplate (tmpl);
    det.reset (0);
    std::vector<Trigger> out;
    for (int64_t s = 0; s < length; s += block)
        det.process (audio + s, (int) std::min<int64_t> (block, length - s), [&] (const DetectedHit& h)
        {
            Trigger t;
            t.position = h.position;
            t.velocity = h.velocity;
            t.ghost = h.ghost;
            out.push_back (t);
        });
    if (outVelocityRefDb != nullptr) *outVelocityRefDb = ref;
    if (outTemplate != nullptr) *outTemplate = tmpl;
    return out;
}

//==============================================================================
namespace
{
    void putVarLen (std::vector<uint8_t>& o, uint32_t v)
    {
        uint8_t buf[5];
        int n = 0;
        buf[n++] = (uint8_t) (v & 0x7F);
        while ((v >>= 7) != 0) buf[n++] = (uint8_t) ((v & 0x7F) | 0x80);
        while (n > 0) o.push_back (buf[--n]);
    }
    void putBE32 (std::vector<uint8_t>& o, uint32_t v)
    {
        for (int s = 24; s >= 0; s -= 8) o.push_back ((uint8_t) (v >> s));
    }
    void putBE16 (std::vector<uint8_t>& o, uint16_t v)
    {
        o.push_back ((uint8_t) (v >> 8));
        o.push_back ((uint8_t) v);
    }
}

std::vector<uint8_t> writeMidiFile (std::vector<MidiNoteEvent> notes, double sampleRate, double bpm,
                                    int ppq, double noteLen, const std::string& trackName)
{
    struct Ev { int64_t tick; int order; uint8_t a, b, c; };
    std::vector<Ev> evs;
    const double ticksPerSample = (bpm / 60.0) * ppq / sampleRate;
    const auto lenTicks = (int64_t) std::max (1.0, noteLen * sampleRate * ticksPerSample);
    for (auto& n : notes)
    {
        const auto t = (int64_t) std::llround ((double) n.samplePosition * ticksPerSample);
        const uint8_t note = (uint8_t) std::clamp (n.note, 0, 127);
        const uint8_t vel = (uint8_t) std::clamp (n.velocity, 1, 127);
        evs.push_back ({ t, 1, 0x99, note, vel });           // note on, channel 10
        evs.push_back ({ t + lenTicks, 0, 0x89, note, 0 });   // note off first at equal ticks
    }
    std::stable_sort (evs.begin(), evs.end(), [] (const Ev& x, const Ev& y)
                      { return x.tick != y.tick ? x.tick < y.tick : x.order < y.order; });

    std::vector<uint8_t> track;
    // tempo + time signature + name
    putVarLen (track, 0);
    track.insert (track.end(), { 0xFF, 0x03, (uint8_t) std::min<size_t> (trackName.size(), 127) });
    track.insert (track.end(), trackName.begin(), trackName.begin() + (long) std::min<size_t> (trackName.size(), 127));
    const auto usPerQuarter = (uint32_t) std::llround (60000000.0 / std::max (1.0, bpm));
    putVarLen (track, 0);
    track.insert (track.end(), { 0xFF, 0x51, 0x03, (uint8_t) (usPerQuarter >> 16), (uint8_t) (usPerQuarter >> 8), (uint8_t) usPerQuarter });
    putVarLen (track, 0);
    track.insert (track.end(), { 0xFF, 0x58, 0x04, 4, 2, 24, 8 });

    int64_t last = 0;
    for (auto& e : evs)
    {
        putVarLen (track, (uint32_t) std::max<int64_t> (0, e.tick - last));
        last = e.tick;
        track.insert (track.end(), { e.a, e.b, e.c });
    }
    putVarLen (track, 0);
    track.insert (track.end(), { 0xFF, 0x2F, 0x00 });

    std::vector<uint8_t> out;
    out.insert (out.end(), { 'M', 'T', 'h', 'd' });
    putBE32 (out, 6);
    putBE16 (out, 0);    // format 0 (single track)
    putBE16 (out, 1);
    putBE16 (out, (uint16_t) ppq);
    out.insert (out.end(), { 'M', 'T', 'r', 'k' });
    putBE32 (out, (uint32_t) track.size());
    out.insert (out.end(), track.begin(), track.end());
    return out;
}

} // namespace thruma
