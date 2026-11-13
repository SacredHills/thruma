#pragma once

#include "TrackerLane.h"
#include "Widgets.h"

namespace thruma::ui
{

/** Drag this into the DAW to drop the detected performance as a MIDI clip. */
class MidiDragSource : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit MidiDragSource (ThrumaProcessor& p) : proc (p) { setMouseCursor (juce::MouseCursor::DraggingHandCursor); }
    void paint (juce::Graphics&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragging = false; repaint(); }
    std::function<void (const juce::String&)> onMessage;

private:
    ThrumaProcessor& proc;
    bool dragging = false;
};

class ReplacePage : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit ReplacePage (ThrumaProcessor& p);
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void timerCallback() override;
    void saveMidi();
    int currentSlot() const;

    ThrumaProcessor& proc;
    Panel trackPanel { "Track" }, controlPanel;
    ParamCombo instrument, sample;
    juce::Label hits, info;
    TrackerLane lane;
    ParamSlider sensitivity, velocity, blend, output;
    MidiDragSource dragMidi;
    juce::TextButton saveBtn { "Save MIDI..." }, clearBtn { "Clear" }, finalizeBtn { "Finalize" }, importBtn { "Import Audio..." };
    void importFile (const juce::File& f);
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String message;
    juce::uint32 messageTime = 0;
    void showMessage (const juce::String& m) { message = m; messageTime = juce::Time::getMillisecondCounter(); timerCallback(); }
};

} // namespace thruma::ui
