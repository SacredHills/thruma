#include "PluginEditor.h"

namespace thruma
{

ThrumaEditor::ThrumaEditor (ThrumaProcessor& p)
    : AudioProcessorEditor (p), proc (p), replacePage (p), debleedPage (p), samplesPage (p)
{
    setLookAndFeel (&lnf);
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

    const char* names[] = { "REPLACE", "DEBLEED", "SAMPLES" };
    for (int i = 0; i < 3; ++i)
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
    showTab (juce::jlimit (0, 2, proc.getUiTab()));
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
    for (int i = 0; i < 3; ++i)
        tabs[(size_t) i].setToggleState (i == index, juce::dontSendNotification);
    replacePage.setVisible (index == 0);
    debleedPage.setVisible (index == 1);
    samplesPage.setVisible (index == 2);
    if (index == 2) samplesPage.refreshDetail();
}

void ThrumaEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::colours::background);
    auto header = getLocalBounds().removeFromTop (52);
    g.setColour (ui::colours::panel);
    g.fillRect (header);
    g.setColour (ui::colours::outline);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());

    g.setColour (ui::colours::text);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("THRUMA", 18, 0, 140, header.getHeight(), juce::Justification::centredLeft);
    g.setColour (ui::colours::accent);
    g.fillRect (18, header.getHeight() - 14, 26, 3);
}

void ThrumaEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (52);
    const int tabW = 120, tabH = 32;
    auto tabArea = header.withSizeKeepingCentre (tabW * 3 + 12, tabH);
    for (auto& t : tabs) { t.setBounds (tabArea.removeFromLeft (tabW)); tabArea.removeFromLeft (6); }

    auto right = header.reduced (14, 10);
    outMeter.setBounds (right.removeFromRight (8));
    right.removeFromRight (4);
    inMeter.setBounds (right.removeFromRight (8));
    right.removeFromRight (12);
    hitLed.setBounds (right.removeFromRight (22));
    right.removeFromRight (10);
    latencyLabel.setBounds (right.removeFromRight (150));

    for (juce::Component* p : std::initializer_list<juce::Component*> { &replacePage, &debleedPage, &samplesPage })
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
