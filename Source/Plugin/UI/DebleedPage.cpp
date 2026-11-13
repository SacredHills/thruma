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
      dbOn (p.apvts, ids::dbOn, "Debleed", "Turn the debleed processor on (adds about 21 ms latency, compensated by the host)."),
      display (p),
      instrument (p.apvts, ids::instrument, "Instrument", "The drum this mic is for (shared with the Replace page)."),
      reference (p.apvts, ids::dbReference, "Reference", "Optional reference mic: the sidechain input or another linked Thruma instance."),
      amount (p.apvts, ids::dbAmount, "Amount", "How strongly bleed is reduced."),
      sensitivity (p.apvts, ids::dbSens, "Sensitivity", "How readily quiet hits (ghost notes) of the target drum are recognised and protected."),
      attack (p.apvts, ids::dbAttack, "Attack Preservation", "Length of the untouched window at each hit."),
      release (p.apvts, ids::dbRelease, "Release", "How quickly reduction returns after the drum - longer is smoother."),
      maxRed (p.apvts, ids::dbMaxRed, "Max Reduction", "Deepest attenuation at 100 % Amount (never a hard gate)."),
      smooth (p.apvts, ids::dbSmooth, "Smoothing", "Frequency smoothing of the reduction (avoids artifacts)."),
      lowProtect (p.apvts, ids::dbLowProtect, "Low Protect", "Never reduce below this frequency."),
      decay (p.apvts, ids::dbDecay, "Decay Scale", "Scales the learned drum decay that is always preserved."),
      adaptive (p.apvts, ids::dbAdaptive, "Adaptive learning", "Keep refining the bleed estimate while playing.")
{
    addAndMakeVisible (dbOn);
    addAndMakeVisible (analyzeBtn);
    addAndMakeVisible (advancedBtn);
    analyzeBtn.setTooltip ("Learn the drum's decay, its fingerprint and the bleed from the audio captured on the Replace page; "
                           "picks the best reference and a suitable Amount.");
    advancedBtn.setClickingTogglesState (true);
    advancedBtn.onClick = [this] { showAdvanced = advancedBtn.getToggleState(); resized(); };
    analyzeBtn.onClick = [this]
    {
        report.setText ("Analyzing...", juce::dontSendNotification);
        juce::Component::SafePointer<DebleedPage> safe (this);
        proc.analyseDebleed ([safe] (const juce::String& r) { if (safe != nullptr) safe->report.setText (r, juce::dontSendNotification); });
    };

    report.setColour (juce::Label::textColourId, colours::textDim);
    report.setText (proc.getDebleedReport().isNotEmpty() ? proc.getDebleedReport()
                                                         : "Tip: capture a section on the Replace page, then press ANALYZE for automatic settings.",
                    juce::dontSendNotification);
    addAndMakeVisible (report);
    addAndMakeVisible (display);

    addAndMakeVisible (mainPanel);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &instrument, &reference, &amount, &sensitivity, &attack, &release })
        addAndMakeVisible (c);

    linkedLabel.setText ("LINKED SOURCE", juce::dontSendNotification);
    linkedLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    linkedLabel.setColour (juce::Label::textColourId, colours::textDim);
    addAndMakeVisible (linkedLabel);
    linkedSource.setTextWhenNothingSelected ("(choose instance)");
    linkedSource.setTooltip ("Another Thruma instance that shares its track (set 'Share this track as' there).");
    linkedSource.onChange = [this] { proc.setLinkedSourceName (linkedSource.getText()); };
    addAndMakeVisible (linkedSource);

    shareLabel.setText ("SHARE THIS TRACK AS", juce::dontSendNotification);
    shareLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    shareLabel.setColour (juce::Label::textColourId, colours::textDim);
    addAndMakeVisible (shareLabel);
    shareName.setText (proc.getPublishName(), juce::dontSendNotification);
    shareName.setTextToShowWhenEmpty ("e.g. Kick In", colours::textDim);
    shareName.setTooltip ("Make this track available as a reference for other Thruma instances in this project.");
    shareName.onReturnKey = [this] { proc.setPublishName (shareName.getText()); };
    shareName.onFocusLost = [this] { proc.setPublishName (shareName.getText()); };
    addAndMakeVisible (shareName);

    linkStatus.setColour (juce::Label::textColourId, colours::textDim);
    linkStatus.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (linkStatus);

    addChildComponent (advPanel);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &maxRed, &smooth, &lowProtect, &decay, &adaptive })
        addChildComponent (c);

    mainPanel.toBack();
    advPanel.toBack();
    refreshLinkSources();
    startTimerHz (25);
}

