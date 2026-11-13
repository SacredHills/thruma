#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace thruma::ui
{

namespace colours
{
    const juce::Colour background  { 0xff111317 };
    const juce::Colour panel       { 0xff181b20 };
    const juce::Colour panelRaised { 0xff1f2329 };
    const juce::Colour outline     { 0xff2b3037 };
    const juce::Colour text        { 0xffe4e7eb };
    const juce::Colour textDim     { 0xff8a929c };
    const juce::Colour accent      { 0xff45c7b8 };   // teal: controls, waveform
    const juce::Colour trigger     { 0xfff2a545 };   // amber: hits
    const juce::Colour selection   { 0xffffffff };
    const juce::Colour danger      { 0xffef6f6c };
    const juce::Colour record      { 0xffe5534b };
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
