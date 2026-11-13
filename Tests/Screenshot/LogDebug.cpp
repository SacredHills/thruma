#include "../../Source/Plugin/PluginProcessor.h"
#include <random>
struct PH : juce::AudioPlayHead { int64_t pos = 0; juce::Optional<PositionInfo> getPosition() const override { PositionInfo p; p.setIsPlaying (true); p.setTimeInSamples (pos); return p; } };
int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto* proc = dynamic_cast<thruma::ThrumaProcessor*> (createPluginFilter());
    PH ph; proc->setPlayHead (&ph); proc->prepareToPlay (48000.0, 512);
    std::mt19937 rng (1); std::uniform_real_distribution<float> u (-1, 1);
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    for (int64_t pos = 480000; pos < 480000 + 12 * 48000; pos += 512)
    {
        ph.pos = pos;
        for (int i = 0; i < 512; ++i) { const int64_t n = pos + i; const double t = (double) (n % 12000) / 48000.0;
            const float v = (float) (0.6 * std::exp (-t / 0.05)) * u (rng) + 0.0005f * u (rng); buf.setSample (0, i, v); buf.setSample (1, i, v); }
        midi.clear();
        proc->processBlock (buf, midi);
        if ((pos / 512) % 8 == 0) { juce::Thread::sleep (2); juce::Timer::callPendingTimersSynchronously(); }
    }
    for (int i = 0; i < 5; ++i) { juce::Thread::sleep (60); juce::Timer::callPendingTimersSynchronously(); }
    std::printf ("hitCounter=%d logged=%d\n", proc->hitCounter.load(), (int) proc->getTriggers().size());
    for (auto& t : proc->getTriggers().getAll()) std::printf ("%lld ", (long long) (t.position - 480000) / 12000);
    std::printf ("\n");
    auto& sc = proc->getScope();
    std::printf ("events=%lld\n", (long long) sc.eventsWritten.load());
    for (int64_t e = 0; e < sc.eventsWritten.load() && e < 60; ++e)
        std::printf ("ev %lld v=%.2f\n", (long long) sc.eventPos[(size_t) e].load(), sc.eventVel[(size_t) e].load());
    delete proc;
}
