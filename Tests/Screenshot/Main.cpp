// Dev tool: runs the plugin's processor on a synthetic drum session with a moving transport and
// writes PNG snapshots of the editor pages. Usage: ThrumaScreenshot <out-dir>
#include "../../Source/Plugin/UI/TrackerLane.h"
#include "../../Source/Plugin/PluginProcessor.h"
#include "../TestSignals.h"
#include <thread>

struct PH : juce::AudioPlayHead
{
    int64_t pos = 0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setIsPlaying (true); p.setTimeInSamples (pos); p.setBpm (190.0); return p;
    }
};

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File out (argc > 1 ? argv[1] : "/tmp");
    std::unique_ptr<juce::AudioProcessor> proc (createPluginFilter());
    PH ph;
    proc->setPlayHead (&ph);
    proc->setRateAndBufferSizeDetails (48000.0, 256);
    proc->prepareToPlay (48000.0, 256);

    sig::SessionOptions o; o.seconds = 14.0; o.bleedScaleDb = 6.0f;
    const auto s = sig::makeMetalSession (o);

    // a small kit: snare (2 round-robin files), kick, and a pad
    auto* tp = dynamic_cast<thruma::ThrumaProcessor*> (proc.get());
    auto wav = [] (const juce::String& name, float freq)
    {
        juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("thruma_shot").getChildFile (name + ".wav");
        f.getParentDirectory().createDirectory();
        juce::AudioBuffer<float> b (1, 12000);
        for (int i = 0; i < 12000; ++i) b.setSample (0, i, 0.8f * std::sin ((float) i * freq) * std::exp (-(float) i / 2500.0f));
        f.deleteFile();
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (f), 48000.0, 1, 24, {}, 0));
        w->writeFromAudioSampleBuffer (b, 0, 12000);
        return f;
    };
    tp->getKitManager().assignFiles ((int) thruma::Instrument::Snare, { wav ("Snare_Top_rr1", 0.05f), wav ("Snare_Top_rr2", 0.051f) });
    tp->getKitManager().assignFiles ((int) thruma::Instrument::Kick, { wav ("Kick_In", 0.01f) });
    tp->getKitManager().setSlotName (thruma::kNumInstruments, "Cowbell");
    tp->getKitManager().assignFiles (thruma::kNumInstruments, { wav ("Cowbell", 0.12f) });
    tp->rebuildKit();

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditorAndMakeActive());
    ed->setSize (1080, 760);
    auto snap = [&] (const juce::String& tab, const juce::String& file)
    {
        juce::MessageManager::callAsync ([&, tab, file]
        {
            if (tab.isNotEmpty())
                for (auto* c : ed->getChildren())
                    if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == tab) b->onClick();
        });
        juce::Thread::sleep (250);
        juce::MessageManager::callAsync ([&, file]
        {
            auto img = ed->createComponentSnapshot (ed->getLocalBounds());
            juce::File f = out.getChildFile (file);
            f.deleteFile();
            juce::FileOutputStream os (f);
            juce::PNGImageFormat().writeImageToStream (img, os);
        });
        juce::Thread::sleep (200);
    };
    // audio on its own thread (like a host), message loop on the main thread
    std::thread audio ([&]
    {
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        for (int64_t p = 0; p + 256 < s.length; p += 256)
        {
            ph.pos = p;
            for (int c = 0; c < 2; ++c) buf.copyFrom (c, 0, s.mics[sig::Snare].data() + p, 256);
            midi.clear();
            proc->processBlock (buf, midi);
            std::this_thread::sleep_for (std::chrono::microseconds (900));
            if (p == (s.length * 6 / 10) / 256 * 256)
            {
                juce::Thread::sleep (300);
                snap ("REPLACE", "replace_live.png");
                // hover the first visible trigger point, then open Advanced
                juce::MessageManager::callAsync ([&]
                {
                    std::function<juce::Component* (juce::Component*)> findLane = [&] (juce::Component* c) -> juce::Component*
                    {
                        if (dynamic_cast<thruma::ui::TrackerLane*> (c) != nullptr) return c;
                        for (auto* k : c->getChildren()) if (auto* f = findLane (k)) return f;
                        return nullptr;
                    };
                    if (auto* lane = dynamic_cast<thruma::ui::TrackerLane*> (findLane (ed.get())))
                        for (auto& tp2 : tp->getTriggerPoints())
                        {
                            const float x = lane->posToX ((double) tp2.position);
                            if (x < 40.0f || x > (float) lane->getWidth() - 40.0f) continue;
                            const juce::Point<float> at (x, lane->pointY (tp2.peakDb));
                            auto src = juce::Desktop::getInstance().getMainMouseSource();
                            lane->mouseMove (juce::MouseEvent (src, at, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, lane, lane, juce::Time::getCurrentTime(), at,
                                                               juce::Time::getCurrentTime(), 0, false));
                            break;
                        }
                });
                snap ({}, "replace_hover.png");
                juce::MessageManager::callAsync ([&]
                {
                    std::function<void (juce::Component*)> click = [&] (juce::Component* c)
                    {
                        if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == "Advanced...")
                        {
                            b->setToggleState (true, juce::dontSendNotification);
                            b->onClick();
                            return;
                        }
                        for (auto* k : c->getChildren()) click (k);
                    };
                    click (ed.get());
                });
                juce::Thread::sleep (400);
                snap ({}, "replace_advanced.png");
                juce::MessageManager::callAsync ([&]
                {
                    std::function<void (juce::Component*)> close = [&] (juce::Component* c)
                    {
                        if (auto* b = dynamic_cast<juce::TextButton*> (c); b != nullptr && b->getButtonText() == "Advanced...")
                        { b->setToggleState (false, juce::dontSendNotification); b->onClick(); return; }
                        for (auto* k : c->getChildren()) close (k);
                    };
                    close (ed.get());
                });
                juce::Thread::sleep (300);
            }
            if (p == (s.length * 7 / 10) / 256 * 256) { tp->auditionSlot (thruma::kNumInstruments, 0.9f); tp->auditionSlot ((int) thruma::Instrument::Kick, 0.9f); snap ("PLAYER", "player.png"); }
            if (p == (s.length * 8 / 10) / 256 * 256) { snap ("SAMPLES", "samples.png"); }
            if (p == (s.length * 85 / 100) / 256 * 256)
            {
                juce::Array<juce::File> many;
                for (int k = 0; k < 60; ++k)
                {
                    juce::File f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("thruma_shot").getChildFile ("Ride_long_" + juce::String (k) + ".wav");
                    if (! f.existsAsFile())
                    {
                        juce::AudioBuffer<float> b (2, 48000 * 12);
                        for (int i = 0; i < b.getNumSamples(); ++i) { const float v = 0.3f * std::sin ((float) i * (0.02f + 0.0003f * (float) k)) * std::exp (-(float) i / 90000.0f); b.setSample (0, i, v); b.setSample (1, i, v); }
                        juce::WavAudioFormat fmt;
                        std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (new juce::FileOutputStream (f), 44100.0, 2, 24, {}, 0));
                        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
                    }
                    many.add (f);
                }
                tp->runKitJob ([many] (thruma::KitManager& km) { km.setProgressTotal (many.size()); km.assignFiles ((int) thruma::Instrument::Ride, many); });
                juce::Thread::sleep (450);
                snap ("PLAYER", "loading.png");
            }
            if (p == (s.length * 9 / 10) / 256 * 256)
            {
                tp->getKitManager().setMix ((int) thruma::Instrument::Snare, -3.0f, -0.3f, false, false);
                tp->getKitManager().setMix ((int) thruma::Instrument::Kick, 2.0f, 0.0f, false, true);
                tp->syncMixer();
                snap ("MIXER", "mixer.png");
            }
        }
        juce::Thread::sleep (300);
        juce::MessageManager::callAsync ([&] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    audio.join();
    ed.reset();
    proc->releaseResources();
    return 0;
}
