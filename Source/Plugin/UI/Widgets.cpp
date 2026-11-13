#include "Widgets.h"

namespace thruma::ui
{

ParamSlider::ParamSlider (APVTS& state, const juce::String& paramId, const juce::String& label, const juce::String& tooltip)
{
    name.setText (label, juce::dontSendNotification);
    name.setColour (juce::Label::textColourId, colours::textDim);
    name.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (name);

    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 20);
    slider.setColour (juce::Slider::textBoxTextColourId, colours::text);
    slider.setTooltip (tooltip);
    name.setTooltip (tooltip);
    addAndMakeVisible (slider);
    attachment = std::make_unique<APVTS::SliderAttachment> (state, paramId, slider);
    slider.setDoubleClickReturnValue (true, (double) state.getParameter (paramId)->convertFrom0to1 (state.getParameter (paramId)->getDefaultValue()));
}

void ParamSlider::resized()
{
    auto r = getLocalBounds();
    name.setBounds (r.removeFromLeft (juce::jmin (130, r.getWidth() / 3)));
    slider.setBounds (r);
}

void ParamSlider::paint (juce::Graphics&) {}

ParamToggle::ParamToggle (APVTS& state, const juce::String& paramId, const juce::String& label, const juce::String& tooltip)
{
    button.setButtonText (label);
    button.setTooltip (tooltip);
    addAndMakeVisible (button);
    attachment = std::make_unique<APVTS::ButtonAttachment> (state, paramId, button);
}

ParamCombo::ParamCombo (APVTS& state, const juce::String& paramId, const juce::String& label, const juce::String& tooltip)
{
    caption.setText (label.toUpperCase(), juce::dontSendNotification);
    caption.setColour (juce::Label::textColourId, colours::textDim);
    caption.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    addAndMakeVisible (caption);
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (paramId)))
        combo.addItemList (p->choices, 1);
    combo.setTooltip (tooltip);
    addAndMakeVisible (combo);
    attachment = std::make_unique<APVTS::ComboBoxAttachment> (state, paramId, combo);
}

void ParamCombo::resized()
{
    auto r = getLocalBounds();
    caption.setBounds (r.removeFromTop (16));
    combo.setBounds (r.removeFromTop (juce::jmin (28, r.getHeight())));
}

void Panel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (colours::panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (r, 8.0f, 1.0f);
    if (heading.isNotEmpty())
    {
        g.setColour (colours::textDim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (heading.toUpperCase(), getLocalBounds().reduced (14, 8).removeFromTop (16), juce::Justification::centredLeft);
    }
}

juce::Rectangle<int> Panel::getContentBounds() const
{
    auto r = getBoundsInParent().reduced (14, 10);   // in the parent's coordinates (siblings are laid out into it)
    if (heading.isNotEmpty()) r.removeFromTop (22);
    return r;
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colours::panelRaised);
    g.fillRoundedRectangle (r, 2.0f);
    const float db = juce::Decibels::gainToDecibels (level, -60.0f);
    const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    auto fill = r.withTrimmedTop (r.getHeight() * (1.0f - frac));
    g.setColour (db > -1.0f ? colours::danger : colours::accent);
    g.fillRoundedRectangle (fill, 2.0f);
}

void HitLed::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    const float d = juce::jmin (r.getWidth(), r.getHeight());
    auto c = r.withSizeKeepingCentre (d, d);
    g.setColour (colours::panelRaised);
    g.fillEllipse (c);
    if (brightness > 0.0f)
    {
        g.setColour (colours::trigger.withAlpha (brightness));
        g.fillEllipse (c.reduced (1.5f));
    }
}

void layoutRows (juce::Rectangle<int> area, std::initializer_list<juce::Component*> comps, int rowHeight, int gap)
{
    for (auto* c : comps)
    {
        if (c == nullptr) continue;
        c->setBounds (area.removeFromTop (rowHeight));
        area.removeFromTop (gap);
    }
}

} // namespace thruma::ui
