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
    void paintOverChildren (juce::Graphics&) override;
    /** Height this row needs at a given width (all its sample chips visible). */
    int heightForWidth (int width) const;
    /** Light up the chip of the sample that just played (by SampleData::pathHash). */
    void flash (uint64_t sampleHash, float velocity);
    bool decay();   // true while still lit

private:
    int layoutChips (juce::Rectangle<int> area, bool apply) const;   // returns the bottom of the last line
    std::vector<uint64_t> hashes;   // per chip
    int flashIdx = -1;
    float flashLevel = 0.0f, flashVel = 0.0f;
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
                    private juce::Timer,
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
    /** Show a drum or pad (kit slot). */
    void selectSlot (int slot);

private:
    void timerCallback() override;
    // ListBoxModel
    int getNumRows() override;
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
    std::array<uint32_t, kNumKitSlots> seenHits {};
    std::array<float, kNumKitSlots> rowFlash {};
    std::unique_ptr<juce::FileChooser> chooser;
};

} // namespace thruma::ui
