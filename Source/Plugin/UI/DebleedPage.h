#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace thruma::ui
{

/** Live per-band gain-reduction display. */
class ReductionDisplay : public juce::Component
{
public:
    explicit ReductionDisplay (ThrumaProcessor& p) : proc (p) {}
    void paint (juce::Graphics&) override;
    void update();

private:
    ThrumaProcessor& proc;
    std::array<float, DebleedProcessor::kMeterBands> shown {};
};

class DebleedPage : public juce::Component, private juce::Timer
{
public:
    explicit DebleedPage (ThrumaProcessor& p);
    void resized() override;

private:
    void timerCallback() override;
    void refreshReferences();
    void referenceChosen();

    ThrumaProcessor& proc;
    ParamToggle dbOn;
    juce::Label status;
    ReductionDisplay display;
    Panel panel { "Debleed" };
    ParamCombo instrument;
    juce::Label refLabel;
    juce::ComboBox reference;
    ParamSlider amount, sensitivity, attack, release;
    juce::StringArray lastItems;
    int tick = 0;
};

} // namespace thruma::ui
