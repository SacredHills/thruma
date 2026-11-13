#include "MixerPage.h"

namespace thruma::ui
{

static constexpr int kStripW = 78, kGap = 6;

MixerStrip::MixerStrip (ThrumaProcessor& p, int s) : proc (p), slot (s)
{
    name.setJustificationType (juce::Justification::centred);
    name.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    name.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (name);

    pan.setRange (-100.0, 100.0, 1.0);
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
    pan.textFromValueFunction = [] (double v) { return v == 0.0 ? juce::String ("C") : (v < 0 ? "L" : "R") + juce::String (std::abs ((int) v)); };
    pan.setPopupDisplayEnabled (true, true, nullptr);
    pan.onValueChange = [this] { push(); repaint(); };
    addAndMakeVisible (pan);

    fader.setRange (-60.0, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.textFromValueFunction = [] (double v) { return v <= -59.9 ? juce::String ("-inf") : juce::String (v, 1) + " dB"; };
    fader.setPopupDisplayEnabled (true, true, nullptr);
    fader.onValueChange = [this] { push(); repaint(); };
    addAndMakeVisible (fader);

    for (auto* b : { &mute, &solo })
    {
        b->setClickingTogglesState (true);
        b->onClick = [this] { push(); };
        addAndMakeVisible (b);
    }
    mute.setColour (juce::TextButton::buttonOnColourId, colours::danger);
    solo.setColour (juce::TextButton::buttonOnColourId, colours::trigger);
    mute.setTooltip ("Mute");
    solo.setTooltip ("Solo");
    refresh();
}

void MixerStrip::refresh()
{
    auto& km = proc.getKitManager();
    const auto m = km.getSlot (slot);
    name.setText (km.getSlotName (slot), juce::dontSendNotification);
    name.setTooltip (km.getSlotName (slot));
    pan.setValue (m.pan * 100.0f, juce::dontSendNotification);
    fader.setValue (m.volumeDb <= -59.9f ? -60.0 : m.volumeDb, juce::dontSendNotification);
    mute.setToggleState (m.mute, juce::dontSendNotification);
    solo.setToggleState (m.solo, juce::dontSendNotification);
    hasSamples = ! m.isEmpty();
    name.setColour (juce::Label::textColourId, hasSamples ? colours::text : colours::textDim);
    repaint();
}

void MixerStrip::push()
{
    const float db = fader.getValue() <= -59.9 ? -100.0f : (float) fader.getValue();
    proc.getKitManager().setMix (slot, db, (float) pan.getValue() / 100.0f, mute.getToggleState(), solo.getToggleState());
    proc.syncMixer();
}

void MixerStrip::tick (float peak, float hit)
{
    const float before = meter, beforeHit = hitGlow;
    meter = juce::jmax (peak, meter * 0.86f);
    if (meter < 0.0005f) meter = 0.0f;
    hitGlow = juce::jmax (hit, hitGlow - 0.08f);
    if (meter != before || hitGlow != beforeHit) repaint();
}

void MixerStrip::resized()
{
    auto r = getLocalBounds().reduced (5, 6);
    name.setBounds (r.removeFromTop (30));
    r.removeFromTop (2);
    pan.setBounds (r.removeFromTop (44).withSizeKeepingCentre (44, 44));
    r.removeFromTop (16);   // pan readout
    auto ms = r.removeFromTop (24);
    mute.setBounds (ms.removeFromLeft (ms.getWidth() / 2 - 2));
    ms.removeFromLeft (4);
    solo.setBounds (ms);
    r.removeFromTop (8);
    r.removeFromBottom (20);   // dB readout
    fader.setBounds (r.removeFromLeft (r.getWidth() - 14));
}

void MixerStrip::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (colours::panelRaised.interpolatedWith (colours::accent, 0.25f * hitGlow));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (b, 6.0f, 1.0f);

    g.setColour (colours::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (pan.getTextFromValue (pan.getValue()), pan.getBounds().translated (0, 44).withHeight (16), juce::Justification::centred);
    g.drawText (fader.getTextFromValue (fader.getValue()), 0, getHeight() - 24, getWidth(), 18, juce::Justification::centred);

    // meter next to the fader
    const auto fb = fader.getBounds();
    auto mr = juce::Rectangle<float> ((float) fb.getRight() + 2.0f, (float) fb.getY() + 6.0f, 6.0f, (float) fb.getHeight() - 12.0f);
    g.setColour (colours::background);
    g.fillRoundedRectangle (mr, 2.0f);
    if (meter > 0.0f)
    {
        const float db = juce::Decibels::gainToDecibels (meter, -60.0f);
        const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
        auto lit = mr.removeFromBottom (mr.getHeight() * frac);
        g.setColour (db > -1.0f ? colours::danger : db > -9.0f ? colours::trigger : colours::accent);
        g.fillRoundedRectangle (lit, 2.0f);
    }
}

//==============================================================================
MixerPage::MixerPage (ThrumaProcessor& p) : proc (p)
{
    viewport.setViewedComponent (&holder, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);
    resetBtn.setTooltip ("Every drum and pad back to 0 dB, centre, no mute / solo.");
    resetBtn.onClick = [this]
    {
        for (int i = 0; i < kNumKitSlots; ++i) proc.getKitManager().setMix (i, 0.0f, 0.0f, false, false);
        proc.syncMixer();
        for (auto* s : strips) s->refresh();
    };
    addAndMakeVisible (resetBtn);
    for (int i = 0; i < kNumKitSlots; ++i) seen[(size_t) i] = proc.getHitMonitor().slots[(size_t) i].count.load();
    rebuild();
    startTimerHz (30);
}

void MixerPage::rebuild()
{
    shownPads = proc.getKitManager().getNumUsedPads();
    strips.clear();
    for (int i = 0; i < kNumInstruments + shownPads; ++i)
        holder.addAndMakeVisible (strips.add (new MixerStrip (proc, i)));
    resized();
}

void MixerPage::refresh()
{
    if (proc.getKitManager().getNumUsedPads() != shownPads) { rebuild(); return; }
    for (auto* s : strips) s->refresh();
}

void MixerPage::resized()
{
    auto r = getLocalBounds().reduced (12, 8);
    auto top = r.removeFromTop (30);
    resetBtn.setBounds (top.removeFromRight (120));
    r.removeFromTop (6);
    viewport.setBounds (r);
    const int h = juce::jmax (300, r.getHeight() - (strips.size() * (kStripW + kGap) > r.getWidth() ? viewport.getScrollBarThickness() : 0));
    int x = 0;
    for (auto* s : strips)
    {
        if (s->getSlot() == kNumInstruments) x += 10;   // a little gap before the pads
        s->setBounds (x, 0, kStripW, h);
        x += kStripW + kGap;
    }
    holder.setSize (juce::jmax (x, r.getWidth()), h);
}

void MixerPage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    g.setColour (colours::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Volume, pan, mute and solo per drum and pad (saved with the kit). Double-click a fader or knob to reset it.",
                getLocalBounds().reduced (14, 8).removeFromTop (30), juce::Justification::centredLeft);
}

void MixerPage::timerCallback()
{
    if (! isVisible()) return;
    if (proc.getKitManager().getNumUsedPads() != shownPads) rebuild();
    auto& mx = proc.getMixer();
    const auto& hm = proc.getHitMonitor();
    for (auto* s : strips)
    {
        const auto i = (size_t) s->getSlot();
        const float peak = mx.slots[i].meter.exchange (0.0f, std::memory_order_relaxed);
        float hit = 0.0f;
        if (const auto c = hm.slots[i].count.load (std::memory_order_acquire); c != seen[i]) { seen[i] = c; hit = 1.0f; }
        s->tick (peak, hit);
    }
}

} // namespace thruma::ui
