// PLAYER: the kit as a picture. Click a drum to play it, right-click to load / manage its samples, drop audio
// files on it; drums light up as they are played (tracking, MIDI, MIDI files). Extra kit sounds are pads below.
#pragma once

#include "../PluginProcessor.h"
#include "Widgets.h"

namespace thruma::ui
{

class PlayerPage : public juce::Component,
                   public juce::FileDragAndDropTarget,
                   public juce::TooltipClient,
                   private juce::Timer
{
public:
    explicit PlayerPage (ThrumaProcessor& p);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override { dropSlot = -1; repaint(); }
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    juce::String getTooltip() override;
    /** The right-click menu of a drum / pad (public for tests). */
    juce::PopupMenu buildMenu (int slot, std::vector<juce::File>& fileIds);

    /** Editor hooks: open a slot on the SAMPLES page; the kit changed (refresh other pages). */
    std::function<void (int slot)> onEditSlot;
    std::function<void()> onKitChanged;

private:
    void timerCallback() override;
    juce::Rectangle<float> imageBounds() const;      // the room picture (16:9)
    juce::Rectangle<float> kitBounds() const;        // the drums inside it
    juce::Rectangle<float> slotBounds (int slot) const;
    void buildMasks();
    juce::Rectangle<float> padBounds (int pad) const; // pad index 0.. (shown pads + the "+" pad)
    int numPadTiles() const;
    int padsPerRow() const;
    int padRows() const;
    int padsHeight() const;
    int slotAt (juce::Point<float> p) const;
    void showMenu (int slot);
    void showBuiltMenu (juce::PopupMenu& menu, int slot, std::vector<juce::File> fileIds);
    void chooseFiles (int slot, bool add, bool folder);
    void assign (int slot, const juce::Array<juce::File>& files, bool add);
    void setMidiNote (int slot, int note);
    void chooseTde (int slot, bool save);
    void loadTde (int slot, const juce::File& f);
    void kitEdited();     // a quick model edit: refresh, rebuild the audio kit in the background
    void kitLoaded();     // after a background load: refresh
    juce::String describe (int slot) const;

    ThrumaProcessor& proc;
    juce::Image room, drums;
    // which drum each kit-picture pixel belongs to (kNone = stand / background), at the picture's resolution
    static constexpr uint8_t kNone = 255;
    std::vector<uint8_t> owner;
    int kitW = 0, kitH = 0;
    struct Mask { juce::Image lit, glow; juce::Rectangle<int> area, glowArea; };   // in picture pixels
    std::array<Mask, kNumInstruments> masks;
    std::array<float, kNumKitSlots> glow {};
    std::array<uint32_t, kNumKitSlots> seen {};
    int hover = -1, dropSlot = -1;
    juce::TextButton importKitBtn { "Import Kit Folder..." }, loadKitBtn { "Load Kit..." };
    juce::Label kitLabel;
    std::unique_ptr<juce::FileChooser> chooser;
};

} // namespace thruma::ui
