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

    /** Editor hooks: open a slot on the SAMPLES page; the kit changed (refresh other pages). */
    std::function<void (int slot)> onEditSlot;
    std::function<void()> onKitChanged;

private:
    struct Spot { int slot; bool ellipse; float a, b, c, d; };   // image pixels: ellipse cx,cy,rx,ry / rect x1,y1,x2,y2
    void timerCallback() override;
    juce::Rectangle<float> imageBounds() const;
    juce::Path spotPath (const Spot&) const;
    juce::Path slotPath (int slot) const;            // all spots of a drum, in component coordinates
    juce::Rectangle<float> padBounds (int pad) const; // pad index 0.. (shown pads + the "+" pad)
    int numPadTiles() const;
    int slotAt (juce::Point<float> p) const;
    void showMenu (int slot);
    void chooseFiles (int slot, bool add, bool folder);
    void assign (int slot, const juce::Array<juce::File>& files, bool add);
    void kitEdited();
    juce::String describe (int slot) const;

    ThrumaProcessor& proc;
    juce::Image photo;
    std::vector<Spot> spots;
    std::array<float, kNumKitSlots> glow {};
    std::array<uint32_t, kNumKitSlots> seen {};
    int hover = -1, dropSlot = -1;
    juce::TextButton importKitBtn { "Import Kit Folder..." }, loadKitBtn { "Load Kit..." };
    juce::Label kitLabel;
    std::unique_ptr<juce::FileChooser> chooser;
};

} // namespace thruma::ui
