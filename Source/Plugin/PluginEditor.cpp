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
        const int maxW = juce::jmax (80, tabs[3].getX() - 30);
        auto area = juce::Rectangle<int> (14, 5, maxW, header.getHeight() - 10);
        g.drawImage (logo, area.toFloat(), juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yMid | juce::RectanglePlacement::onlyReduceInSize);
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
}

void ThrumaEditor::timerCallback()
{
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
