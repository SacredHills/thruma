#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace thruma::ui
{

class SamplesPage;

/** One velocity layer: velocity range + its round-robin samples. Accepts dropped files. */
class LayerRow : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    LayerRow (SamplesPage& owner, int layerIndex);
    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }

private:
    SamplesPage& page;
    int layer;
    juce::Label title;
    juce::Slider velLo, velHi;
    juce::TextButton removeBtn { "Remove" };
    juce::OwnedArray<juce::TextButton> chips;
    bool dragOver = false;
};

class SamplesPage : public juce::Component,
                    public juce::FileDragAndDropTarget,
                    private juce::ListBoxModel,
                    private juce::FileBrowserListener
{
public:
    explicit SamplesPage (ThrumaProcessor& p);
    ~SamplesPage() override;
    void resized() override;
    void paint (juce::Graphics&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override;

    // used by LayerRow
    ThrumaProcessor& getProcessor() { return proc; }
    int getSelectedInstrument() const { return selectedInstrument; }
    void kitChanged();          // rebuild audio kit + refresh UI
    void refreshDetail();

private:
    // ListBoxModel
    int getNumRows() override { return kNumInstruments; }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    // FileBrowserListener
    void selectionChanged() override {}
    void fileClicked (const juce::File& f, const juce::MouseEvent&) override;
    void fileDoubleClicked (const juce::File& f) override;
    void browserRootChanged (const juce::File&) override {}

    juce::Array<juce::File> browserSelection() const;
    void addFiles (const juce::Array<juce::File>& files);

    ThrumaProcessor& proc;
    int selectedInstrument = (int) Instrument::Snare;

    Panel browserPanel { "WAV Browser" }, kitPanel { "Kit" }, detailPanel;
    juce::WildcardFileFilter filter { "*.wav;*.wave;*.aif;*.aiff;*.flac", "*", "Audio files" };
    std::unique_ptr<juce::FileBrowserComponent> browser;
    juce::TextButton addBtn { "Add to instrument" }, importBtn { "Import Folder..." }, loadBtn { "Load Kit..." },
                     saveBtn { "Save Kit..." }, clearAllBtn { "New Kit" };
    juce::ListBox instrumentList;
    juce::Label kitName, detailTitle, hint;
    juce::Slider midiNote;
    juce::Label midiLabel;
    juce::ComboBox rrMode;
    juce::TextButton autoVelBtn { "Auto velocity" }, clearInstBtn { "Clear" };
    juce::Viewport layerViewport;
    juce::Component layerHolder;
    juce::OwnedArray<LayerRow> rows;
    std::unique_ptr<juce::FileChooser> chooser;
};

} // namespace thruma::ui
