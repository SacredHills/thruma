#pragma once

#include "PluginProcessor.h"
#include "UI/DebleedPage.h"
#include "UI/LookAndFeel.h"
#include "UI/ReplacePage.h"
#include "UI/SamplesPage.h"
#include "UI/PlayerPage.h"
#include "UI/MixerPage.h"

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

    static constexpr int kNumTabs = 5;     // index: 0 DEBLEED, 1 REPLACE, 2 SAMPLES, 3 PLAYER, 4 MIXER (saved in projects)
    std::array<juce::TextButton, kNumTabs> tabs;
    ui::PlayerPage playerPage;
    ui::MixerPage mixerPage;
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
