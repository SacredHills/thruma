// Dev harness through the real processor: MIDI-file playback on the DAW beat grid, MIDI with no audio input,
// in-plugin preview of imported audio, automatic re-analysis.
#include "../../Source/Plugin/PluginProcessor.h"
#include <random>
using namespace thruma;

// ---- a fake RAVENYR host API (the real one is exported by the RAVENYR executable) ----
static int64_t fakeLocated = -1;
static int fakePlaying = 0;
static float trackSample (int64_t i)   // the "track": snare-like bursts every 0.4 s from 1.0 s
{
    if (i < 48000) return 0.0f;
    const int64_t k = (i - 48000) % 19200;
    uint32_t h = (uint32_t) i * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;   // repeatable noise
    const float noise = (float) (h & 0xffff) / 32768.0f - 1.0f;
    return (float) (0.6 * std::exp (-(double) k / 2400.0)) * noise;
}
extern "C" __attribute__((visibility ("default"))) const RavenyrHostApiV1* ravenyr_host_api_v1()
{
    static const RavenyrHostApiV1 api {
        1, (int32_t) sizeof (RavenyrHostApiV1),
        [] () -> int64_t { return 77; },
        [] (int64_t s) { fakeLocated = s; },
        [] { fakePlaying = 1; }, [] { fakePlaying = 0; },
        [] () -> int32_t { return fakePlaying; },
        [] () -> double { return 48000.0; },
        [] (int64_t key) -> int64_t { return key == 77 ? 48000 * 9 : 0; },
        [] (int64_t key, int64_t start, int64_t n, float* l, float* r) -> int64_t
        {
            if (key != 77) return 0;
            for (int64_t i = 0; i < n; ++i) { l[i] = trackSample (start + i); if (r) r[i] = l[i]; }
            return n;
        },
        [] (int64_t) {}
    };
    return &api;
}
struct PH : juce::AudioPlayHead
{
    int64_t pos = 0; double bpm = 137.0; bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setIsPlaying (playing); p.setTimeInSamples (pos); p.setBpm (bpm);
        p.setPpqPosition (4.0 + (double) pos / 48000.0 * bpm / 60.0);   // song start = beat 4
        return p;
    }
};
static juce::File writeWav (const juce::String& name, std::function<float (int)> f, int len, double rate)
{
    juce::File wav ("/tmp/" + name); wav.deleteFile();
    juce::AudioBuffer<float> b (1, len);
    for (int i = 0; i < len; ++i) b.setSample (0, i, f (i));
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (wav), rate, 1, 24, {}, 0));
    w->writeFromAudioSampleBuffer (b, 0, len);
    return wav;
}
namespace juce::detail { bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages); }
static void pump (int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
        if (! juce::detail::dispatchNextMessageOnSystemQueue (true)) juce::Thread::sleep (2);
}
int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    int fails = 0;
    auto check = [&] (bool ok, const juce::String& what) { std::printf ("  [%s] %s\n", ok ? "ok" : "FAIL", what.toRawUTF8()); if (! ok) ++fails; };

    std::unique_ptr<ThrumaProcessor> proc (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
    PH ph; proc->setPlayHead (&ph); proc->prepareToPlay (48000.0, 512);
    // a kit sample: a click at sample 0 (so its onset is exact)
    auto click = writeWav ("thruma_click.wav", [] (int i) { return i < 4 ? 0.9f : 0.0f; }, 4800, 48000.0);
    proc->getKitManager().assignFiles ((int) Instrument::Kick, { click });
    proc->rebuildKit();
    if (auto* b = proc->apvts.getParameter (ids::repBlend)) b->setValueNotifyingHost (1.0f);
    pump (100);
    const int L = proc->getLatencySamples();

    // ---- 1) MIDI file on the beat grid, no audio input ----
    {
        juce::MidiMessageSequence seq;
        for (int k = 0; k < 8; ++k) seq.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 100), (double) (480 * (6 + k)));   // beats 6..13
        juce::MidiFile mf; mf.setTicksPerQuarterNote (480); mf.addTrack (seq);
        juce::File mid ("/tmp/thruma_test_in.mid"); mid.deleteFile();
        { juce::FileOutputStream os (mid); mf.writeTo (os); }
        juce::String msg;
        check (proc->importMidiFile (mid, msg), "MIDI file imported: " + msg);

        std::vector<int64_t> onsets;
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        for (int64_t pos = 0; pos < 6 * 48000; pos += 512)
        {
            ph.pos = pos; buf.clear(); midi.clear();
            proc->processBlock (buf, midi);
            for (int i = 0; i < 512; ++i)
                if (std::abs (buf.getSample (0, i)) > 0.3f && (onsets.empty() || pos + i - onsets.back() > 100)) onsets.push_back (pos + i);
        }
        double maxErr = 0;
        for (size_t k = 0; k < onsets.size(); ++k)
        {
            // the host plays our output L samples earlier (latency compensation): heard at onset - L
            const double heard = (double) (onsets[k] - L);
            const double want = ((6.0 + (double) k) - 4.0) * 60.0 / ph.bpm * 48000.0;
            maxErr = std::max (maxErr, std::abs (heard - want));
        }
        check (onsets.size() == 8, "MIDI file notes play the kit: " + juce::String ((int) onsets.size()) + " of 8");
        check (maxErr <= 1.0, "MIDI file notes land on their beats after latency compensation: max error " + juce::String (maxErr, 2) + " samples");
        proc->clearMidiFile();

        // live MIDI notes with no audio input (Thruma on a MIDI / instrument track)
        int heard = 0;
        for (int blk = 0; blk < 20; ++blk)
        {
            buf.clear(); midi.clear();
            if (blk == 2) midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 110), 100);
            proc->processBlock (buf, midi);
            if (buf.getMagnitude (0, 0, 512) > 0.3f) ++heard;
        }
        check (heard >= 1, "MIDI notes from a MIDI track play the kit with silent input");
    }

    // ---- 2) Import audio: analysis, in-plugin preview with the DAW stopped, ruler locate ----
    {
        std::mt19937 rng (5); std::uniform_real_distribution<float> u (-1, 1);
        auto wav = writeWav ("thruma_import.wav", [&] (int i) { const double t = std::fmod ((double) i / 48000.0, 0.3); return (float) (0.5 * std::exp (-t / 0.05)) * u (rng); },
                             48000 * 6, 48000.0);
        juce::String result;
        proc->importAudio (wav, [&] (const juce::String& m) { result = m; });
        for (int i = 0; i < 100 && result.isEmpty(); ++i) pump (50);
        check (result.contains ("20 notes"), "import: " + result);
        check (proc->getOfflineSource() == ThrumaProcessor::OfflineSource::File && proc->isFinalized(), "imported audio is finalized and can be previewed");

        ph.playing = false; ph.pos = 0;
        check (proc->locateTo (48000 * 2), "ruler click sets the preview position");
        proc->togglePlay();
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        float pk = 0;
        for (int blk = 0; blk < 94; ++blk) { buf.clear(); midi.clear(); proc->processBlock (buf, midi); pk = std::max (pk, buf.getMagnitude (0, 0, 512)); }
        check (pk > 0.3f, "preview plays (replacement samples on the imported drum) while the DAW is stopped: peak " + juce::String (pk, 2));
        check (std::abs (proc->getPlayheadPosition() - (48000 * 3 - 512)) < 1024, "preview starts at the clicked position: playhead " + juce::String (proc->getPlayheadPosition()));
        proc->togglePlay();
        check (! proc->isPreviewPlaying(), "stop");

        // automatic re-analysis on a settings change, hand-edited notes kept
        auto list = proc->getTriggers().getAll();
        Trigger extra; extra.position = 48000 * 5 + 7000; extra.velocity = 0.5f; extra.id = 999999;
        list.push_back (extra);
        std::sort (list.begin(), list.end(), [] (auto& a, auto& b) { return a.position < b.position; });
        proc->commitTriggerEdit (list, "add", true);
        const int before = proc->getOfflineMessageCount();
        if (auto* sens = proc->apvts.getParameter (ids::repSens)) sens->setValueNotifyingHost (0.9f);
        for (int i = 0; i < 60 && proc->getOfflineMessageCount() == before; ++i) pump (50);
        check (proc->getOfflineMessageCount() > before, "changing Sensitivity re-analysed the audio: " + proc->getOfflineMessage());
        bool kept = false;
        for (auto& t : proc->getTriggers().getAll()) if (t.position == extra.position && t.locked) kept = true;
        check (kept, "a hand-added note survives re-analysis");

        proc->setFinalized (false);
        pump (200);
        check (proc->getOfflineSource() == ThrumaProcessor::OfflineSource::None, "un-finalizing returns to live detection");
    }
    // ---- 3) host extension (RAVENYR): Analyze reads the whole track, the ruler moves the host's play point ----
    {
        std::unique_ptr<ThrumaProcessor> p2 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH ph2; ph2.playing = false; p2->setPlayHead (&ph2); p2->prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        buf.clear(); p2->processBlock (buf, midi);   // the host tells the plugin its track while processing
        check (p2->canReadHostTrack(), "plugin found the host API and its track");
        juce::String result;
        p2->analyseTrack ([&] (const juce::String& m) { result = m; });
        for (int i = 0; i < 100 && result.isEmpty(); ++i) pump (50);
        int good = 0;
        for (auto& t : p2->getTriggers().getAll())
        {
            const int64_t k = (t.position - 48000) % 19200;
            if (t.position >= 48000 && (k < 48 || k > 19200 - 48)) ++good;
        }
        check (p2->getOfflineSource() == ThrumaProcessor::OfflineSource::HostTrack, "Analyze read the whole track from the host: " + result);
        check (good >= 19 && (int) p2->getTriggers().size() <= 21, "whole-track analysis found the hits: " + juce::String (good) + " on time of " + juce::String ((int) p2->getTriggers().size()));
        check (p2->locateTo (48000 * 4) && fakeLocated == 48000 * 4, "ruler click moves the host's play position");
        p2->togglePlay();
        check (fakePlaying == 1, "Play starts the host transport");
    }

    std::printf (fails == 0 ? "ALL HARNESS CHECKS PASSED\n" : "HARNESS FAILURES: %d\n", fails);
    proc.reset();
    return fails == 0 ? 0 : 1;
}
