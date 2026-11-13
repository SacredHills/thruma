#include "DebleedPage.h"

namespace thruma::ui
{

void ReductionDisplay::update()
{
    for (int b = 0; b < DebleedProcessor::kMeterBands; ++b)
    {
        const float target = proc.isDebleedActive() ? proc.getDebleedMeterDb (b) : 0.0f;
        shown[(size_t) b] = target < shown[(size_t) b] ? target : shown[(size_t) b] + (target - shown[(size_t) b]) * 0.3f;
    }
    repaint();
}

void ReductionDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

    auto plot = r.reduced (44.0f, 26.0f).withTrimmedBottom (4.0f);
    const float maxDb = 36.0f;
    g.setFont (juce::FontOptions (11.0f));
    for (float db = 0.0f; db <= maxDb; db += 12.0f)
    {
        const float y = plot.getY() + plot.getHeight() * db / maxDb;
        g.setColour (colours::outline);
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
        g.setColour (colours::textDim);
        g.drawText ("-" + juce::String ((int) db) + " dB", (int) r.getX() + 4, (int) y - 7, 38, 14, juce::Justification::centredRight);
    }

    const int n = DebleedProcessor::kMeterBands;
    const float bw = plot.getWidth() / n;
    for (int b = 0; b < n; ++b)
    {
        const float red = juce::jlimit (0.0f, maxDb, -shown[(size_t) b]);
        auto bar = juce::Rectangle<float> (plot.getX() + b * bw + 1.5f, plot.getY(), bw - 3.0f, plot.getHeight() * red / maxDb);
        g.setColour (colours::accent.withAlpha (0.25f + 0.6f * red / maxDb));
        g.fillRoundedRectangle (bar, 2.0f);
    }
    g.setColour (colours::textDim);
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        // bands are log spaced between 30 Hz and min(20 kHz, Nyquist)
        const float lo = 30.0f, hi = (float) juce::jmin (20000.0, proc.getCurrentSampleRate() * 0.5);
        const float t = std::log (hz / lo) / std::log (hi / lo);
        if (t < 0.0f || t > 1.0f) continue;
        const float x = plot.getX() + t * plot.getWidth();
        g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000)) + "k" : juce::String ((int) hz), (int) x - 20, (int) plot.getBottom() + 4, 40, 14,
                    juce::Justification::centred);
    }
    g.setColour (colours::textDim);
    g.drawText (proc.isDebleedActive() ? "GAIN REDUCTION" : "DEBLEED OFF", r.reduced (12, 6).removeFromTop (14).toNearestInt(), juce::Justification::centredLeft);
}

//==============================================================================
DebleedPage::DebleedPage (ThrumaProcessor& p)
    : proc (p),
      dbOn (p.apvts, ids::dbOn, "Debleed", "Reduce bleed before the drum is detected and replaced (adds about 21 ms latency, compensated by the DAW)."),
      display (p),
      instrument (p.apvts, ids::instrument, "Instrument", "The drum on this track (shared with REPLACE)."),
      amount (p.apvts, ids::dbAmount, "Amount", "How strongly bleed is reduced."),
      sensitivity (p.apvts, ids::dbSens, "Sensitivity", "How readily quiet hits of the drum (ghost notes) are recognised and kept."),
      attack (p.apvts, ids::dbAttack, "Attack Preservation", "Keeps the start of every hit untouched."),
      release (p.apvts, ids::dbRelease, "Release", "How smoothly the reduction lets go after each hit.")
{
    for (juce::Component* c : std::initializer_list<juce::Component*> { &dbOn, &status, &display, &panel, &instrument, &refLabel, &reference,
                                                                        &amount, &sensitivity, &attack, &release })
        addAndMakeVisible (c);
    panel.toBack();
    status.setColour (juce::Label::textColourId, colours::textDim);
    refLabel.setText ("REFERENCE", juce::dontSendNotification);
    refLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    refLabel.setColour (juce::Label::textColourId, colours::textDim);
    reference.setTooltip ("Optional: another mic that carries the bleed (e.g. overheads or the kick). Pick the sidechain input "
                          "or any other track that has Thruma on it.");
    reference.onChange = [this] { referenceChosen(); };
    refreshReferences();
    startTimerHz (25);
}

void DebleedPage::refreshReferences()
{
    juce::StringArray items { "None", "Sidechain L", "Sidechain R", "Sidechain L+R" };
    for (auto& n : proc.getOtherTrackNames()) items.add ("Track: " + n);
    if (proc.getLinkedSourceName().isNotEmpty()) items.addIfNotAlreadyThere ("Track: " + proc.getLinkedSourceName());

    const auto ref = (ReferenceSource) (int) proc.apvts.getRawParameterValue (ids::dbReference)->load();
    const juce::String current = ref == ReferenceSource::Linked ? "Track: " + proc.getLinkedSourceName() : items[juce::jlimit (0, 3, (int) ref)];
    if (items != lastItems)
    {
        lastItems = items;
        reference.clear (juce::dontSendNotification);
        reference.addItemList (items, 1);
    }
    if (reference.getText() != current) reference.setText (current, juce::dontSendNotification);
}

void DebleedPage::referenceChosen()
{
    const auto text = reference.getText();
    int choice = 0;
    if (text.startsWith ("Track: ")) { choice = (int) ReferenceSource::Linked; proc.setLinkedSourceName (text.fromFirstOccurrenceOf ("Track: ", false, false)); }
    else choice = juce::jmax (0, juce::StringArray { "None", "Sidechain L", "Sidechain R", "Sidechain L+R" }.indexOf (text));
    if (auto* prm = proc.apvts.getParameter (ids::dbReference))
        prm->setValueNotifyingHost (prm->convertTo0to1 ((float) choice));
}

void DebleedPage::timerCallback()
{
    display.update();
    if (++tick % 12 == 0) refreshReferences();
    const auto ref = (ReferenceSource) (int) proc.apvts.getRawParameterValue (ids::dbReference)->load();
    juce::String s;
    if (! proc.isDebleedActive()) s = "Off. Turn on to clean up the drum before it is detected and replaced.";
    else if (ref == ReferenceSource::None) s = "On. Bleed is recognised from the drum's own hits and decay. Learns while the track plays.";
    else if (ref == ReferenceSource::Linked) s = proc.isLinkSynced() ? "On. Using the reference track." : "On. Waiting for the reference track to play...";
    else s = proc.isSidechainConnected() ? "On. Using the sidechain reference." : "On. Route a reference mic to the sidechain input.";
    status.setText (s, juce::dontSendNotification);
}

void DebleedPage::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto bar = r.removeFromTop (30);
    dbOn.setBounds (bar.removeFromLeft (130));
    status.setBounds (bar);
    r.removeFromTop (8);

    auto controls = r.removeFromBottom (170);
    r.removeFromBottom (10);
    display.setBounds (r);

    panel.setBounds (controls);
    auto c = panel.getContentBounds();
    auto left = c.removeFromLeft (juce::jmin (260, c.getWidth() / 3));
    c.removeFromLeft (24);
    instrument.setBounds (left.removeFromTop (46));
    left.removeFromTop (10);
    refLabel.setBounds (left.removeFromTop (16));
    reference.setBounds (left.removeFromTop (28));
    layoutRows (c, { &amount, &sensitivity, &attack, &release }, 28, 6);
}

} // namespace thruma::ui
