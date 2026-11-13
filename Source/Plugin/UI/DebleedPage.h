#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace thruma::ui
{

/** Live per-band gain-reduction display (visual feedback instead of more knobs). */
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
    void refreshLinkSources();

    ThrumaProcessor& proc;
    ParamToggle dbOn;
    juce::TextButton analyzeBtn { "ANALYZE" }, advancedBtn { "Advanced" };
    juce::Label report, linkStatus, shareLabel;
    ReductionDisplay display;

    Panel mainPanel { "Debleed" };
    ParamCombo instrument, reference;
    juce::Label linkedLabel;
    juce::ComboBox linkedSource;
    juce::TextEditor shareName;
    ParamSlider amount, sensitivity, attack, release;

    Panel advPanel { "Advanced" };
    ParamSlider maxRed, smooth, lowProtect, decay;
    ParamToggle adaptive;
    bool showAdvanced = false;
    int tick = 0;
    juce::StringArray lastSources;
};

} // namespace thruma::ui
