// Loads the built Thruma VST3 through JUCE's VST3 hosting layer and exercises it like a DAW would:
// bus layouts, sample rates, odd block sizes, latency/PDC correctness, automation sweeps, MIDI,
// state save/restore across instances and editor creation. Exit code 0 = all checks passed.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <cmath>
#include <iostream>
#include <random>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
    if (! ok) ++failures;
}

juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& p, const juce::String& name)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (128) == name) return prm;
    return nullptr;
}

void setParamPlain (juce::AudioPluginInstance& p, const juce::String& name, float normalised)
{
    if (auto* prm = findParam (p, name)) prm->setValueNotifyingHost (normalised);
    else check (false, "parameter '" + name + "' exists");
}

/** Decaying noise bursts every 250 ms (snare-like). */
float testSignal (int64_t n, double sr, std::mt19937& rng)
{
    const auto period = (int64_t) (0.25 * sr);
    const double t = (double) (n % period) / sr;
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    return (float) (0.6 * std::exp (-t / 0.05)) * u (rng) + 0.0005f * u (rng);
}

struct RunResult { bool finite = true; double energy = 0.0; int midiNotes = 0; };

RunResult run (juce::AudioPluginInstance& p, double sr, int block, double seconds, bool sendMidi)
{
    RunResult r;
    std::mt19937 rng (1);
    const int chans = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buf (chans, block);
    juce::MidiBuffer midi;
    const auto total = (int64_t) (seconds * sr);
    for (int64_t pos = 0; pos < total; pos += block)
    {
        buf.clear();
        for (int i = 0; i < block; ++i)
        {
            const float v = testSignal (pos + i, sr, rng);
            for (int c = 0; c < p.getTotalNumInputChannels(); ++c) buf.setSample (c, i, v);
        }
        midi.clear();
        if (sendMidi && (pos / block) % 20 == 0) midi.addEvent (juce::MidiMessage::noteOn (10, 38, (juce::uint8) 100), 0);
        p.processBlock (buf, midi);
        for (const auto m : midi) if (m.getMessage().isNoteOn()) ++r.midiNotes;
        for (int c = 0; c < p.getTotalNumOutputChannels(); ++c)
            for (int i = 0; i < block; ++i)
            {
                const float v = buf.getSample (c, i);
                if (! std::isfinite (v)) r.finite = false;
                r.energy += (double) v * v;
            }
    }
    return r;
}

