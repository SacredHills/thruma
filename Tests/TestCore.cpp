#include "TestFramework.h"
#include "TestSignals.h"

#include "Debleed.h"
#include "FFT.h"
#include "LinkHub.h"
#include "Rcu.h"
#include "Resampler.h"
#include "Triggers.h"

#include <complex>
#include <cstring>

using namespace thruma;

TEST_CASE ("FFT: matches naive DFT and round-trips")
{
    for (int n : { 16, 64, 1024, 4096 })
    {
        RealFFT fft;
        fft.prepare (n);
        sig::Rng r (n);
        std::vector<float> x ((size_t) n), y ((size_t) n);
        for (auto& v : x) v = r.uni();
        std::vector<std::complex<float>> X ((size_t) n / 2 + 1);
        fft.forward (x.data(), X.data());

        if (n <= 64)
        {
            double maxErr = 0.0;
            for (int k = 0; k <= n / 2; ++k)
            {
                std::complex<double> acc;
                for (int i = 0; i < n; ++i)
                    acc += (double) x[(size_t) i] * std::exp (std::complex<double> (0.0, -2.0 * kPi * k * i / n));
                maxErr = std::max (maxErr, std::abs (acc - std::complex<double> (X[(size_t) k])));
            }
            CHECK (maxErr < 1e-4);
        }

        fft.inverse (X.data(), y.data());
        double err = 0.0;
        for (int i = 0; i < n; ++i) err = std::max (err, (double) std::abs (x[(size_t) i] - y[(size_t) i]));
        CHECK (err < 1e-5);
    }
}

TEST_CASE ("Resampler: 44.1k -> 48k and 96k -> 44.1k preserve a sine with > 80 dB SNR")
{
    for (auto rates : { std::pair<double, double> { 44100.0, 48000.0 }, { 96000.0, 44100.0 }, { 48000.0, 96000.0 } })
    {
        const double inR = rates.first, outR = rates.second, f = 1000.0;
        std::vector<float> in ((size_t) (inR * 0.5));
        for (size_t i = 0; i < in.size(); ++i) in[i] = (float) (0.5 * std::sin (2.0 * kPi * f * i / inR));
        const auto out = resampleHighQuality (in, inR, outR);
        CHECK_NEAR ((double) out.size(), in.size() * outR / inR, 2.0);

        double sig = 0, err = 0;
        for (size_t i = 2000; i + 2000 < out.size(); ++i)
        {
            const double ref = 0.5 * std::sin (2.0 * kPi * f * i / outR);
            sig += ref * ref;
            err += (out[i] - ref) * (out[i] - ref);
        }
        const double snr = 10.0 * std::log10 (sig / (err + 1e-30));
        tf::metric ("resampler SNR (dB)", snr, "dB");
        CHECK (snr > 80.0);
    }
}

TEST_CASE ("Instruments: names map to instruments")
{
    CHECK (guessInstrumentFromName ("Kick_In_03.wav") == Instrument::Kick);
    CHECK (guessInstrumentFromName ("SNARE top v4.wav") == Instrument::Snare);
    CHECK (guessInstrumentFromName ("FloorTom_1.wav") == Instrument::Tom4);
    CHECK (guessInstrumentFromName ("china_18.wav") == Instrument::China);
    CHECK (guessInstrumentFromName ("Ride Bell.wav") == Instrument::Ride);
    CHECK (guessInstrumentFromName ("HiHat closed.wav") == Instrument::HiHat);
    CHECK (guessInstrumentFromName ("perc_01.wav") == Instrument::Count);
}

TEST_CASE ("Triggers: edit operations, serialisation round-trip")
{
    TriggerList list;
    const auto a = list.add (1000, 0.5f);
    const auto b = list.add (500, 0.9f, true);
    const auto c = list.add (2000, 0.2f);
    CHECK (list.getAll()[0].id == b);
    CHECK (list.move (a, 3000));
    CHECK (list.getAll().back().id == a);
    CHECK (list.setVelocity (c, 0.75f));
    CHECK (list.remove (b));
    CHECK (list.size() == 2);
    CHECK (list.findNearest (2010, 20) != nullptr && list.findNearest (2010, 20)->id == c);
    CHECK (list.findNearest (2500, 20) == nullptr);

    const auto bytes = list.serialise();
    TriggerList restored;
    CHECK (restored.deserialise (bytes.data(), bytes.size()));
    CHECK (restored.getAll() == list.getAll());
    // ids continue after restore
    const auto d = restored.add (10, 1.0f);
    CHECK (d > std::max (a, c));
    CHECK (! restored.deserialise (bytes.data(), 5));
}

