#pragma once

#include "PluginProcessor.h"
#include "UI/DebleedPage.h"
#include "UI/LookAndFeel.h"
#include "UI/ReplacePage.h"
#include "UI/SamplesPage.h"

namespace thruma
{

class ThrumaEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ThrumaEditor (ThrumaProcessor&);
    ~ThrumaEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void showTab (int index);

    ThrumaProcessor& proc;
    ui::ThrumaLookAndFeel lnf;
    juce::TooltipWindow tooltips { this, 700 };

    std::array<juce::TextButton, 3> tabs;
    ui::ReplacePage replacePage;
    ui::DebleedPage debleedPage;
    ui::SamplesPage samplesPage;
    ui::LevelMeter inMeter, outMeter;
    ui::HitLed hitLed;
    juce::Label latencyLabel;
    int lastHits = 0;
    int currentTab = 0;
};

} // namespace thruma
