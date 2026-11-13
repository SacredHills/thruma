#pragma once

#include "WaveformEditor.h"
#include "Widgets.h"

namespace thruma::ui
{

class ReplacePage : public juce::Component,
                    public juce::FileDragAndDropTarget,
                    private juce::Timer
{
public:
    explicit ReplacePage (ThrumaProcessor& p);
    void resized() override;
    void paint (juce::Graphics&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

private:
    void timerCallback() override;
    void updateStatus();
    void exportMidi();

    ThrumaProcessor& proc;

    // waveform toolbar
    juce::TextButton analyzeBtn { "Re-detect" }, clearBtn { "Clear" }, undoBtn { "Undo" }, redoBtn { "Redo" },
                     fitBtn { "Fit" }, zoomInBtn { "+" }, zoomOutBtn { "-" }, exportBtn { "Export MIDI" }, auditionBtn { "Audition" };
    juce::Label status, liveBadge;
    WaveformEditor wave;

    // main controls
    Panel mainPanel { "Replace" };
    ParamToggle repOn;
    ParamCombo instrument, sample, source, mode;
    ParamSlider sensitivity, velResp, blend, output;
    juce::TextButton advancedBtn { "Advanced" };
    juce::Label sampleInfo;

    // advanced
    Panel advPanel { "Advanced" };
    ParamSlider ghost, retrig, dynRange, gain, offset, align;
    ParamToggle doubleHits, focus, polarity, autoAlign, midiOut;
    ParamCombo latency;
    bool showAdvanced = false;

    std::unique_ptr<juce::FileChooser> chooser;
};

} // namespace thruma::ui
