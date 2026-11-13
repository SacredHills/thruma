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

    // ---- 4) instrument with the main input switched off (Studio One): both channels, sidechain as the drum ----
    {
        std::unique_ptr<ThrumaProcessor> p3 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        auto layout = p3->getBusesLayout();
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::disabled();
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        check (p3->setBusesLayout (layout), "layout without main input accepted");
        PH ph3; ph3.playing = true; p3->setPlayHead (&ph3); p3->prepareToPlay (48000.0, 512);
        p3->getKitManager().assignFiles ((int) Instrument::Snare, { click });
        p3->getKitManager().assignFiles ((int) Instrument::Kick, { click });
        p3->rebuildKit();
        pump (100);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        float l = 0, r = 0;
        for (int blk = 0; blk < 20; ++blk)
        {
            buf.clear(); midi.clear(); ph3.pos = blk * 512;
            if (blk == 2) midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 110), 100);
            p3->processBlock (buf, midi);
            l = std::max (l, buf.getMagnitude (0, 0, 512)); r = std::max (r, buf.getMagnitude (1, 0, 512));
        }
        check (l > 0.3f && std::abs (l - r) < 0.01f, "no main input: MIDI plays on both channels (L " + juce::String (l, 2) + ", R " + juce::String (r, 2) + ")");

        // sidechain drum (in the first channels) triggers the snare, on both sides
        std::mt19937 rng (3); std::uniform_real_distribution<float> u (-1, 1);
        int hitsL = 0; float rr = 0;
        for (int64_t pos = 0; pos < 48000 * 4; pos += 512)
        {
            ph3.pos = 20 * 512 + pos; midi.clear();
            for (int i = 0; i < 512; ++i)
            {
                const double t = std::fmod ((double) (pos + i) / 48000.0, 0.5);
                const float v = (float) (0.5 * std::exp (-t / 0.04)) * u (rng);
                buf.setSample (0, i, v); buf.setSample (1, i, v);
            }
            p3->processBlock (buf, midi);
            rr = std::max (rr, buf.getMagnitude (1, 0, 512));
        }
        pump (300);
        hitsL = (int) p3->getTriggers().size();
        check (hitsL >= 7, "sidechain drum is tracked when there is no main input: " + juce::String (hitsL) + " hits");
        check (rr > 0.3f, "and the right channel carries the replacement");
    }

    // ---- 5) blend: 0 % = original only, 100 % = samples only ----
    {
        std::unique_ptr<ThrumaProcessor> p4 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH ph4; p4->setPlayHead (&ph4); p4->prepareToPlay (48000.0, 512);
        auto tone = writeWav ("thruma_tone.wav", [] (int i) { return 0.8f * std::sin ((float) i * 0.05f) * std::exp (-(float) i / 4000.0f); }, 24000, 48000.0);
        p4->getKitManager().assignFiles ((int) Instrument::Snare, { tone });
        p4->rebuildKit();
        auto runWith = [&] (float blend) -> std::pair<double, double>   // (correlation with dry, energy)
        {
            p4->apvts.getParameter (ids::repBlend)->setValueNotifyingHost (blend);
            pump (50);
            std::mt19937 rng (9); std::uniform_real_distribution<float> u (-1, 1);
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
            std::vector<float> in, out;
            for (int64_t pos = 0; pos < 48000 * 6; pos += 512)
            {
                ph4.pos = pos + (int64_t) (blend * 1.0e7);
                for (int i = 0; i < 512; ++i)
                {
                    const double t = std::fmod ((double) (pos + i) / 48000.0, 0.5);
                    const float v = (float) (0.5 * std::exp (-t / 0.04)) * u (rng);
                    buf.setSample (0, i, v); buf.setSample (1, i, v); in.push_back (v);
                }
                midi.clear(); p4->processBlock (buf, midi);
                for (int i = 0; i < 512; ++i) out.push_back (buf.getSample (0, i));
            }
            const int L = p4->getLatencySamples();
            double xy = 0, xx = 0, yy = 0;
            for (size_t i = 48000 * 3; i + L < out.size(); ++i) { xy += in[i] * out[i + L]; xx += in[i] * in[i]; yy += out[i + L] * out[i + L]; }
            return { xy / std::sqrt (xx * yy + 1e-12), yy };
        };
        const auto b0 = runWith (0.0f), b1 = runWith (1.0f), bh = runWith (0.5f);
        check (b0.first > 0.99, "blend 0 %: output is the original (correlation " + juce::String (b0.first, 3) + ")");
        check (b1.first < 0.2, "blend 100 %: the original is gone (correlation " + juce::String (b1.first, 3) + ")");
        check (bh.first > b1.first + 0.2 && bh.first < b0.first - 0.05, "blend 50 %: in between (correlation " + juce::String (bh.first, 3) + ")");
    }

    // ---- 6) stopped host: the playhead stays put ----
    {
        std::unique_ptr<ThrumaProcessor> p5 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH ph5; ph5.playing = true; ph5.pos = 0; p5->setPlayHead (&ph5); p5->prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        buf.clear(); p5->processBlock (buf, midi);          // the host has played once
        ph5.playing = false; ph5.pos = 96000;
        for (int blk = 0; blk < 50; ++blk) { for (int i = 0; i < 512; ++i) { buf.setSample (0, i, 0.3f * std::sin (i * 0.3f)); buf.setSample (1, i, 0.3f * std::sin (i * 0.3f)); } p5->processBlock (buf, midi); }
        check (! p5->isTransportPlaying() && p5->getPlayheadPosition() == 96000, "stopped host with audio coming in: not playing, playhead at the host cursor (" + juce::String (p5->getPlayheadPosition()) + ")");
        // a host that keeps its position running while stopped, after it has reported play once
        ph5.playing = true; buf.clear(); p5->processBlock (buf, midi);
        ph5.playing = false;
        for (int blk = 0; blk < 50; ++blk) { ph5.pos += 512; buf.clear(); p5->processBlock (buf, midi); }
        check (! p5->isTransportPlaying(), "position running while the host says stopped: treated as stopped");
    }

    // ---- 7) turning Debleed on keeps the waveform and keeps drawing it ----
    for (int refChoice : { 0, 1 })   // 0 = no reference, 1 = sidechain L
    {
        std::unique_ptr<ThrumaProcessor> p6 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH ph6; ph6.playing = true; p6->setPlayHead (&ph6); p6->prepareToPlay (48000.0, 512);
        if (refChoice) { auto* rp = p6->apvts.getParameter (ids::dbReference); rp->setValueNotifyingHost (rp->convertTo0to1 (1.0f)); pump (100); }
        std::mt19937 rng (4); std::uniform_real_distribution<float> u (-1, 1);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        auto playFor = [&] (int64_t from, int64_t to)
        {
            for (int64_t pos = from; pos < to; pos += 512)
            {
                ph6.pos = pos;
                for (int i = 0; i < 512; ++i)
                {
                    const double t = std::fmod ((double) (pos + i) / 48000.0, 0.5);
                    const float v = (float) (0.5 * std::exp (-t / 0.04)) * u (rng) + 0.002f * u (rng);
                    buf.setSample (0, i, v); buf.setSample (1, i, v);
                }
                midi.clear(); p6->processBlock (buf, midi);
            }
        };
        auto peakDb = [&] (int64_t from, int64_t to)
        {
            const auto& env = p6->getEnvelopeLevel();
            float pk = -200.0f;
            for (int64_t b = from / 64; b < to / 64 && b < (int64_t) env.size(); ++b) pk = std::max (pk, env[(size_t) b]);
            return pk;
        };
        playFor (0, 48000 * 4);
        const float before = peakDb (0, 48000 * 4);
        p6->apvts.getParameter (ids::dbOn)->setValueNotifyingHost (1.0f);
        pump (200);
        p6->releaseResources(); p6->prepareToPlay (48000.0, 512);   // hosts re-prepare when the latency changes
        playFor (48000 * 4, 48000 * 8);
        const float kept = peakDb (0, 48000 * 4), after = peakDb (48000 * 5, 48000 * 8);
        const juce::String tag = refChoice ? " (sidechain reference)" : " (no reference)";
        check (kept > before - 1.0f, "Debleed on: the waveform recorded before is kept" + tag + ": " + juce::String (kept, 1) + " dB (was " + juce::String (before, 1) + ")");
        check (after > before - 12.0f, "Debleed on: the waveform keeps being drawn" + tag + ": peak " + juce::String (after, 1) + " dB");
    }

    std::printf (fails == 0 ? "ALL HARNESS CHECKS PASSED\n" : "HARNESS FAILURES: %d\n", fails);
    proc.reset();
    return fails == 0 ? 0 : 1;
}
