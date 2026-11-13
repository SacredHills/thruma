// Live scrolling view of what the trigger detector "sees": the drum's level, the trigger threshold,
// the measured bleed floor, accepted hits and rejected (bleed) candidates. Drag the threshold line
// to set Sensitivity.
#pragma once

#include "../PluginProcessor.h"
#include "LookAndFeel.h"

namespace thruma::ui
{

class LiveScope : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit LiveScope (ThrumaProcessor& p);
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void timerCallback() override;
    float dbToY (float db) const;
    float yToDb (float y) const;
    float currentThresholdDb() const;
    void setThresholdDb (float db);

    ThrumaProcessor& proc;
    static constexpr float kMinDb = -72.0f, kMaxDb = 0.0f;
    static constexpr double kSeconds = 4.0;
    bool dragging = false, overThreshold = false;
    int64_t lastWritten = -1;
};

} // namespace thruma::ui
