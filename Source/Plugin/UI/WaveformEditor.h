// Interactive waveform / trigger editor (non-destructive: the recording is never modified).
#pragma once

#include "../PluginProcessor.h"
#include "LookAndFeel.h"

#include <set>

namespace thruma::ui
{

class WaveformEditor : public juce::Component,
                       private juce::Timer,
                       private juce::ScrollBar::Listener
{
public:
    explicit WaveformEditor (ThrumaProcessor& p);
    ~WaveformEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    void zoomToFit();
    void zoomBy (double factor, float anchorX);
    void deleteSelection();
    int getNumSelected() const { return (int) selected.size(); }

    std::function<void()> onEdited;   // trigger list changed / selection changed

private:
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;

    juce::Rectangle<int> waveArea() const;
    int64_t dataOrigin() const;
    int64_t dataLength() const;
    bool minMax (int64_t from, int64_t to, float& lo, float& hi) const;
    void updatePeakCache();
    void updateScrollBar();
    double xToPos (float x) const;
    float posToX (double pos) const;
    int hitTest (juce::Point<float> p) const;
    const std::vector<Trigger>& displayed() const;
    float estimateVelocity (int64_t pos) const;
    void showContextMenu (int index);

    ThrumaProcessor& proc;
    juce::ScrollBar hScroll { false };

    double viewStart = 0.0, spp = 256.0;   // timeline samples at left edge, samples per pixel
    bool hasFitted = false;

    std::vector<float> peakCache;          // min/max pairs per kBin samples of the capture
    int64_t peakCacheLen = 0;
    float dataPeak = 0.0f;
    static constexpr int kBin = ThrumaProcessor::kOverviewBin;

    std::set<uint32_t> selected;
    enum class Drag { None, Move, Velocity, Rubber } drag = Drag::None;
    std::vector<Trigger> working, dragOriginal;
    bool editing = false, dragChanged = false;
    double dragStartPos = 0.0;
    float dragStartY = 0.0f;
    juce::Rectangle<float> rubber;
    int hoverIndex = -1;
    juce::uint32 lastInteractionMs = 0;
    size_t lastTriggerCount = (size_t) -1;
};

} // namespace thruma::ui
