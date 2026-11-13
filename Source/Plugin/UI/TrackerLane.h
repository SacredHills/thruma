// Tracker-style track lane: the drum's detection level over the song with the trigger threshold
// (drag it), and a MIDI velocity lane below it where the hits are edited.
#pragma once

#include "../PluginProcessor.h"
#include "LookAndFeel.h"

#include <set>

namespace thruma::ui
{

class TrackerLane : public juce::Component, private juce::Timer, private juce::ScrollBar::Listener
{
public:
    explicit TrackerLane (ThrumaProcessor& p);
    ~TrackerLane() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    void zoomToFit();

private:
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double start) override;

    juce::Rectangle<int> audioArea() const;
    juce::Rectangle<int> midiArea() const;
    double xToPos (float x) const { return viewStart + (double) x * spp; }
    float posToX (double pos) const { return (float) ((pos - viewStart) / spp); }
    float dbToY (float db) const;
    float yToDb (float y) const;
    float velToY (float v) const;
    int hitTestBar (juce::Point<float> p, bool& onTop) const;
    const std::vector<Trigger>& shown() const { return editing ? working : proc.getTriggers().getAll(); }
    void updateScrollBar();
    void setThresholdDb (float db);
    void deleteSelection();
    int64_t historyStart() const;
    int64_t historyEnd() const;

    ThrumaProcessor& proc;
    juce::ScrollBar hScroll { false };
    double viewStart = 0.0, spp = 400.0;
    bool initialised = false;
    juce::uint32 lastInteractionMs = 0;

    std::set<uint32_t> selected;
    enum class Drag { None, Move, Velocity, Threshold, Rubber } drag = Drag::None;
    std::vector<Trigger> working, original;
    bool editing = false, changed = false;
    double dragStartPos = 0.0;
    juce::Rectangle<float> rubber;
    bool hoverThreshold = false;
};

} // namespace thruma::ui
