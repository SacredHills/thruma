// Dev harness: MIDI export timing against the DAW beat grid, and Import Audio, through the real processor.
#include "../../Source/Plugin/PluginProcessor.h"
#include <random>
#include <thread>
struct PH : juce::AudioPlayHead
{
    int64_t pos = 0; double bpm = 137.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setIsPlaying (true); p.setTimeInSamples (pos); p.setBpm (bpm);
        p.setPpqPosition (4.0 + (double) pos / 48000.0 * bpm / 60.0);   // song starts at beat 4 (e.g. 1 bar count-in offset)
        return p;
    }
};
static float burst (int64_t n, std::mt19937& rng)
{
    std::uniform_real_distribution<float> u (-1, 1);
    const double t = (double) (n % 12000) / 48000.0;
    return (float) (0.6 * std::exp (-t / 0.05)) * u (rng) + 0.0005f * u (rng);
}
int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto* proc = dynamic_cast<thruma::ThrumaProcessor*> (createPluginFilter());
    PH ph; proc->setPlayHead (&ph); proc->prepareToPlay (48000.0, 512);
    std::thread audio ([&]
    {
        std::mt19937 rng (1);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        for (int64_t pos = 0; pos < 10 * 48000; pos += 512)
        {
            ph.pos = pos;
            for (int i = 0; i < 512; ++i) { const float v = burst (pos + i, rng); buf.setSample (0, i, v); buf.setSample (1, i, v); }
            midi.clear(); proc->processBlock (buf, midi);
            std::this_thread::sleep_for (std::chrono::microseconds (300));
        }
        juce::Thread::sleep (300);
        juce::MessageManager::callAsync ([&]
        {
            // 1) MIDI timing: notes should sit at beat 4 + t*bpm/60 for hits at t = k * 0.25 s
            juce::File f ("/tmp/thruma_test.mid");
            proc->exportMidi (f);
            juce::FileInputStream in (f); juce::MidiFile mf; mf.readFrom (in);
            double maxErrMs = 0; int notes = 0;
            for (auto& t : proc->getTriggers().getAll()) std::printf ("trig %lld (%.2f ms off grid) beat %.4f\n", (long long) t.position, ((t.position % 12000) > 6000 ? (t.position % 12000) - 12000 : (t.position % 12000)) / 48.0, proc->beatAt (t.position));
            for (auto* e : *mf.getTrack (0))
                if (e->message.isNoteOn())
                {
                    const double beat = e->message.getTimeStamp() / 960.0;
                    const double t = (beat - 4.0) * 60.0 / ph.bpm;
                    const double nearest = std::round (t / 0.25) * 0.25;
                    maxErrMs = std::max (maxErrMs, std::abs (t - nearest) * 1000.0);
                    ++notes;
                }
            std::printf ("MIDI export: %d notes, max timing error vs true hits on the DAW grid = %.3f ms\n", notes, maxErrMs);

            // 2) Import audio: write a WAV, import it
            juce::File wav ("/tmp/thruma_import.wav"); wav.deleteFile();
            {
                std::mt19937 rng (5);
                juce::AudioBuffer<float> b (1, 44100 * 6);
                for (int i = 0; i < b.getNumSamples(); ++i) { const double t = std::fmod ((double) i / 44100.0, 0.3);
                    std::uniform_real_distribution<float> u (-1, 1); b.setSample (0, i, (float) (0.5 * std::exp (-t / 0.05)) * u (rng)); }
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (wav), 44100.0, 1, 24, {}, 0));
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            }
            proc->importAudio (wav, [&] (const juce::String& m)
            {
                std::printf ("Import: %s  finalized=%d  heard=%.2f s\n", m.toRawUTF8(), proc->isFinalized() ? 1 : 0,
                             proc->getCapture().length.load() / 48000.0);
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
        });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    audio.join();
    delete proc;
}