/** Impulse in -> measure where it comes out; must equal the reported latency (layer mode, empty kit). */
int measureDelay (juce::AudioPluginInstance& p, double sr, int block)
{
    const int chans = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buf (chans, block);
    juce::MidiBuffer midi;
    const int impulseAt = 3000;
    int found = -1;
    float best = 0.0f;
    for (int64_t pos = 0; pos < (int64_t) (sr * 0.5); pos += block)
    {
        buf.clear();
        if (impulseAt >= pos && impulseAt < pos + block)
            for (int c = 0; c < p.getTotalNumInputChannels(); ++c) buf.setSample (c, (int) (impulseAt - pos), 0.5f);
        p.processBlock (buf, midi);
        for (int i = 0; i < block; ++i)
            if (std::abs (buf.getSample (0, i)) > best) { best = std::abs (buf.getSample (0, i)); found = (int) (pos + i); }
    }
    return found - impulseAt;
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "usage: ThrumaHostValidator <path/to/Thruma.vst3> [--no-editor]" << std::endl;
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    const bool testEditor = ! (argc > 2 && juce::String (argv[2]) == "--no-editor");

    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> types;
    vst3.findAllTypesForFile (types, juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]).getFullPathName());
    check (types.size() == 1, "VST3 bundle contains exactly one plugin");
    if (types.isEmpty()) return 1;
    const auto desc = *types[0];
    std::cout << "Loaded: " << desc.name << " " << desc.version << " by " << desc.manufacturerName << std::endl;

    juce::String err;
    auto plugin = vst3.createInstanceFromDescription (desc, 48000.0, 512, err);
    check (plugin != nullptr, "instantiate (" + err + ")");
    if (plugin == nullptr) return 1;
    check (plugin->getName() == "Thruma", "name");
    check (plugin->acceptsMidi() && plugin->producesMidi(), "MIDI in/out");
    check (plugin->getParameters().size() >= 30, "parameters exposed: " + juce::String (plugin->getParameters().size()));

    // --- stereo, several rates and odd block sizes ---
    for (auto [sr, bs] : { std::pair<double, int> { 44100.0, 64 }, { 48000.0, 512 }, { 96000.0, 1024 }, { 48000.0, 37 }, { 192000.0, 256 } })
    {
        plugin->releaseResources();
        plugin->setRateAndBufferSizeDetails (sr, bs);
        plugin->prepareToPlay (sr, bs);
        const auto r = run (*plugin, sr, bs, 2.0, true);
        check (r.finite && r.energy > 0.0, "process " + juce::String (sr) + " Hz / " + juce::String (bs) + " samples: finite, audible");
        check (plugin->getLatencySamples() > 0, "  reports latency (" + juce::String (plugin->getLatencySamples()) + " samples)");
    }

    // --- latency compensation correctness: layer mode, empty kit -> exact dry delay ---
    setParamPlain (*plugin, "Mode", 1.0f);   // Layer
    for (bool debleed : { false, true })
    {
        setParamPlain (*plugin, "Debleed On", debleed ? 1.0f : 0.0f);
        setParamPlain (*plugin, "Debleed Amount", 0.0f);
        // latency-affecting parameters are applied on the message thread
        for (int i = 0; i < 10; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        plugin->releaseResources();
        plugin->prepareToPlay (48000.0, 256);
        const int delay = measureDelay (*plugin, 48000.0, 256);
        check (delay == plugin->getLatencySamples(), juce::String ("measured delay == reported latency") + (debleed ? " (debleed on)" : "")
                                                         + ": " + juce::String (delay) + " vs " + juce::String (plugin->getLatencySamples()));
    }
    setParamPlain (*plugin, "Debleed Amount", 0.6f);

    // --- mono layout ---
    {
        auto layout = plugin->getBusesLayout();
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::mono();
        plugin->releaseResources();
        check (plugin->setBusesLayout (layout), "mono in/out layout accepted");
        plugin->prepareToPlay (48000.0, 128);
        const auto r = run (*plugin, 48000.0, 128, 1.5, false);
        check (r.finite && r.energy > 0.0, "mono processing");
    }

    // --- sidechain reference + debleed ---
    {
        auto layout = plugin->getBusesLayout();
        layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();
        if (layout.inputBuses.size() > 1) layout.inputBuses.getReference (1) = juce::AudioChannelSet::stereo();
        plugin->releaseResources();
        check (layout.inputBuses.size() > 1 && plugin->setBusesLayout (layout), "stereo + stereo sidechain layout accepted");
        setParamPlain (*plugin, "Debleed On", 1.0f);
        setParamPlain (*plugin, "Reference", 1.0f / 4.0f);   // Sidechain L
        for (int i = 0; i < 10; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        plugin->prepareToPlay (48000.0, 512);
        const auto r = run (*plugin, 48000.0, 512, 2.0, false);
        check (r.finite && r.energy > 0.0, "debleed with sidechain reference");
    }

    // --- automation sweep: random values on every parameter between blocks ---
    {
        plugin->prepareToPlay (48000.0, 256);
        std::mt19937 rng (7);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        bool finite = true;
        for (int round = 0; round < 40; ++round)
        {
            for (auto* prm : plugin->getParameters())
                if (prm->isAutomatable()) prm->setValueNotifyingHost (u (rng));
            finite &= run (*plugin, 48000.0, 256, 0.05, true).finite;
        }
        check (finite, "random automation of all parameters stays finite");
    }

    // --- state round trip into a fresh instance ---
    {
        setParamPlain (*plugin, "Sensitivity", 0.33f);
        setParamPlain (*plugin, "Blend", 0.42f);
        setParamPlain (*plugin, "Instrument", 3.0f / 10.0f);
        run (*plugin, 48000.0, 256, 0.02, false);   // deliver queued parameter changes to the processor
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        juce::MemoryBlock state;
        plugin->getStateInformation (state);
        check (state.getSize() > 100, "state saved (" + juce::String ((int) state.getSize()) + " bytes)");
        auto second = vst3.createInstanceFromDescription (desc, 48000.0, 512, err);
        second->setStateInformation (state.getData(), (int) state.getSize());
        second->prepareToPlay (48000.0, 256);
        run (*second, 48000.0, 256, 0.02, false);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        bool same = true;
        for (int i = 0; i < plugin->getParameters().size(); ++i)
        {
            // compare what the plugin displays: the source instance's host cache holds unquantised values
            const auto a = plugin->getParameters()[i]->getCurrentValueAsText();
            const auto b = second->getParameters()[i]->getCurrentValueAsText();
            if (a != b) { same = false; std::cout << "    differs: " << plugin->getParameters()[i]->getName (64) << " '" << a << "' vs '" << b << "'" << std::endl; }
        }
        check (same, "all parameter values restored in a new instance");
    }

    // --- editor ---
    if (testEditor && plugin->hasEditor())
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (plugin->createEditorAndMakeActive());
        check (ed != nullptr && ed->getWidth() >= 900 && ed->getHeight() >= 600, "editor opens at a sensible size");
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        ed.reset();
    }

    plugin->releaseResources();
    plugin.reset();
    std::cout << (failures == 0 ? "ALL HOST CHECKS PASSED" : "HOST CHECKS FAILED: " + juce::String (failures).toStdString()) << std::endl;
    return failures == 0 ? 0 : 1;
}
