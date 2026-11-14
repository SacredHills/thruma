// Small parameter-bound controls in the Thruma style.
#pragma once

#include "LookAndFeel.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace thruma::ui
{

using APVTS = juce::AudioProcessorValueTreeState;

/** "Label ───●──── value" row bound to a parameter. */
class ParamSlider : public juce::Component
{
public:
    ParamSlider (APVTS& state, const juce::String& paramId, const juce::String& label, const juce::String& tooltip = {});
    void resized() override;
    void paint (juce::Graphics&) override;
    juce::Slider& getSlider() { return slider; }

private:
    juce::Label name;
    juce::Slider slider;
    std::unique_ptr<APVTS::SliderAttachment> attachment;
};

/** Rotary knob with a caption above and the value below, bound to a parameter. */
class ParamKnob : public juce::Component
{
public:
    ParamKnob (APVTS& state, const juce::String& paramId, const juce::String& label, const juce::String& tooltip = {});
    void resized() override;
    juce::Slider& getSlider() { return slider; }

private:
    juce::Label caption;
    juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    std::unique_ptr<APVTS::SliderAttachment> attachment;
};

class ParamToggle : public juce::Component
{
public:
    ParamToggle (APVTS& state, const juce::String& paramId, const juce::String& label, const juce::String& tooltip = {});
    void resized() override { button.setBounds (getLocalBounds()); }
    juce::ToggleButton& getButton() { return button; }

private:
    juce::ToggleButton button;
    std::unique_ptr<APVTS::ButtonAttachment> attachment;
};

/** Small caption above a combo box bound to a choice parameter. */
class ParamCombo : public juce::Component
{
public:
    ParamCombo (APVTS& state, const juce::String& paramId, const juce::String& label, const juce::String& tooltip = {});
    void resized() override;
    juce::ComboBox& getCombo() { return combo; }

private:
    juce::Label caption;
    juce::ComboBox combo;
    std::unique_ptr<APVTS::ComboBoxAttachment> attachment;
    std::unique_ptr<juce::ParameterAttachment> ordered;   // drum lists: shown in display order (item id = choice + 1)
};

/** Rounded panel with an optional title. */
class Panel : public juce::Component
{
public:
    explicit Panel (juce::String title = {}) : heading (std::move (title)) { setInterceptsMouseClicks (false, true); }
    void paint (juce::Graphics&) override;
    /** Inner area in the parent's coordinate space (controls are siblings placed on top of the panel). */
    juce::Rectangle<int> getContentBounds() const;
    void setTitle (const juce::String& t) { heading = t; repaint(); }

private:
    juce::String heading;
};

/** Vertical peak meter fed from an atomic. */
class LevelMeter : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setLevel (float linear) { level = juce::jmax (linear, level * 0.85f); repaint(); }
    void paint (juce::Graphics&) override;

private:
    float level = 0.0f;
};

/** Flashes on every detected hit (brightness = velocity). */
class HitLed : public juce::Component, public juce::SettableTooltipClient
{
public:
    void trigger (float velocity) { brightness = 0.35f + 0.65f * velocity; repaint(); }
    void decay() { if (brightness > 0.0f) { brightness = juce::jmax (0.0f, brightness - 0.12f); repaint(); } }
    void paint (juce::Graphics&) override;

private:
    float brightness = 0.0f;
};

/** Grid helper: lays out children in rows. */
void layoutRows (juce::Rectangle<int> area, std::initializer_list<juce::Component*> comps, int rowHeight, int gap);

} // namespace thruma::ui
