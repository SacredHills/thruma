#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace thruma::ui
{

namespace colours
{
    // black & white theme
    const juce::Colour background  { 0xff080808 };
    const juce::Colour panel       { 0xff121212 };
    const juce::Colour panelRaised { 0xff1c1c1c };
    const juce::Colour outline     { 0xff343434 };
    const juce::Colour text        { 0xfff0f0f0 };
    const juce::Colour textDim     { 0xff8a8a8a };
    const juce::Colour accent      { 0xffffffff };   // white: controls, waveform, hits lighting up
    const juce::Colour trigger     { 0xffb8b8b8 };   // light grey: hits, solo
    const juce::Colour selection   { 0xffffffff };
    const juce::Colour danger      { 0xffd8d8d8 };   // missing files, clipping, mute
    const juce::Colour record      { 0xffffffff };   // playhead
}

class ThrumaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ThrumaLookAndFeel();

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getPopupMenuFont() override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& bg, bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical, int thumbStart,
                        int thumbSize, bool over, bool down) override;
    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
};

} // namespace thruma::ui