TEST_CASE ("MIDI: file structure, tick mapping and velocities")
{
    std::vector<MidiNoteEvent> notes { { 48000, 38, 100 }, { 0, 36, 127 }, { 24000, 42, 1 } };
    const auto bytes = writeMidiFile (notes, 48000.0, 120.0, 960);
    CHECK (bytes.size() > 22);
    CHECK (std::memcmp (bytes.data(), "MThd", 4) == 0);
    CHECK (std::memcmp (bytes.data() + 14, "MTrk", 4) == 0);
    const uint32_t trackLen = (uint32_t) bytes[18] << 24 | (uint32_t) bytes[19] << 16 | (uint32_t) bytes[20] << 8 | bytes[21];
    CHECK (trackLen == bytes.size() - 22);

    // walk the track and collect note-ons with absolute ticks
    size_t p = 22;
    int64_t tick = 0;
    std::vector<std::pair<int64_t, int>> ons;
    std::vector<int> vels;
    while (p < bytes.size())
    {
        uint32_t dt = 0;
        uint8_t b;
        do { b = bytes[p++]; dt = (dt << 7) | (b & 0x7F); } while (b & 0x80);
        tick += dt;
        const uint8_t st = bytes[p++];
        if (st == 0xFF) { const uint8_t len = bytes[p + 1]; p += 2 + len; continue; }
        const uint8_t d1 = bytes[p++], d2 = bytes[p++];
        if ((st & 0xF0) == 0x90 && d2 > 0) { ons.push_back ({ tick, d1 }); vels.push_back (d2); }
    }
    CHECK (ons.size() == 3);
    if (ons.size() == 3)
    {
        CHECK (ons[0].first == 0 && ons[0].second == 36);
        CHECK (ons[1].first == 960 && ons[1].second == 42);    // 0.5 s @120 BPM = 1 beat
        CHECK (ons[2].first == 1920 && ons[2].second == 38);
        CHECK (vels[0] == 127 && vels[1] == 1 && vels[2] == 100);
    }
    CHECK (velocityToMidi (0.0f) == 1 && velocityToMidi (1.0f) == 127 && velocityToMidi (0.5f) == 64);
}

TEST_CASE ("LinkHub: timeline-aligned sharing and discontinuity handling")
{
    auto& hub = LinkHub::getInstance();
    const int id = hub.registerSource ("test-snare");
    CHECK (id >= 0);
    CHECK (hub.findSource ("test-snare") == id);

    std::vector<float> block (256), out (256);
    for (int b = 0; b < 8; ++b)
    {
        for (int i = 0; i < 256; ++i) block[(size_t) i] = (float) (b * 256 + i);
        hub.write (id, 10000 + b * 256, block.data(), 256);
    }
    CHECK (hub.read (id, 10000 + 300, out.data(), 256));
    CHECK (out[0] == 300.0f && out[255] == 555.0f);
    // future data is not available -> zeros and 'false'
    CHECK (! hub.read (id, 10000 + 8 * 256 - 100, out.data(), 256));
    CHECK (out[99] == (float) (8 * 256 - 1) && out[100] == 0.0f);
    // timeline jump: old segment no longer readable
    hub.write (id, 500000, block.data(), 256);
    CHECK (! hub.read (id, 10000, out.data(), 64));
    CHECK (hub.read (id, 500000, out.data(), 256));
    hub.unregisterSource (id);
    CHECK (hub.findSource ("test-snare") == -1);
}

TEST_CASE ("RCU: objects are freed only after the audio side moves on")
{
    struct Obj { int v; };
    RcuPublisher<Obj> pub;
    pub.publish (std::make_unique<Obj> (Obj { 1 }));
    auto* h1 = pub.acquire();
    pub.reportOldestInUse (h1->seq);
    pub.publish (std::make_unique<Obj> (Obj { 2 }));
    pub.publish (std::make_unique<Obj> (Obj { 3 }));
    CHECK (pub.getNumRetained() == 3);          // audio still holds #1
    auto* h3 = pub.acquire();
    CHECK (h3->object->v == 3);
    pub.reportOldestInUse (h3->seq);
    pub.collectGarbage();
    CHECK (pub.getNumRetained() == 1);
}

TEST_CASE ("Debleed model: remapping between sample rates keeps values per frequency")
{
    DebleedModel m;
    m.sampleRate = 48000.0;
    m.fftSize = 1024;
    const int bins = m.getNumBins();
    for (int k = 0; k < bins; ++k)
    {
        m.transfer.push_back ((float) k / bins);
        m.bleedFloor.push_back (1.0f);
        m.decayTau.push_back (0.1f + (float) k / bins);
    }
    const auto r = m.remapped (96000.0, 2048);
    CHECK (r.getNumBins() == 1025 && (int) r.transfer.size() == 1025);
    // 1 kHz -> bin ~21.3 @48k/1024 and ~21.3 @96k/2048
    const int k48 = (int) (1000.0 * 1024 / 48000.0), k96 = (int) (1000.0 * 2048 / 96000.0);
    CHECK_NEAR (r.decayTau[(size_t) k96], m.decayTau[(size_t) k48], 0.01);
    CHECK_NEAR (r.bleedFloor[10], 2.0, 1e-3);   // noise power per bin scales with N
}
