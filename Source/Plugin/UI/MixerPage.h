// MIXER: a channel strip per drum and pad - volume, pan, mute, solo and a level meter.
#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace thruma::ui
{

class MixerStrip : public juce::Component
{
public:
    MixerStrip (ThrumaProcessor& p, int slot);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();                 // from the kit model
    void tick (float peak, float hit);
    int getSlot() const { return slot; }

private:
    void push();
    ThrumaProcessor& proc;
    const int slot;
    juce::Label name;
    juce::Slider pan { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
    juce::TextButton mute { "M" }, solo { "S" };
    float meter = 0.0f, hitGlow = 0.0f;
    bool hasSamples = false;
};

class MixerPage : public juce::Component, private juce::Timer
{
public:
    explicit MixerPage (ThrumaProcessor& p);
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh();                 // the kit changed (new pads, names, samples)

private:
    void timerCallback() override;
    void rebuild();
    ThrumaProcessor& proc;
    juce::Viewport viewport;
    juce::Component holder;
    juce::OwnedArray<MixerStrip> strips;
    juce::TextButton resetBtn { "Reset Mixer" };
    std::array<uint32_t, kNumKitSlots> seen {};
    int shownPads = -1;
};

} // namespace thruma::ui