void DebleedPage::refreshLinkSources()
{
    juce::StringArray names;
    for (auto& s : LinkHub::getInstance().listSources())
        if (juce::String (s.name) != proc.getPublishName()) names.add (s.name);
    if (proc.getLinkedSourceName().isNotEmpty()) names.addIfNotAlreadyThere (proc.getLinkedSourceName());
    if (names == lastSources) return;
    lastSources = names;
    linkedSource.clear (juce::dontSendNotification);
    linkedSource.addItemList (names, 1);
    linkedSource.setText (proc.getLinkedSourceName(), juce::dontSendNotification);
}

void DebleedPage::timerCallback()
{
    display.update();
    if (++tick % 12 == 0) refreshLinkSources();

    const auto ref = (ReferenceSource) (int) proc.apvts.getRawParameterValue (ids::dbReference)->load();
    const bool linked = ref == ReferenceSource::Linked;
    linkedSource.setEnabled (linked);
    juce::String s;
    if (ref == ReferenceSource::None) s = "Blind mode: bleed is identified from the drum's own hits and decay.";
    else if (linked) s = proc.getLinkedSourceName().isEmpty() ? "Choose a linked instance."
                       : proc.isLinkSynced() ? "Linked reference in sync." : "Linked reference waiting for playback...";
    else s = proc.isSidechainConnected() ? "Sidechain reference connected." : "Route a reference mic to the sidechain input.";
    linkStatus.setText (s, juce::dontSendNotification);
    analyzeBtn.setEnabled (! proc.isAnalysing());
}

void DebleedPage::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto bar = r.removeFromTop (30);
    dbOn.setBounds (bar.removeFromLeft (120));
    analyzeBtn.setBounds (bar.removeFromLeft (100));
    bar.removeFromLeft (14);
    advancedBtn.setBounds (bar.removeFromRight (100));
    report.setBounds (bar);
    r.removeFromTop (8);

    auto controls = r.removeFromBottom (196);
    r.removeFromBottom (10);
    if (showAdvanced)
    {
        auto adv = r.removeFromBottom (120);
        r.removeFromBottom (10);
        advPanel.setBounds (adv);
        auto c = advPanel.getContentBounds();
        auto col1 = c.removeFromLeft ((c.getWidth() - 20) / 2);
        c.removeFromLeft (20);
        layoutRows (col1, { &maxRed, &smooth }, 30, 8);
        layoutRows (c, { &lowProtect, &decay, &adaptive }, 26, 4);
    }
    for (juce::Component* comp : std::initializer_list<juce::Component*> { &advPanel, &maxRed, &smooth, &lowProtect, &decay, &adaptive })
        comp->setVisible (showAdvanced);
    display.setBounds (r);

    mainPanel.setBounds (controls);
    auto c = mainPanel.getContentBounds();
    auto left = c.removeFromLeft (juce::jmin (420, c.getWidth() / 2));
    c.removeFromLeft (24);
    const int w = (left.getWidth() - 12) / 2;
    auto row1 = left.removeFromTop (46);
    instrument.setBounds (row1.removeFromLeft (w));
    row1.removeFromLeft (12);
    reference.setBounds (row1);
    left.removeFromTop (10);
    auto row2 = left.removeFromTop (46);
    auto shareCol = row2.removeFromLeft (w);
    shareLabel.setBounds (shareCol.removeFromTop (16));
    shareName.setBounds (shareCol.removeFromTop (28));
    row2.removeFromLeft (12);
    linkedLabel.setBounds (row2.removeFromTop (16));
    linkedSource.setBounds (row2.removeFromTop (28));
    left.removeFromTop (8);
    linkStatus.setBounds (left.removeFromTop (24));

    layoutRows (c, { &amount, &sensitivity, &attack, &release }, 28, 8);
}

} // namespace thruma::ui
