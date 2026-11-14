#include "PluginEditor.h"
#include "BinaryData.h"

namespace thruma
{

ThrumaEditor::ThrumaEditor (ThrumaProcessor& p)
    : AudioProcessorEditor (p), proc (p), playerPage (p), mixerPage (p), replacePage (p), debleedPage (p), samplesPage (p)
{
    setLookAndFeel (&lnf);
    logo = juce::ImageCache::getFromMemory (BinaryData::logo_png, BinaryData::logo_pngSize);
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

    const char* names[] = { "DEBLEED", "REPLACE", "SAMPLES", "PLAYER", "MIXER" };
    for (int i = 0; i < kNumTabs; ++i)
    {
        auto& t = tabs[(size_t) i];
        t.setButtonText (names[i]);
        t.setClickingTogglesState (false);
        t.onClick = [this, i] { showTab (i); };
        addAndMakeVisible (t);
    }
    addChildComponent (replacePage);
    addChildComponent (debleedPage);
    addChildComponent (samplesPage);
    addChildComponent (playerPage);
    addChildComponent (mixerPage);
    playerPage.onEditSlot = [this] (int slot) { showTab (2); samplesPage.selectSlot (slot); };
    playerPage.onKitChanged = [this] { samplesPage.selectSlot (samplesPage.getSelectedInstrument()); mixerPage.refresh(); };
    addAndMakeVisible (inMeter);
    addAndMakeVisible (outMeter);
    addAndMakeVisible (hitLed);
    inMeter.setTooltip ("Input level");
    outMeter.setTooltip ("Output level");
    hitLed.setTooltip ("Flashes on every triggered hit");
    latencyLabel.setColour (juce::Label::textColourId, ui::colours::textDim);
    latencyLabel.setFont (juce::FontOptions (11.0f));
    latencyLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (latencyLabel);

    addChildComponent (loading);   // last: on top of everything

    setResizable (true, true);
    setResizeLimits (940, 620, 2000, 1400);
    setSize (1080, 700);
    showTab (juce::jlimit (0, kNumTabs - 1, proc.getUiTab()));
    startTimerHz (30);
}

ThrumaEditor::~ThrumaEditor()
{
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void ThrumaEditor::showTab (int index)
{
    currentTab = index;
    proc.setUiTab (index);
    for (int i = 0; i < kNumTabs; ++i)
        tabs[(size_t) i].setToggleState (i == index, juce::dontSendNotification);
    debleedPage.setVisible (index == 0);
    replacePage.setVisible (index == 1);
    samplesPage.setVisible (index == 2);
    playerPage.setVisible (index == 3);
    mixerPage.setVisible (index == 4);
    if (index == 4) mixerPage.refresh();
    if (index == 2) samplesPage.selectSlot (samplesPage.getSelectedInstrument());
}

void ThrumaEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::colours::background);
    auto header = getLocalBounds().removeFromTop (kHeaderH);
    g.setColour (juce::Colours::black);
    g.fillRect (header);
    g.setColour (ui::colours::outline);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());

    if (logo.isValid())
    {
        // keep it clear of the tabs
        // centred between the left edge and the first tab (PLAYER)
        auto area = juce::Rectangle<int> (0, 0, tabs[3].getX(), header.getHeight()).reduced (12, 5);
        g.drawImage (logo, area.toFloat(), juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
    }
    else
    {
        g.setColour (ui::colours::text);
        g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        g.drawText ("THRUMA", 18, 0, 140, header.getHeight(), juce::Justification::centredLeft);
    }
}

void ThrumaEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (kHeaderH);
    const int tabW = 108, tabH = 32;
    auto tabArea = header.withSizeKeepingCentre (tabW * kNumTabs + 6 * (kNumTabs - 1), tabH);
    for (int i : { 3, 0, 1, 2, 4 })   // shown: PLAYER, DEBLEED, REPLACE, SAMPLES, MIXER
    {
        tabs[(size_t) i].setBounds (tabArea.removeFromLeft (tabW));
        tabArea.removeFromLeft (6);
    }

    auto right = header.reduced (14, 10);
    outMeter.setBounds (right.removeFromRight (8));
    right.removeFromRight (4);
    inMeter.setBounds (right.removeFromRight (8));
    right.removeFromRight (12);
    hitLed.setBounds (right.removeFromRight (22));
    right.removeFromRight (10);
    latencyLabel.setBounds (right.removeFromRight (150));

    for (juce::Component* p : std::initializer_list<juce::Component*> { &debleedPage, &replacePage, &samplesPage, &playerPage, &mixerPage })
        p->setBounds (r);
    loading.setBounds (getLocalBounds());
}

void LoadingOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    auto& km = proc.getKitManager();
    const auto phase = km.getProgressPhase();
    const int done = km.getProgressDone(), total = km.getProgressTotal();
    const auto box = getLocalBounds().withSizeKeepingCentre (380, 96).toFloat();
    g.setColour (ui::colours::panelRaised);
    g.fillRoundedRectangle (box, 10.0f);
    g.setColour (ui::colours::outline);
    g.drawRoundedRectangle (box, 10.0f, 1.0f);

    juce::String text = phase == KitManager::LoadPhase::Kit ? "Preparing kit" : "Loading samples";
    if (total > 0) text << "  " << juce::jmin (done, total) << " / " << total;
    g.setColour (ui::colours::text);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (text, box.reduced (20.0f, 14.0f).removeFromTop (24.0f), juce::Justification::centredLeft);

    auto bar = box.reduced (20.0f, 0.0f).withTrimmedTop (54.0f).withHeight (10.0f);
    g.setColour (ui::colours::background);
    g.fillRoundedRectangle (bar, 5.0f);
    g.setColour (ui::colours::accent);
    if (total > 0)
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, (float) done / (float) total)), 5.0f);
    else
    {
        // amount unknown yet (scanning a folder): a moving block
        spin = std::fmod (spin + 0.03f, 1.0f);
        const float w = bar.getWidth() * 0.25f;
        g.fillRoundedRectangle (bar.withWidth (w).withX (bar.getX() + (bar.getWidth() - w) * (0.5f + 0.5f * std::sin (spin * juce::MathConstants<float>::twoPi))), 5.0f);
    }
}

void ThrumaEditor::timerCallback()
{
    // the loading bar appears only for loads that take a moment (quick edits don't flash it)
    const auto now = juce::Time::getMillisecondCounter();
    if (! proc.isLoadingKit()) loadingSince = 0;
    else if (loadingSince == 0) loadingSince = now;
    const bool showLoading = loadingSince != 0 && now - loadingSince > 250;
    if (showLoading != loading.isVisible()) loading.setVisible (showLoading);
    if (loading.isVisible()) loading.repaint();
    inMeter.setLevel (proc.inputPeak.load());
    outMeter.setLevel (proc.outputPeak.load());
    const int hits = proc.hitCounter.load();
    if (hits != lastHits) { hitLed.trigger (proc.lastHitVelocity.load()); lastHits = hits; }
    else hitLed.decay();

    const double ms = proc.getLatencySamples() * 1000.0 / juce::jmax (1.0, proc.getCurrentSampleRate());
    latencyLabel.setText ("latency " + juce::String (ms, 1) + " ms", juce::dontSendNotification);
}

} // namespace thruma

juce::AudioProcessorEditor* thruma::ThrumaProcessor::createEditor()
{
    return new ThrumaEditor (*this);
}
