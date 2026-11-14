#include "LookAndFeel.h"

namespace thruma::ui
{

ThrumaLookAndFeel::ThrumaLookAndFeel()
{
    using namespace colours;
    setColourScheme ({ background, panel, panelRaised, outline, text, accent, juce::Colours::black, panelRaised, text });

    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::Label::textColourId, text);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::backgroundColourId, panelRaised);
    setColour (juce::ComboBox::backgroundColourId, panelRaised);
    setColour (juce::ComboBox::outlineColourId, outline);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, textDim);
    setColour (juce::PopupMenu::backgroundColourId, panelRaised);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.25f));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedTextColourId, text);
    setColour (juce::TextButton::buttonColourId, panelRaised);
    setColour (juce::TextButton::buttonOnColourId, accent.withAlpha (0.85f));
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::TextEditor::backgroundColourId, panelRaised);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, outline);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::ListBox::backgroundColourId, panel);
    setColour (juce::ListBox::textColourId, text);
    setColour (juce::ScrollBar::thumbColourId, outline.brighter (0.3f));
    setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, accent.withAlpha (0.25f));
    setColour (juce::DirectoryContentsDisplayComponent::textColourId, text);
    setColour (juce::FileBrowserComponent::currentPathBoxBackgroundColourId, panelRaised);
    setColour (juce::FileBrowserComponent::currentPathBoxTextColourId, text);
    setColour (juce::FileBrowserComponent::filenameBoxBackgroundColourId, panelRaised);
    setColour (juce::FileBrowserComponent::filenameBoxTextColourId, text);
    setColour (juce::TooltipWindow::backgroundColourId, panelRaised);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, outline);
    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
}

juce::Font ThrumaLookAndFeel::getLabelFont (juce::Label& l)
{
    return juce::Font (juce::FontOptions (juce::jmin (15.0f, l.getHeight() * 0.62f)));
}
juce::Font ThrumaLookAndFeel::getComboBoxFont (juce::ComboBox& c)
{
    return juce::Font (juce::FontOptions (juce::jmin (14.0f, c.getHeight() * 0.55f)));
}
juce::Font ThrumaLookAndFeel::getTextButtonFont (juce::TextButton&, int h)
{
    return juce::Font (juce::FontOptions (juce::jmin (14.0f, h * 0.5f), juce::Font::bold));
}
juce::Font ThrumaLookAndFeel::getPopupMenuFont()
{
    return juce::Font (juce::FontOptions (14.0f));
}

void ThrumaLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto c = bounds.getCentre();
    const float angle = a0 + pos * (a1 - a0);
    const bool hot = s.isMouseOverOrDragging();

    // body
    g.setColour (colours::panelRaised);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c).reduced (radius * 0.22f));
    g.setColour (colours::outline);
    g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c).reduced (radius * 0.22f), 1.0f);

    // track and value arc
    const float arcR = radius - 2.0f, thick = juce::jmax (2.0f, radius * 0.1f);
    juce::Path track;
    track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, a0, a1, true);
    g.setColour (colours::outline);
    g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    juce::Path value;
    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;   // pan: the arc grows from the centre
    const float from = bipolar ? a0 + (float) ((0.0 - s.getMinimum()) / (s.getMaximum() - s.getMinimum())) * (a1 - a0) : a0;
    value.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
    g.setColour (colours::accent.withAlpha (hot ? 1.0f : 0.9f));
    g.strokePath (value, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // pointer
    const float inner = radius * 0.25f, outer = radius * 0.7f;
    const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
    g.setColour (colours::text);
    g.drawLine ({ c + dir * inner, c + dir * outer }, juce::jmax (2.0f, radius * 0.09f));
}

void ThrumaLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float sliderPos, float, float,
                                          juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearBar)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, 0, 0, style, s);
        return;
    }
    const auto r = juce::Rectangle<float> ((float) x, (float) y + h * 0.5f - 3.0f, (float) w, 6.0f);
    g.setColour (colours::panelRaised.brighter (0.05f));
    g.fillRoundedRectangle (r, 3.0f);

    // bipolar sliders (e.g. dB around 0) fill from the zero point
    const double lo = s.getMinimum(), hi = s.getMaximum();
    const bool bipolar = lo < 0.0 && hi > 0.0;
    const float zeroX = bipolar ? (float) s.getPositionOfValue (0.0) : r.getX();
    auto fill = juce::Rectangle<float>::leftTopRightBottom (std::min (zeroX, sliderPos), r.getY(), std::max (zeroX, sliderPos), r.getBottom());
    g.setColour (s.isEnabled() ? colours::accent : colours::textDim);
    g.fillRoundedRectangle (fill, 3.0f);

    const float knobR = 7.0f;
    g.setColour (colours::text);
    g.fillEllipse (sliderPos - knobR, r.getCentreY() - knobR, knobR * 2.0f, knobR * 2.0f);
    g.setColour (colours::background);
    g.drawEllipse (sliderPos - knobR, r.getCentreY() - knobR, knobR * 2.0f, knobR * 2.0f, 1.5f);
}

void ThrumaLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool hi, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId) : bg;
    if (down) c = c.brighter (0.15f);
    else if (hi) c = c.brighter (0.07f);
    if (! b.isEnabled()) c = c.withMultipliedAlpha (0.5f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
}

void ThrumaLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool hi, bool)
{
    // pill switch + label
    auto r = b.getLocalBounds().toFloat();
    const float sw = 30.0f, sh = 16.0f;
    auto pill = juce::Rectangle<float> (r.getX() + 2.0f, r.getCentreY() - sh * 0.5f, sw, sh);
    const bool on = b.getToggleState();
    g.setColour (on ? colours::accent : colours::panelRaised.brighter (hi ? 0.15f : 0.08f));
    g.fillRoundedRectangle (pill, sh * 0.5f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (pill, sh * 0.5f, 1.0f);
    const float kx = on ? pill.getRight() - sh + 2.0f : pill.getX() + 2.0f;
    g.setColour (on ? colours::background : colours::textDim);
    g.fillEllipse (kx, pill.getY() + 2.0f, sh - 4.0f, sh - 4.0f);

    g.setColour (b.isEnabled() ? colours::text : colours::textDim);
    g.setFont (juce::FontOptions (14.0f));
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft ((int) sw + 10), juce::Justification::centredLeft, 1);
}

void ThrumaLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& c)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (c.findColour (juce::ComboBox::backgroundColourId).brighter (c.isMouseOver() ? 0.06f : 0.0f));
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (c.hasKeyboardFocus (true) ? colours::accent.withAlpha (0.7f) : colours::outline);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
    juce::Path arrow;
    const float ax = (float) w - 16.0f, ay = h * 0.5f;
    arrow.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
    g.setColour (colours::textDim);
    g.fillPath (arrow);
}

void ThrumaLookAndFeel::positionComboBoxText (juce::ComboBox& c, juce::Label& l)
{
    l.setBounds (8, 1, c.getWidth() - 30, c.getHeight() - 2);
    l.setFont (getComboBoxFont (c));
}

void ThrumaLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (colours::panelRaised);
    g.setColour (colours::outline);
    g.drawRect (0, 0, w, h);
}

void ThrumaLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                       int thumbStart, int thumbSize, bool over, bool down)
{
    g.setColour (colours::panel);
    g.fillRect (x, y, w, h);
    auto t = vertical ? juce::Rectangle<int> (x + 2, thumbStart, w - 4, thumbSize) : juce::Rectangle<int> (thumbStart, y + 2, thumbSize, h - 4);
    g.setColour (colours::outline.brighter (down ? 0.5f : over ? 0.35f : 0.2f));
    g.fillRoundedRectangle (t.toFloat(), 3.0f);
}

void ThrumaLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (colours::panelRaised);
    g.fillRoundedRectangle (0, 0, (float) w, (float) h, 4.0f);
}

void ThrumaLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
{
    g.setColour (e.hasKeyboardFocus (true) ? colours::accent : colours::outline);
    g.drawRoundedRectangle (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f, 4.0f, 1.0f);
}

} // namespace thruma::ui
