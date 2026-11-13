// Dev tool: runs the plugin's processor on a synthetic drum session with a moving transport and
// writes PNG snapshots of the editor pages. Usage: ThrumaScreenshot <out-dir>
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
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditorAndMakeActive());
    ed->setSize (1080, 760);
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
            if (p == (s.length * 7 / 10) / 256 * 256)
            {
                juce::Thread::sleep (300);
                juce::MessageManager::callAsync ([&]
                {
                    auto img = ed->createComponentSnapshot (ed->getLocalBounds());
                    juce::File f = out.getChildFile ("replace_live.png");
                    f.deleteFile();
                    juce::FileOutputStream os (f);
                    juce::PNGImageFormat().writeImageToStream (img, os);
                });
                juce::Thread::sleep (300);
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
