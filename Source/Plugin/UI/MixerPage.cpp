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
    pan.onValueChange = [this] { push(); panValue.setText (pan.getTextFromValue (pan.getValue()), juce::dontSendNotification); };
    addAndMakeVisible (pan);

    // the values under the knob and the fader: click and type ("L30", "R 15", "-20", "C" / "-6", "+2.5")
    for (auto* l : { &panValue, &dbValue })
    {
        l->setEditable (true, true, false);
        l->setJustificationType (juce::Justification::centred);
        l->setFont (juce::FontOptions (11.0f));
        l->setColour (juce::Label::textColourId, colours::textDim);
        l->setColour (juce::Label::backgroundWhenEditingColourId, colours::background);
        l->setColour (juce::Label::textWhenEditingColourId, colours::text);
        l->setColour (juce::Label::outlineWhenEditingColourId, colours::accent);
        addAndMakeVisible (l);
    }
    panValue.setTooltip ("Click to type: L30, R15, -20, C");
    dbValue.setTooltip ("Click to type the volume in dB (-inf: type -60 or less)");
    panValue.onTextChange = [this]
    {
        auto t = panValue.getText().trim().toUpperCase();
        double v = 0.0;
        if (t.startsWithChar ('L')) v = -std::abs (t.substring (1).trim().getDoubleValue());
        else if (t.startsWithChar ('R')) v = std::abs (t.substring (1).trim().getDoubleValue());
        else if (t != "C" && t.isNotEmpty()) v = t.getDoubleValue();
        pan.setValue (juce::jlimit (-100.0, 100.0, v));
        panValue.setText (pan.getTextFromValue (pan.getValue()), juce::dontSendNotification);
    };
    dbValue.onTextChange = [this]
    {
        auto t = dbValue.getText().trim().toLowerCase().replace ("db", "").trim();
        fader.setValue (t.contains ("inf") ? -60.0 : juce::jlimit (-60.0, 12.0, t.getDoubleValue()));
        dbValue.setText (fader.getTextFromValue (fader.getValue()), juce::dontSendNotification);
    };

    output.setTooltip ("Where this drum plays: the main output, or one of Thruma's extra outputs (turn those on in your DAW to give each drum its own channel).");
    output.onChange = [this]
    {
        proc.getKitManager().setOutput (slot, juce::jmax (0, output.getSelectedId() - 1));
        proc.syncMixer();
    };
    addAndMakeVisible (output);

    fader.setRange (-60.0, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (-12.0);
    fader.setDoubleClickReturnValue (true, 0.0);
    fader.textFromValueFunction = [] (double v) { return v <= -59.9 ? juce::String ("-inf") : juce::String (v, 1) + " dB"; };
    fader.setPopupDisplayEnabled (true, true, nullptr);
    fader.onValueChange = [this] { push(); dbValue.setText (fader.getTextFromValue (fader.getValue()), juce::dontSendNotification); repaint(); };
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
    panValue.setText (pan.getTextFromValue (pan.getValue()), juce::dontSendNotification);
    dbValue.setText (fader.getTextFromValue (fader.getValue()), juce::dontSendNotification);
    refreshOutputs();
    name.setColour (juce::Label::textColourId, hasSamples ? colours::text : colours::textDim);
    repaint();
}

void MixerStrip::refreshOutputs()
{
    // "Main", then the extra outputs; ones the host has not turned on are marked
    output.clear (juce::dontSendNotification);
    output.addItem ("Main", 1);
    for (int k = 1; k < proc.getBusCount (false) && k <= AuxTargets::kMax; ++k)
    {
        const auto* bus = proc.getBus (false, k);
        output.addItem ("Out " + juce::String (k + 1) + (bus != nullptr && bus->isEnabled() ? "" : "  (off in DAW)"), k + 1);
    }
    output.setSelectedId (proc.getKitManager().getSlot (slot).output + 1, juce::dontSendNotification);
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
    panValue.setBounds (r.removeFromTop (16));
    auto ms = r.removeFromTop (24);
    mute.setBounds (ms.removeFromLeft (ms.getWidth() / 2 - 2));
    ms.removeFromLeft (4);
    solo.setBounds (ms);
    r.removeFromTop (6);
    output.setBounds (r.removeFromTop (22));
    r.removeFromTop (6);
    dbValue.setBounds (r.removeFromBottom (20).expanded (4, 0));
    fader.setBounds (r.removeFromLeft (r.getWidth() - 14));
}

void MixerStrip::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (colours::panelRaised.interpolatedWith (colours::accent, 0.25f * hitGlow));
    g.fillRoundedRectangle (b, 6.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (b, 6.0f, 1.0f);


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
    for (int row = 0; row < kNumInstruments + shownPads; ++row)
        holder.addAndMakeVisible (strips.add (new MixerStrip (proc, slotForRow (row))));
    resized();
}

void MixerPage::refresh()
{
    for (auto* s : strips) s->refreshOutputs();   // the host may have turned outputs on / off
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
