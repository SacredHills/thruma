// Dev harness through the real processor: MIDI-file playback on the DAW beat grid, MIDI with no audio input,
// in-plugin preview of imported audio, automatic re-analysis.
#include "../../Source/Plugin/PluginProcessor.h"
#include "../../Source/Plugin/UI/TrackerLane.h"
#include "../../Source/Plugin/UI/SamplesPage.h"
#include "../../Source/Plugin/UI/PlayerPage.h"
#include <random>
#include <set>
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
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        buf.clear(); midi.clear(); proc->processBlock (buf, midi);   // the DAW has stopped
        check (proc->locateTo (48000 * 2).isEmpty(), "ruler click sets the preview position");
        proc->togglePlay();
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
        buf.clear(); midi.clear(); p2->processBlock (buf, midi);   // the host tells the plugin its track while processing
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
        check (p2->locateTo (48000 * 4).isEmpty() && fakeLocated == 48000 * 4, "ruler click moves the host's play position");
        p2->togglePlay();
        check (fakePlaying == 1, "Play starts the host transport");
    }

    // ---- 3b) noise from a plugin before Thruma while the DAW is stopped must not wipe the waveform ----
    {
        auto wavB = writeWav ("thruma_noise_test.wav", [] (int i) { return (i % 24000) < 400 ? 0.8f * std::sin ((float) i * 0.3f) * std::exp (-(float) (i % 24000) / 120.0f) : 0.0f; }, 48000 * 6, 48000.0);
        std::unique_ptr<ThrumaProcessor> pn (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH phn; phn.playing = false; phn.pos = 48000; pn->setPlayHead (&phn); pn->prepareToPlay (48000.0, 512);
        juce::String result;
        pn->importAudio (wavB, [&] (const juce::String& m) { result = m; });
        for (int i = 0; i < 100 && result.isEmpty(); ++i) pump (50);
        const auto len0 = pn->getCapture().length.load();
        const auto org0 = pn->getCapture().origin.load();
        std::vector<float> before (pn->getCapture().data[0].begin(), pn->getCapture().data[0].begin() + len0);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        juce::Random rnd (3);
        auto feed = [&] (int blocks, bool play)
        {
            phn.playing = play;
            for (int b = 0; b < blocks; ++b)
            {
                for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) buf.setSample (c, i, 0.002f * (rnd.nextFloat() * 2 - 1));   // -54 dB hiss
                midi.clear(); pn->processBlock (buf, midi);
                if (play) phn.pos += 512;
            }
        };
        feed (300, false);   // ~3 s stopped, with a noisy plugin before Thruma
        bool same = pn->getCapture().length.load() == len0 && pn->getCapture().origin.load() == org0;
        for (int64_t i = 0; same && i < len0; i += 7) same = pn->getCapture().data[0][(size_t) i] == before[(size_t) i];
        check (len0 > 48000 * 5 && same && pn->getTransportMode() == 0, "stopped DAW + noise from a plugin before Thruma: imported waveform untouched");
        phn.pos = 0;
        feed (200, true);    // then the DAW plays
        same = pn->getCapture().length.load() >= len0 && pn->getCapture().origin.load() == org0;
        for (int64_t i = 0; same && i < len0; i += 7) same = pn->getCapture().data[0][(size_t) i] == before[(size_t) i];
        check (same, "...and pressing play in the DAW keeps it");
    }

    // ---- 3c) trigger points: hits not used come back with a reason (Analyze and live); Amount / Match re-analyse ----
    {
        // loud snare-like hits every 0.5 s, and very quiet ones (-40 dB) in between
        auto hit = [] (int k) { return std::sin ((float) k * 0.09f) * std::exp (-(float) k / 900.0f) + 0.3f * std::sin ((float) k * 0.61f) * std::exp (-(float) k / 200.0f); };
        auto wavQ = writeWav ("thruma_points.wav", [&] (int i)
        {
            const int beat = 24000, k = i % beat;
            return 0.7f * hit (k) + 0.007f * hit ((i + beat / 2) % beat);
        }, 48000 * 8, 48000.0);
        std::vector<int64_t> quietTruth;
        for (int64_t t = 12000; t < 48000 * 8; t += 24000) quietTruth.push_back (48000 + t);   // import origin = playhead (1 s)
        std::unique_ptr<ThrumaProcessor> pq (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH phq; phq.playing = false; phq.pos = 48000; pq->setPlayHead (&phq); pq->prepareToPlay (48000.0, 512);
        juce::String result;
        pq->importAudio (wavQ, [&] (const juce::String& m) { result = m; });
        for (int i = 0; i < 100 && result.isEmpty(); ++i) pump (50);
        const int notes0 = (int) pq->getTriggers().size();
        int quietPoints = 0;
        for (auto& tp : pq->getTriggerPoints())
            for (auto q : quietTruth)
                if (std::llabs (tp.position - q) < 480 && tp.reason == RejectReason::TooQuiet) { ++quietPoints; break; }
        const auto firstReason = pq->getTriggerPoints().empty() ? juce::String ("none") : ThrumaProcessor::describe (pq->getTriggerPoints().front());
        check (notes0 >= 15 && notes0 <= 17, "Analyze: the loud hits become notes (" + juce::String (notes0) + ")");
        check (quietPoints >= 12, "Analyze: the quiet hits show as 'too quiet' trigger points (" + juce::String (quietPoints) + " of 16): " + firstReason);

        // Match / Amount re-analyse like Sensitivity
        const int before = pq->getOfflineMessageCount();
        if (auto* prm = pq->apvts.getParameter (ids::repMatch)) prm->setValueNotifyingHost (0.2f);
        for (int i = 0; i < 100 && pq->getOfflineMessageCount() == before; ++i) pump (50);
        check (pq->getOfflineMessageCount() > before, "changing Tolerance re-analyses: " + pq->getOfflineMessage());
        const int before2 = pq->getOfflineMessageCount();
        if (auto* prm = pq->apvts.getParameter (ids::repAmount)) prm->setValueNotifyingHost (0.8f);
        for (int i = 0; i < 100 && pq->getOfflineMessageCount() == before2; ++i) pump (50);
        check (pq->getOfflineMessageCount() > before2, "changing Amount re-analyses: " + pq->getOfflineMessage());
        // raising Sensitivity all the way turns most quiet points into notes
        const int before3 = pq->getOfflineMessageCount();
        if (auto* prm = pq->apvts.getParameter (ids::repSens)) prm->setValueNotifyingHost (1.0f);
        if (auto* prm = pq->apvts.getParameter (ids::repGhost)) prm->setValueNotifyingHost (1.0f);
        for (int i = 0; i < 100 && pq->getOfflineMessageCount() == before3; ++i) pump (50);
        check ((int) pq->getTriggers().size() >= notes0 + 10, "Sensitivity + Ghost up: the quiet hits become notes (" + juce::String ((int) pq->getTriggers().size()) + ")");

        // live: the same audio played through the DAW
        std::unique_ptr<ThrumaProcessor> pl (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH phl; phl.playing = true; phl.pos = 0; pl->setPlayHead (&phl); pl->prepareToPlay (48000.0, 512);
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (wavQ));
        juce::AudioBuffer<float> all (1, (int) rd->lengthInSamples);
        rd->read (&all, 0, (int) rd->lengthInSamples, 0, true, false);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        for (int p = 0; p + 512 <= all.getNumSamples(); p += 512)
        {
            for (int c = 0; c < 2; ++c) buf.copyFrom (c, 0, all, 0, p, 512);
            midi.clear(); phl.pos = p; pl->processBlock (buf, midi);
            if ((p / 512) % 40 == 0) pump (5);
        }
        pump (300);
        int livePoints = 0;
        for (auto& tp : pl->getTriggerPoints())
            for (auto q : quietTruth)
                if (std::llabs (tp.position - (q - 48000)) < 480) { ++livePoints; break; }
        check (livePoints >= 10, "live: quiet hits show as trigger points on the song timeline (" + juce::String (livePoints) + " of 16, "
                                     + juce::String ((int) pl->getTriggerPoints().size()) + " points)");
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

        // sidechain drum (in the first channels) triggers the snare, on both sides (after the MIDI note's
        // 2-second window: while MIDI plays the kit, audio is not tracked)
        for (int blk = 0; blk < 200; ++blk) { buf.clear(); midi.clear(); ph3.pos = (20 + blk) * 512; p3->processBlock (buf, midi); }
        std::mt19937 rng (3); std::uniform_real_distribution<float> u (-1, 1);
        int hitsL = 0; float rr = 0;
        for (int64_t pos = 0; pos < 48000 * 4; pos += 512)
        {
            ph3.pos = 220 * 512 + pos; midi.clear();
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

    // ---- 5b) blend also sets MIDI-played samples when the tracked drum (Snare) has no samples ----
    {
        std::unique_ptr<ThrumaProcessor> p7 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH ph7; p7->setPlayHead (&ph7); p7->prepareToPlay (48000.0, 512);
        p7->getKitManager().assignFiles ((int) Instrument::Kick, { click });
        p7->rebuildKit();
        auto peakAt = [&] (float blend)
        {
            p7->apvts.getParameter (ids::repBlend)->setValueNotifyingHost (blend);
            pump (50);
            juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
            float pk = 0;
            for (int blk = 0; blk < 12; ++blk)
            {
                buf.clear(); midi.clear(); ph7.pos += 512;
                if (blk == 1) midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 127), 50);
                p7->processBlock (buf, midi);
                pk = std::max (pk, buf.getMagnitude (0, 0, 512));
            }
            return pk;
        };
        const float full = peakAt (1.0f), quarter = peakAt (0.25f);
        check (full > 0.3f && std::abs (quarter - full) < 0.01f, "MIDI-played samples play at full level whatever the Blend (MIDI is a sample player): "
               + juce::String (quarter, 3) + " vs " + juce::String (full, 3));
    }

    // ---- 5c) imported audio through the real editor: ruler click while playing in Thruma, Blend while playing ----
    {
        std::unique_ptr<ThrumaProcessor> p8 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH ph8; ph8.playing = false; ph8.pos = 0; p8->setPlayHead (&ph8); p8->prepareToPlay (48000.0, 512);
        auto tone = writeWav ("thruma_tone2.wav", [] (int i) { return 0.8f * std::sin ((float) i * 0.05f) * std::exp (-(float) i / 4000.0f); }, 24000, 48000.0);
        p8->getKitManager().assignFiles ((int) Instrument::Snare, { tone });
        p8->rebuildKit();
        std::unique_ptr<juce::AudioProcessorEditor> ed (p8->createEditorIfNeeded());
        ed->setSize (1080, 760);
        std::function<ui::TrackerLane* (juce::Component*)> findLane = [&] (juce::Component* c) -> ui::TrackerLane*
        {
            if (auto* l = dynamic_cast<ui::TrackerLane*> (c)) return l;
            for (auto* ch : c->getChildren()) if (auto* l = findLane (ch)) return l;
            return nullptr;
        };
        auto* lane = findLane (ed.get());
        check (lane != nullptr && lane->isShowing() == lane->isShowing(), "editor has the track lane");

        std::mt19937 rng (5); std::uniform_real_distribution<float> u (-1, 1);
        auto wav = writeWav ("thruma_import2.wav", [&] (int i) { const double t = std::fmod ((double) i / 48000.0, 0.5); return (float) (0.5 * std::exp (-t / 0.04)) * u (rng); },
                             48000 * 20, 48000.0);
        juce::String result;
        p8->importAudio (wav, [&] (const juce::String& m) { result = m; });
        for (int i = 0; i < 100 && result.isEmpty(); ++i) pump (50);
        pump (300);
        p8->togglePlay();
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        for (int blk = 0; blk < 40; ++blk) { buf.clear(); midi.clear(); p8->processBlock (buf, midi); }
        pump (100);

        // a real mouse click on the ruler, 12 s into the song
        auto* lc = static_cast<juce::Component*> (lane);
        auto findX = [&] (double pos) { for (int x = 0; x < lc->getWidth(); ++x) if (lane->xToPos ((float) x) >= pos) return x; return -1; };
        juce::ignoreUnused (findX);
        lane->zoomToFit();   // what Import Audio does in the editor
        const int x = lc->getWidth() * 4 / 5;
        const double target = lane->xToPos ((float) x);
        std::printf ("  (lane shows %.2f .. %.2f s, clicking %.2f s)\n", lane->xToPos (0) / 48000.0, lane->xToPos ((float) lc->getWidth()) / 48000.0, target / 48000.0);
        if (x >= 0)
        {
            auto src = juce::Desktop::getInstance().getMainMouseSource();
            const juce::Point<float> pt ((float) x, 8.0f);
            const auto now = juce::Time::getCurrentTime();
            juce::MouseEvent e (src, pt, juce::ModifierKeys::leftButtonModifier, 1.0f, 0, 0, 0, 0, lc, lc, now, pt, now, 1, false);
            lc->mouseDown (e);
            lc->mouseUp (e);
        }
        for (int blk = 0; blk < 4; ++blk) { buf.clear(); midi.clear(); p8->processBlock (buf, midi); }
        pump (100);
        const auto head = p8->getPlayheadPosition();
        p8->togglePlay();
        fakeLocated = -1;
        buf.clear(); midi.clear(); p8->processBlock (buf, midi);
        const auto stoppedWhy = p8->locateTo (48000 * 7);
        check (stoppedWhy.isEmpty() && fakeLocated == 48000 * 7, "stopped, imported audio: a ruler click also moves the DAW's playhead (RAVENYR)");
        p8->togglePlay();
        p8->locateTo ((int64_t) target);
        for (int blk = 0; blk < 4; ++blk) { buf.clear(); midi.clear(); p8->processBlock (buf, midi); }
        check (p8->isPreviewPlaying() && std::abs ((double) p8->getPlayheadPosition() - target) < 48000 / 4, "ruler click during in-plugin playback jumps there: playhead "
               + juce::String ((double) head / 48000.0, 2) + " s");

        auto corrAt = [&] (float blend)
        {
            p8->apvts.getParameter (ids::repBlend)->setValueNotifyingHost (blend);
            pump (60);
            p8->locateTo (48000 * 2);
            double xy = 0, xx = 0, yy = 0;
            const int L = p8->getLatencySamples();
            std::vector<float> outv;
            for (int blk = 0; blk < 300; ++blk) { buf.clear(); midi.clear(); p8->processBlock (buf, midi); for (int i = 0; i < 512; ++i) outv.push_back (buf.getSample (0, i)); }
            // compare with the imported audio at the same song positions
            const auto& cap = p8->getCapture();
            for (size_t i = 48000; i < outv.size(); ++i)
            {
                const int64_t songPos = 48000 * 2 + (int64_t) i - L;
                const float d = cap.data[0][(size_t) songPos];
                xy += d * outv[i]; xx += d * d; yy += outv[i] * outv[i];
            }
            return xy / std::sqrt (xx * yy + 1e-12);
        };
        const double c0 = corrAt (0.0f), c1 = corrAt (1.0f);
        // the DAW playing (instrument track, silent input): the imported drum is still the original for Blend
        p8->togglePlay();
        p8->apvts.getParameter (ids::repBlend)->setValueNotifyingHost (0.0f);
        pump (60);
        ph8.playing = true; ph8.pos = 48000 * 3;
        float pkHost = 0;
        for (int blk = 0; blk < 100; ++blk) { buf.clear(); midi.clear(); p8->processBlock (buf, midi); ph8.pos += 512; pkHost = std::max (pkHost, buf.getMagnitude (0, 0, 512)); }
        check (pkHost > 0.3f, "DAW playing with silent input: the imported drum plays at Blend 0 % (peak " + juce::String (pkHost, 2) + ")");
        fakeLocated = -1;
        const auto why = p8->locateTo (48000);
        check (why.isNotEmpty() || fakeLocated == 48000, "ruler click while the DAW plays: moves a DAW that allows it (RAVENYR), else explains");
        ph8.playing = false; buf.clear(); midi.clear(); p8->processBlock (buf, midi);
        check (c0 > 0.98, "imported audio, Blend 0 %: the original (correlation " + juce::String (c0, 3) + ")");
        check (c1 < 0.3, "imported audio, Blend 100 %: samples only (correlation " + juce::String (c1, 3) + ")");

        // reopening the project: the imported audio comes back, the saved notes (incl. a hand-added one) stay
        auto list = p8->getTriggers().getAll();
        Trigger mine; mine.position = 48000 * 7 + 1234; mine.velocity = 0.6f; mine.id = 4242424;
        list.push_back (mine);
        std::sort (list.begin(), list.end(), [] (auto& a, auto& b) { return a.position < b.position; });
        p8->commitTriggerEdit (list, "add", true);
        juce::MemoryBlock state;
        p8->getStateInformation (state);
        const auto savedNotes = p8->getTriggers().size();
        const auto savedLen = p8->getCapture().length.load();
        std::unique_ptr<ThrumaProcessor> re (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        re->setStateInformation (state.getData(), (int) state.getSize());
        PH phr; phr.playing = false; re->setPlayHead (&phr); re->prepareToPlay (48000.0, 512);
        for (int i = 0; i < 80 && re->getOfflineSource() == ThrumaProcessor::OfflineSource::None; ++i) pump (50);
        for (int i = 0; i < 40 && re->isAnalysing(); ++i) pump (50);
        pump (200);
        bool hasMine = false;
        for (auto& t : re->getTriggers().getAll()) if (t.position == mine.position) hasMine = true;
        check (re->getOfflineSource() == ThrumaProcessor::OfflineSource::File && re->getCapture().length.load() == savedLen,
               "reopened project: the imported WAV and its waveform are back");
        check (re->getTriggers().size() == savedNotes && hasMine && re->isFinalized(), "reopened project: the saved notes are unchanged ("
               + juce::String ((int) re->getTriggers().size()) + " of " + juce::String ((int) savedNotes) + ")");

        // SAMPLES: a drum with many samples - every sample stays inside its layer row
        {
            juce::Array<juce::File> many;
            for (int k = 0; k < 9; ++k)
            {
                juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("HiHat_Closed_Tight_Variation_" + juce::String (k) + ".wav");
                juce::AudioBuffer<float> b (1, 2000);
                for (int i = 0; i < 2000; ++i) b.setSample (0, i, 0.5f * std::sin ((float) i * 0.3f) * std::exp (-(float) i / 400.0f));
                f.deleteFile();
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (f), 48000.0, 1, 24, {}, 0));
                w->writeFromAudioSampleBuffer (b, 0, 2000);
                many.add (f);
            }
            p8->getKitManager().assignFiles ((int) Instrument::HiHat, many);
            p8->rebuildKit();
            std::function<ui::SamplesPage* (juce::Component*)> findPage = [&] (juce::Component* c) -> ui::SamplesPage*
            {
                if (auto* sp = dynamic_cast<ui::SamplesPage*> (c)) return sp;
                for (auto* ch : c->getChildren()) if (auto* sp = findPage (ch)) return sp;
                return nullptr;
            };
            auto* sp = findPage (ed.get());
            sp->setVisible (true);
            sp->selectSlot ((int) Instrument::HiHat);
            int chipsSeen = 0, outside = 0;
            std::function<void (juce::Component*)> scan = [&] (juce::Component* c)
            {
                if (auto* row = dynamic_cast<ui::LayerRow*> (c))
                    for (auto* ch : row->getChildren())
                        if (auto* b = dynamic_cast<juce::TextButton*> (ch); b != nullptr && b->getButtonText().startsWith ("HiHat"))
                        {
                            ++chipsSeen;
                            if (! row->getLocalBounds().contains (b->getBounds())) ++outside;
                        }
                for (auto* ch : c->getChildren()) scan (ch);
            };
            scan (sp);
            check (chipsSeen == 9 && outside == 0, "SAMPLES: all 9 hi-hat samples visible inside their rows (" + juce::String (chipsSeen) + " seen, " + juce::String (outside) + " cut off)");
        }
        // PLAYER: the right-click menu builds for every drum and pad, whatever their MIDI notes
        {
            std::function<ui::PlayerPage* (juce::Component*)> findPlayer = [&] (juce::Component* c) -> ui::PlayerPage*
            {
                if (auto* pp = dynamic_cast<ui::PlayerPage*> (c)) return pp;
                for (auto* ch : c->getChildren()) if (auto* pp = findPlayer (ch)) return pp;
                return nullptr;
            };
            auto* pp = findPlayer (ed.get());
            int built = 0;
            for (int note : { 0, 27, 36, 87, 127 })
                for (int slot = 0; slot < kNumInstruments + 3; ++slot)
                {
                    auto m = p8->getKitManager().getSlot (slot); m.midiNote = note; p8->getKitManager().setSlot (slot, m);
                    std::vector<juce::File> ids;
                    if (pp->buildMenu (slot, ids).getNumItems() > 5) ++built;
                }
            check (built == 5 * (kNumInstruments + 3), "PLAYER right-click menu builds for every drum and pad (" + juce::String (built) + ")");
        }
        ed.reset();
    }

    // ---- 5d) kit pads, hit monitor, auditions ----
    {
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("thruma_kit_import");
        dir.deleteRecursively();
        dir.getChildFile ("Cowbell").createDirectory();
        auto mk = [] (const juce::File& f, float freq)
        {
            juce::AudioBuffer<float> b (1, 6000);
            for (int i = 0; i < 6000; ++i) b.setSample (0, i, 0.7f * std::sin ((float) i * freq) * std::exp (-(float) i / 1500.0f));
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (f), 48000.0, 1, 24, {}, 0));
            w->writeFromAudioSampleBuffer (b, 0, 6000);
        };
        mk (dir.getChildFile ("Snare_rr1.wav"), 0.05f);
        mk (dir.getChildFile ("Snare_rr2.wav"), 0.06f);
        mk (dir.getChildFile ("Cowbell").getChildFile ("01.wav"), 0.2f);
        mk (dir.getChildFile ("Cowbell").getChildFile ("02.wav"), 0.21f);
        mk (dir.getChildFile ("Tambourine_v1.wav"), 0.3f);
        mk (dir.getChildFile ("Tambourine_v2.wav"), 0.31f);

        std::unique_ptr<ThrumaProcessor> p9 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH ph9; ph9.playing = false; p9->setPlayHead (&ph9); p9->prepareToPlay (48000.0, 512);
        auto& km = p9->getKitManager();
        const int n = km.importFolder (dir);
        p9->rebuildKit();
        const int pad0 = kNumInstruments, pad1 = kNumInstruments + 1;
        check (n == 6 && km.getNumUsedPads() == 2, "kit folder import: drums by name, other sounds onto pads (" + juce::String (km.getNumUsedPads()) + " pads)");
        check (km.getSlotName (pad0) == "Cowbell" && km.getSlotName (pad1) == "Tambourine", "pads named after folder / file: "
               + km.getSlotName (pad0) + ", " + km.getSlotName (pad1));
        const int note0 = km.getSlot (pad0).midiNote, note1 = km.getSlot (pad1).midiNote;
        check (note0 >= 0 && note1 >= 0 && note0 != note1 && note0 != 38 && note1 != 36, "pads get their own free MIDI notes: " + juce::String (note0) + ", " + juce::String (note1));

        // removing a pad: the next one moves up, nothing is left behind
        {
            km.setSlotName (kNumInstruments + 2, "Shaker");
            km.assignFiles (kNumInstruments + 2, { dir.getChildFile ("Tambourine_v1.wav") });
            const int shakerNote = km.getSlot (kNumInstruments + 2).midiNote;
            km.clearSlot (pad1);   // remove Tambourine
            check (km.getNumUsedPads() == 2 && km.getSlotName (pad1) == "Shaker" && km.getSlot (pad1).midiNote == shakerNote
                   && km.getSlot (kNumInstruments + 2).isEmpty() && km.getSlot (kNumInstruments + 2).name.isEmpty(),
                   "removing a pad removes its name; the next pad moves up with its note");
            km.clearSlot (pad1);
            km.setSlotName (pad1, "Tambourine");
            km.assignFiles (pad1, { dir.getChildFile ("Tambourine_v1.wav"), dir.getChildFile ("Tambourine_v2.wav") });
            auto t = km.getSlot (pad1); t.midiNote = note1; km.setSlot (pad1, t);
        }

        // save / load keeps the pads
        juce::File kf = dir.getChildFile ("kit.thrumakit");
        km.saveKitFile (kf);
        km.clearAll();
        check (km.getNumUsedPads() == 0, "new kit has no pads");
        km.loadKitFile (kf);
        check (km.getSlotName (pad1) == "Tambourine" && km.getSlot (pad1).midiNote == note1 && ! km.getSlot (pad1).isEmpty(), "pads survive saving and loading the kit");
        p9->rebuildKit();
        pump (100);

        // blend 0 must not silence auditions; the monitor reports the slot and the exact file
        p9->apvts.getParameter (ids::repBlend)->setValueNotifyingHost (0.0f);
        pump (50);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        const auto& hm = p9->getHitMonitor();
        std::set<uint64_t> snareFiles;
        float pk = 0;
        for (int k = 0; k < 4; ++k)
        {
            p9->auditionSlot ((int) Instrument::Snare, 0.9f);
            for (int blk = 0; blk < 3; ++blk) { buf.clear(); midi.clear(); p9->processBlock (buf, midi); pk = std::max (pk, buf.getMagnitude (0, 0, 512)); }
            snareFiles.insert (hm.slots[(size_t) Instrument::Snare].sampleHash.load());
        }
        const auto h1 = (uint64_t) std::hash<std::string>{} (dir.getChildFile ("Snare_rr1.wav").getFullPathName().toStdString());
        const auto h2 = (uint64_t) std::hash<std::string>{} (dir.getChildFile ("Snare_rr2.wav").getFullPathName().toStdString());
        check (pk > 0.3f, "Player click plays at Blend 0 % (peak " + juce::String (pk, 2) + ")");
        check (hm.slots[(size_t) Instrument::Snare].count.load() == 4 && snareFiles == std::set<uint64_t> { h1, h2 },
               "hit monitor names the exact file each hit played (round robin alternates both)");

        // a pad from a MIDI note (instrument track / MIDI file)
        const auto before = hm.slots[(size_t) pad1].count.load();
        buf.clear(); midi.clear(); midi.addEvent (juce::MidiMessage::noteOn (10, note1, (juce::uint8) 100), 10);
        p9->apvts.getParameter (ids::repBlend)->setValueNotifyingHost (1.0f);
        pump (50);
        float padPk = 0;
        for (int blk = 0; blk < 6; ++blk) { p9->processBlock (buf, midi); padPk = std::max (padPk, buf.getMagnitude (0, 0, 512)); buf.clear(); midi.clear(); }
        check (hm.slots[(size_t) pad1].count.load() == before + 1 && padPk > 0.2f, "MIDI note " + juce::String (note1) + " plays the Tambourine pad");

        // ---- mixer: volume / pan / mute / solo per slot ----
        auto hitLR = [&] (int slot) -> std::pair<float, float>
        {
            float l = 0, r = 0;
            p9->auditionSlot (slot, 1.0f);
            for (int blk = 0; blk < 8; ++blk)
            {
                buf.clear(); midi.clear(); p9->processBlock (buf, midi);
                l = std::max (l, buf.getMagnitude (0, 0, 512)); r = std::max (r, buf.getMagnitude (1, 0, 512));
            }
            for (int blk = 0; blk < 20; ++blk) { buf.clear(); midi.clear(); p9->processBlock (buf, midi); }   // let it ring out
            return { l, r };
        };
        const int sn = (int) Instrument::Snare;
        auto hit2 = [&] (int slot) { const auto a = hitLR (slot), b = hitLR (slot); return std::make_pair (std::max (a.first, b.first), std::max (a.second, b.second)); };   // both round-robin files
        hit2 (sn);   // warm-up: let earlier sounds die away
        const auto centre = hit2 (sn);
        km.setMix (sn, 0.0f, -1.0f, false, false); p9->syncMixer();
        const auto left = hit2 (sn);
        check (std::abs (centre.first - centre.second) < 0.01f && left.second < 0.001f && left.first > centre.first * 1.3f,
               "pan hard left: right silent, mono sample constant-power (L " + juce::String (left.first, 2) + " vs centre " + juce::String (centre.first, 2) + ")");
        km.setMix (sn, -6.0f, 0.0f, false, false); p9->syncMixer();
        km.setMix (sn, 0.0f, 0.0f, false, false); p9->syncMixer();
        const auto unity = hit2 (sn);   // same conditions as the -6 dB measurement
        km.setMix (sn, -6.0f, 0.0f, false, false); p9->syncMixer();
        const auto quiet = hit2 (sn);
        check (std::abs (quiet.first / unity.first - 0.501f) < 0.01f, "volume -6 dB halves the level (" + juce::String (quiet.first / unity.first, 3) + ")");
        km.setMix (sn, 0.0f, 0.0f, true, false); p9->syncMixer();
        check (hitLR (sn).first < 0.001f, "mute silences the drum");
        km.setMix (sn, 0.0f, 0.0f, false, false); km.setMix (pad1, 0.0f, 0.0f, false, true); p9->syncMixer();
        check (hitLR (sn).first < 0.001f && hitLR (pad1).first > 0.2f, "solo on a pad silences the other drums");
        km.setMix (pad1, 0.0f, 0.0f, false, false); km.setMix (sn, -3.0f, 0.4f, false, false); p9->syncMixer();
        hitLR (sn);
        check (p9->getMixer().slots[(size_t) sn].meter.exchange (0.0f) > 0.1f, "the snare's mixer meter moves");
        km.saveKitFile (kf); km.clearAll(); km.loadKitFile (kf);
        check (std::abs (km.getSlot (sn).volumeDb + 3.0f) < 0.01f && std::abs (km.getSlot (sn).pan - 0.4f) < 0.01f, "mixer settings are saved with the kit");

        // ---- crash 2: its own kit piece and note ----
        check (guessInstrumentFromName ("Crash 2_rr1") == Instrument::Crash2 && guessInstrumentFromName ("crash_18") == Instrument::Crash,
               "file names: Crash 2 vs Crash");
        km.assignFiles ((int) Instrument::Crash2, { dir.getChildFile ("Snare_rr1.wav") });
        p9->rebuildKit(); pump (50);
        const auto c2 = hm.slots[(size_t) Instrument::Crash2].count.load();
        buf.clear(); midi.clear(); midi.addEvent (juce::MidiMessage::noteOn (10, 57, (juce::uint8) 100), 10);
        p9->processBlock (buf, midi);
        check (hm.slots[(size_t) Instrument::Crash2].count.load() == c2 + 1, "MIDI note 57 plays Crash 2");
    }

    // ---- 5e) MIDI drives the kit: no audio replacing at the same time ----
    {
        std::unique_ptr<ThrumaProcessor> pm (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH phm; phm.playing = true; pm->setPlayHead (&phm); pm->prepareToPlay (48000.0, 512);
        pm->getKitManager().assignFiles ((int) Instrument::Snare, { click });
        pm->getKitManager().assignFiles ((int) Instrument::Kick, { click });
        pm->rebuildKit(); pump (100);
        std::mt19937 rng (2); std::uniform_real_distribution<float> u (-1, 1);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        const auto& hm = pm->getHitMonitor();
        auto snareHits = [&] { return hm.slots[(size_t) Instrument::Snare].count.load(); };
        auto playDrum = [&] (int64_t from, int64_t to, bool sendMidi)
        {
            for (int64_t pos = from; pos < to; pos += 512)
            {
                phm.pos = pos; midi.clear();
                for (int i = 0; i < 512; ++i)
                {
                    const double t = std::fmod ((double) (pos + i) / 48000.0, 0.5);
                    const float v = (float) (0.5 * std::exp (-t / 0.04)) * u (rng);
                    buf.setSample (0, i, v); buf.setSample (1, i, v);
                }
                if (sendMidi && ((pos - from) / 512) % 20 == 0) midi.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 100), 0);
                pm->processBlock (buf, midi);
            }
        };
        playDrum (0, 48000 * 4, false);
        const auto tracked = snareHits();
        check (tracked >= 6, "audio alone: the snare is tracked and replaced (" + juce::String ((int) tracked) + " hits)");
        const auto kickBefore = hm.slots[(size_t) Instrument::Kick].count.load();
        playDrum (48000 * 4, 48000 * 8, true);
        check (snareHits() == tracked && hm.slots[(size_t) Instrument::Kick].count.load() > kickBefore && pm->isMidiDriving(),
               "while MIDI plays the kit, audio is not replaced (snare hits " + juce::String ((int) (snareHits() - tracked)) + ")");
        playDrum (48000 * 8, 48000 * 12, false);
        check (snareHits() > tracked && ! pm->isMidiDriving(), "two seconds after the MIDI stops, tracking resumes");
    }

    // ---- 5f) loading samples in the background, with progress ----
    {
        std::unique_ptr<ThrumaProcessor> pl (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH phl; pl->setPlayHead (&phl); pl->prepareToPlay (44100.0, 512);   // a different rate: samples get converted too
        juce::Array<juce::File> files;
        juce::File d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("thruma_loadtest");
        d.deleteRecursively(); d.createDirectory();
        for (int k = 0; k < 40; ++k)
        {
            juce::File f = d.getChildFile ("Snare_" + juce::String (k) + ".wav");
            juce::AudioBuffer<float> b (2, 48000 * 2);
            for (int i = 0; i < b.getNumSamples(); ++i) { const float v = (0.2f + 0.02f * (float) k) * std::sin ((float) i * 0.07f) * std::exp (-(float) i / 9000.0f); b.setSample (0, i, v); b.setSample (1, i, v); }
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (f), 48000.0, 2, 24, {}, 0));
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            files.add (f);
        }
        bool done = false;
        pl->runKitJob ([files] (KitManager& km) { km.setProgressTotal (files.size()); km.assignFiles ((int) Instrument::Snare, files); }, [&] { done = true; });
        int maxDone = 0, sawTotal = 0;
        double worstMs = 0.0;
        bool sawLoading = false;
        for (int i = 0; i < 600 && ! done; ++i)
        {
            sawLoading = sawLoading || pl->isLoadingKit();
            maxDone = std::max (maxDone, pl->getKitManager().getProgressDone());
            sawTotal = std::max (sawTotal, pl->getKitManager().getProgressTotal());
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            pl->getKitManager().getSlot ((int) Instrument::Snare);   // what the UI does while it loads
            worstMs = std::max (worstMs, juce::Time::getMillisecondCounterHiRes() - t0);
            pump (10);
        }
        int files2 = 0;
        for (auto& l : pl->getKitManager().getSlot ((int) Instrument::Snare).layers) files2 += l.files.size();
        check (done && sawLoading && ! pl->isLoadingKit() && files2 == 40, "40 samples load in the background, then the kit is ready");
        check (sawTotal >= 40 && maxDone > 0, "the loading bar counts files (" + juce::String (maxDone) + " / " + juce::String (sawTotal) + " seen)");
        check (worstMs < 40.0, "the editor never waits on the loading (worst " + juce::String (worstMs, 1) + " ms)");
    }

    // ---- 5g) TDE files, extra outputs, display order ----
    {
        juce::File d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("thruma_tde");
        d.deleteRecursively(); d.createDirectory();
        auto mk = [&] (const juce::String& name, float amp)
        {
            juce::File f = d.getChildFile (name + ".wav");
            juce::AudioBuffer<float> b (1, 4000);
            for (int i = 0; i < 4000; ++i) b.setSample (0, i, amp * std::sin ((float) i * 0.11f) * std::exp (-(float) i / 900.0f));
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (f), 48000.0, 1, 24, {}, 0));
            w->writeFromAudioSampleBuffer (b, 0, 4000);
            return f;
        };
        std::unique_ptr<ThrumaProcessor> pt (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH pht; pt->setPlayHead (&pht);
        auto& km = pt->getKitManager();
        KitManager::SlotModel sm;
        KitManager::LayerModel soft, hard;
        soft.velLo = 0.0f; soft.velHi = 0.37f; soft.files = { mk ("Snare_soft_1", 0.2f), mk ("Snare_soft_2", 0.21f) };
        hard.velLo = 0.38f; hard.velHi = 1.0f; hard.files = { mk ("Snare_hard_1", 0.8f), mk ("Snare_hard_2", 0.81f), mk ("Snare_hard_3", 0.82f) };
        sm.layers = { soft, hard }; sm.rrMode = RoundRobinMode::Random; sm.midiNote = 38;
        km.setSlot ((int) Instrument::Snare, sm);
        juce::String err;
        const auto tde = d.getChildFile ("My Snare.tde");
        check (km.saveSlotTde ((int) Instrument::Snare, tde, err) && tde.existsAsFile(), "snare saved as one TDE file (" + juce::String (tde.getSize() / 1024) + " KB) " + err);
        km.loadSlotTde ((int) Instrument::Tom1, tde, err);
        const auto t1 = km.getSlot ((int) Instrument::Tom1);
        bool same = t1.layers.size() == 2 && t1.rrMode == RoundRobinMode::Random && t1.midiNote == 48
                    && std::abs (t1.layers[0].velHi - 0.37f) < 1.0e-4f && std::abs (t1.layers[1].velLo - 0.38f) < 1.0e-4f
                    && t1.layers[0].files.size() == 2 && t1.layers[1].files.size() == 3;
        auto decode = [&] (const juce::File& f, juce::AudioBuffer<float>& b)
        {
            std::unique_ptr<juce::AudioFormatReader> r (km.getFormatManager().createReaderFor (f));
            if (r == nullptr) return false;
            b.setSize ((int) r->numChannels, (int) r->lengthInSamples);
            return r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
        };
        bool allFlac = same;
        if (same)
            for (int i = 0; i < 3; ++i)
            {
                juce::AudioBuffer<float> a, b;
                const auto& got = t1.layers[1].files[i];
                bool eq = decode (hard.files[i], a) && decode (got, b) && a.getNumSamples() == b.getNumSamples() && a.getNumChannels() == b.getNumChannels();
                for (int c = 0; eq && c < a.getNumChannels(); ++c)
                    eq = std::memcmp (a.getReadPointer (c), b.getReadPointer (c), sizeof (float) * (size_t) a.getNumSamples()) == 0;
                same = same && eq && got.isAChildOf (KitManager::getTdeLibraryFolder());
                allFlac = allFlac && got.hasFileExtension ("flac");
            }
        check (same, "TDE loads onto another drum with the same velocity layers, round robin and bit-identical samples (keeps that drum's MIDI note)");
        juce::int64 wavBytes = 0;
        for (auto& f : soft.files) wavBytes += f.getSize();
        for (auto& f : hard.files) wavBytes += f.getSize();
        check (allFlac && tde.getSize() < wavBytes * 7 / 10, "TDE stores samples as lossless FLAC: " + juce::String (tde.getSize()) + " bytes vs " + juce::String (wavBytes) + " bytes of WAV");
        {
            // 32-bit float WAVs can't go in FLAC bit-exactly: they are stored unchanged.
            juce::File ff = d.getChildFile ("float_hit.wav");
            {
                juce::AudioBuffer<float> b (1, 2000);
                for (int i = 0; i < 2000; ++i) b.setSample (0, i, 0.5f * std::sin ((float) i * 0.07f));
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (ff), 48000.0, 1, 32, {}, 0));
                w->writeFromAudioSampleBuffer (b, 0, 2000);
            }
            KitManager::SlotModel fm; KitManager::LayerModel fl; fl.files = { ff }; fm.layers = { fl };
            km.setSlot ((int) Instrument::Ride, fm);
            const auto ftde = d.getChildFile ("Float.tde");
            km.saveSlotTde ((int) Instrument::Ride, ftde, err);
            km.loadSlotTde ((int) Instrument::China, ftde, err);
            const auto ch = km.getSlot ((int) Instrument::China);
            juce::MemoryBlock a, b;
            ff.loadFileAsData (a);
            if (! ch.layers.empty() && ! ch.layers[0].files.isEmpty()) ch.layers[0].files[0].loadFileAsData (b);
            check (a.getSize() > 0 && a == b, "32-bit float samples are kept as the original WAV inside the TDE");
        }
        const auto firstFolder = t1.layers[0].files[0].getParentDirectory();
        km.loadSlotTde ((int) Instrument::Tom2, tde, err);
        check (km.getSlot ((int) Instrument::Tom2).layers[0].files[0].getParentDirectory() == firstFolder, "loading the same TDE again reuses its unpacked samples");
        {
            // Save all + load many
            km.setKitName ("Test Kit");
            KitManager::SlotModel kick; KitManager::LayerModel kl; kl.files = { mk ("Kick_1", 0.9f) }; kick.layers = { kl };
            km.setSlot ((int) Instrument::Kick, kick);
            const int padSlot = kNumInstruments + km.getNumUsedPads();
            KitManager::SlotModel cow; KitManager::LayerModel cl; cl.files = { mk ("Cowbell_1", 0.5f) }; cow.layers = { cl };
            km.setSlot (padSlot, cow); km.setSlotName (padSlot, "Cowbell");
            const auto all = d.getChildFile ("AllTde");
            juce::StringArray errs;
            int expected = 0;
            for (int i = 0; i < kNumKitSlots; ++i) if (! km.getSlot (i).isEmpty()) ++expected;
            const int nSaved = km.saveAllTde (all, errs);
            const auto saved = all.findChildFiles (juce::File::findFiles, false, "*.tde");
            check (nSaved == expected && saved.size() == expected && all.getChildFile ("Test Kit - Snare.tde").existsAsFile()
                       && all.getChildFile ("Test Kit - Cowbell.tde").existsAsFile(),
                   "Save All writes one TDE per drum/pad into a folder: " + juce::String (saved.size()) + " files");

            std::unique_ptr<ThrumaProcessor> p2 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
            auto& k2 = p2->getKitManager();
            errs.clear();
            const int nLoaded = k2.loadTdeFiles (juce::Array<juce::File> (saved.getRawDataPointer(), saved.size()), errs);
            bool ok = nLoaded == expected && errs.isEmpty()
                      && k2.getSlot ((int) Instrument::Kick).layers.size() == 1
                      && k2.getSlot ((int) Instrument::Snare).layers.size() == 2
                      && k2.getSlot ((int) Instrument::Snare).rrMode == RoundRobinMode::Random;
            bool cowOnPad = false;
            for (int i = kNumInstruments; i < kNumKitSlots; ++i) if (k2.getSlotName (i) == "Cowbell" && ! k2.getSlot (i).isEmpty()) cowOnPad = true;
            check (ok && cowOnPad, "Load TDEs puts each file back on its own drum (and pads on pads) in one go: " + juce::String (nLoaded) + " loaded");
            // two snare files at once: the second goes on a pad, nothing lost
            errs.clear();
            std::unique_ptr<ThrumaProcessor> p3 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
            auto& k3 = p3->getKitManager();
            const int n3 = k3.loadTdeFiles ({ tde, all.getChildFile ("Test Kit - Snare.tde") }, errs);
            check (n3 == 2 && ! k3.getSlot ((int) Instrument::Snare).isEmpty() && k3.getNumUsedPads() == 1, "two TDEs for the same drum: the second lands on a pad");
        }
        juce::File bad = d.getChildFile ("not.tde"); bad.replaceWithText ("hello");
        check (! km.loadSlotTde ((int) Instrument::Tom3, bad, err) && err.isNotEmpty(), "a broken TDE is refused with a message: " + err);

        // extra outputs: route the snare to Out 2
        auto layout = pt->getBusesLayout();
        layout.outputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();   // side-chain on too (shares channels with Out 2)
        check (pt->setBusesLayout (layout), "host can turn on Thruma's extra outputs");
        pt->prepareToPlay (48000.0, 512);
        pt->rebuildKit(); pump (100);
        const int total = pt->getTotalNumOutputChannels();
        juce::AudioBuffer<float> buf (juce::jmax (total, pt->getTotalNumInputChannels()), 512); juce::MidiBuffer midi;
        auto hit = [&] (std::vector<float>& peaks, bool sidechainNoise)
        {
            peaks.assign ((size_t) buf.getNumChannels(), 0.0f);
            pt->auditionSlot ((int) Instrument::Snare, 1.0f);
            for (int blk = 0; blk < 10; ++blk)
            {
                buf.clear(); midi.clear();
                if (sidechainNoise) for (int c = 2; c < 4; ++c) for (int i = 0; i < 512; ++i) buf.setSample (c, i, 0.5f * std::sin ((float) i * 0.3f));
                pt->processBlock (buf, midi);
                for (int c = 0; c < buf.getNumChannels(); ++c) peaks[(size_t) c] = std::max (peaks[(size_t) c], buf.getMagnitude (c, 0, 512));
            }
            for (int blk = 0; blk < 10; ++blk) { buf.clear(); midi.clear(); pt->processBlock (buf, midi); }
        };
        std::vector<float> pk;
        hit (pk, true);
        check (pk[0] > 0.3f && pk[2] < 1.0e-6f, "snare on Main: Out 2 silent, even with side-chain audio in the shared channels (" + juce::String (pk[2], 6) + ")");
        km.setOutput ((int) Instrument::Snare, 1); pt->syncMixer();
        hit (pk, true);
        check (pk[0] < 1.0e-6f && pk[2] > 0.3f && std::abs (pk[2] - pk[3]) < 0.01f, "snare routed to Out 2: plays only there, in stereo (Out 2 "
               + juce::String (pk[2], 2) + ", main " + juce::String (pk[0], 4) + ")");
        km.setOutput ((int) Instrument::Snare, 5); pt->syncMixer();   // Out 6 is switched off in the host
        hit (pk, false);
        check (pk[0] > 0.3f, "routed to an output the host has off: it plays on Main instead");

        check (slotForRow (7) == (int) Instrument::Crash && slotForRow (8) == (int) Instrument::Crash2 && rowForSlot ((int) Instrument::Ride) == 9,
               "lists show Crash 2 right after Crash");
    }

    // ---- 6) stopped host: the playhead stays put ----
    {
        std::unique_ptr<ThrumaProcessor> p5 (dynamic_cast<ThrumaProcessor*> (createPluginFilter()));
        PH ph5; ph5.playing = true; ph5.pos = 0; p5->setPlayHead (&ph5); p5->prepareToPlay (48000.0, 512);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        buf.clear(); midi.clear(); p5->processBlock (buf, midi);          // the host has played once
        ph5.playing = false; ph5.pos = 96000;
        for (int blk = 0; blk < 50; ++blk) { for (int i = 0; i < 512; ++i) { buf.setSample (0, i, 0.3f * std::sin (i * 0.3f)); buf.setSample (1, i, 0.3f * std::sin (i * 0.3f)); } p5->processBlock (buf, midi); }
        check (! p5->isTransportPlaying() && p5->getPlayheadPosition() == 96000, "stopped host with audio coming in: not playing, playhead at the host cursor (" + juce::String (p5->getPlayheadPosition()) + ")");
        // a host that keeps its position running while stopped, after it has reported play once
        ph5.playing = true; buf.clear(); midi.clear(); p5->processBlock (buf, midi);
        ph5.playing = false;
        for (int blk = 0; blk < 50; ++blk) { ph5.pos += 512; buf.clear(); midi.clear(); p5->processBlock (buf, midi); }
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
