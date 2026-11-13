#include "ReplacePage.h"

namespace thruma::ui
{

ReplacePage::ReplacePage (ThrumaProcessor& p)
    : proc (p), wave (p),
      repOn (p.apvts, ids::repOn, "Replace", "Turn drum replacement on or off (MIDI-triggered samples still play)."),
      instrument (p.apvts, ids::instrument, "Instrument", "Which drum this track contains. Selects a tuned detection profile."),
      sample (p.apvts, ids::repSample, "Sample", "Which kit slot plays for detected hits."),
      source (p.apvts, ids::repSource, "Triggers", "Live detection, the edited trigger list below, or MIDI input only."),
      mode (p.apvts, ids::repMode, "Mode", "Replace crossfades original and sample with Blend; Layer adds the sample on top."),
      sensitivity (p.apvts, ids::repSens, "Sensitivity", "Higher catches quieter hits; lower rejects more bleed."),
      velResp (p.apvts, ids::repVelResp, "Velocity Response", "0 % = every hit at full level, 100 % = follow the played dynamics."),
      blend (p.apvts, ids::repBlend, "Blend", "Replace: original/sample balance. Layer: sample level."),
      output (p.apvts, ids::repOutput, "Output", "Output level."),
      ghost (p.apvts, ids::repGhost, "Ghost Notes", "Extends detection to quiet ghost notes (most useful on snare and hats)."),
      retrig (p.apvts, ids::repRetrig, "Retrigger", "Minimum time between hits. 0 = instrument default."),
      dynRange (p.apvts, ids::repDynRange, "Dynamic Range", "Level range mapped onto velocity 1-127."),
      gain (p.apvts, ids::repGain, "Sample Gain", "Gain of the replacement sample."),
      offset (p.apvts, ids::repOffset, "Sample Start", "Skip into the sample (trims its attack)."),
      align (p.apvts, ids::repAlign, "Align", "Shift the replacement against the original (phase alignment)."),
      doubleHits (p.apvts, ids::repDouble, "Double hits", "Detect flams, drags and fast doubles while the previous hit still rings."),
      focus (p.apvts, ids::repFocus, "Frequency focus", "Reject hits whose spectrum does not match the instrument (false-trigger prevention)."),
      polarity (p.apvts, ids::repPolarity, "Invert polarity", "Flip the replacement's polarity."),
      autoAlign (p.apvts, ids::repAutoAlign, "Auto align", "Line up each sample's transient with the detected hit."),
      midiOut (p.apvts, ids::midiOut, "MIDI out", "Send detected hits as MIDI notes."),
      latency (p.apvts, ids::repLatency, "Lookahead", "Accurate: 10 ms lookahead, best timing and velocity. Low latency: 3 ms.")
{
    for (auto* b : { &captureBtn, &analyzeBtn, &clearBtn, &undoBtn, &redoBtn, &fitBtn, &zoomInBtn, &zoomOutBtn, &exportBtn, &auditionBtn, &advancedBtn })
        addAndMakeVisible (b);
    captureBtn.setClickingTogglesState (true);
    captureBtn.setColour (juce::TextButton::buttonOnColourId, colours::record);
    captureBtn.setTooltip ("Record the incoming audio while the host plays, for the editor and Analyze. The original track is never changed.");
    analyzeBtn.setTooltip ("Detect all hits in the captured audio (two passes: take-level dynamics and spectral fingerprint).");
    exportBtn.setTooltip ("Export the trigger list as a Standard MIDI File (host tempo).");
    auditionBtn.setTooltip ("Play the replacement sample.");
    advancedBtn.setClickingTogglesState (true);

    captureBtn.onClick = [this] { proc.setCaptureArmed (captureBtn.getToggleState()); updateStatus(); };
    analyzeBtn.onClick = [this]
    {
        status.setText ("Analyzing...", juce::dontSendNotification);
        juce::Component::SafePointer<ReplacePage> safe (this);
        proc.analyseCapture ([safe] (int n)
        {
            if (safe == nullptr) return;
            if (n < 0) safe->status.setText ("Nothing captured yet - arm CAPTURE and play.", juce::dontSendNotification);
            else
            {
                if (auto* src = safe->proc.apvts.getParameter (ids::repSource))
                    src->setValueNotifyingHost (src->convertTo0to1 ((float) TriggerSource::Edited));
                safe->wave.repaint();
                safe->updateStatus();
            }
        });
    };
    clearBtn.onClick = [this]
    {
        proc.commitTriggerEdit ({}, "Clear triggers");
        updateStatus();
    };
    undoBtn.onClick = [this] { proc.getUndoManager().undo(); updateStatus(); };
    redoBtn.onClick = [this] { proc.getUndoManager().redo(); updateStatus(); };
    fitBtn.onClick = [this] { wave.zoomToFit(); };
    zoomInBtn.onClick = [this] { wave.zoomBy (1.5, wave.getWidth() * 0.5f); };
    zoomOutBtn.onClick = [this] { wave.zoomBy (1.0 / 1.5, wave.getWidth() * 0.5f); };
    exportBtn.onClick = [this] { exportMidi(); };
    auditionBtn.onClick = [this] { proc.auditionReplacement (0.9f); };
    advancedBtn.onClick = [this] { showAdvanced = advancedBtn.getToggleState(); resized(); };
    wave.onEdited = [this] { updateStatus(); };

    status.setColour (juce::Label::textColourId, colours::textDim);
    status.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (status);
    addAndMakeVisible (wave);

    addAndMakeVisible (mainPanel);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &repOn, &instrument, &sample, &source, &mode, &sensitivity, &velResp, &blend, &output })
        addAndMakeVisible (c);
    sampleInfo.setColour (juce::Label::textColourId, colours::textDim);
    sampleInfo.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (sampleInfo);

    addChildComponent (advPanel);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &ghost, &retrig, &dynRange, &gain, &offset, &align, &doubleHits, &focus, &polarity, &autoAlign, &midiOut, &latency })
        addChildComponent (c);

    mainPanel.toBack();   // panels are backgrounds: keep every control above them
    advPanel.toBack();
    captureBtn.setToggleState (proc.isCaptureArmed(), juce::dontSendNotification);
    startTimerHz (8);
    updateStatus();
}

void ReplacePage::paint (juce::Graphics&) {}

void ReplacePage::resized()
{
    auto r = getLocalBounds().reduced (12);

    auto bar = r.removeFromTop (30);
    captureBtn.setBounds (bar.removeFromLeft (96));
    bar.removeFromLeft (6);
    analyzeBtn.setBounds (bar.removeFromLeft (90));
    bar.removeFromLeft (6);
    clearBtn.setBounds (bar.removeFromLeft (60));
    bar.removeFromLeft (14);
    undoBtn.setBounds (bar.removeFromLeft (56));
    bar.removeFromLeft (4);
    redoBtn.setBounds (bar.removeFromLeft (56));
    bar.removeFromLeft (14);
    zoomOutBtn.setBounds (bar.removeFromLeft (30));
    bar.removeFromLeft (4);
    zoomInBtn.setBounds (bar.removeFromLeft (30));
    bar.removeFromLeft (4);
    fitBtn.setBounds (bar.removeFromLeft (44));
    exportBtn.setBounds (bar.removeFromRight (100));
    bar.removeFromRight (8);
    status.setBounds (bar);
    r.removeFromTop (8);

    auto controls = r.removeFromBottom (196);
    r.removeFromBottom (10);

    if (showAdvanced)
    {
        auto adv = r.removeFromBottom (156);
        r.removeFromBottom (10);
        advPanel.setBounds (adv);
        auto c = advPanel.getContentBounds();
        const int colW = (c.getWidth() - 40) / 3;
        auto col1 = c.removeFromLeft (colW); c.removeFromLeft (20);
        auto col2 = c.removeFromLeft (colW); c.removeFromLeft (20);
        auto col3 = c;
        layoutRows (col1, { &ghost, &retrig, &dynRange }, 30, 8);
        layoutRows (col2, { &gain, &offset, &align }, 30, 8);
        auto t = col3.removeFromLeft (col3.getWidth() / 2);
        layoutRows (t, { &doubleHits, &focus, &polarity }, 30, 8);
        layoutRows (col3.removeFromTop (68), { &autoAlign, &midiOut }, 30, 8);
        latency.setBounds (col3.removeFromTop (46));
    }
    for (juce::Component* c : std::initializer_list<juce::Component*> { &advPanel, &ghost, &retrig, &dynRange, &gain, &offset, &align, &doubleHits, &focus, &polarity, &autoAlign, &midiOut, &latency })
        c->setVisible (showAdvanced);

    wave.setBounds (r);

    mainPanel.setBounds (controls);
    auto c = mainPanel.getContentBounds();
    auto left = c.removeFromLeft (juce::jmin (420, c.getWidth() / 2));
    c.removeFromLeft (24);
    {
        auto row1 = left.removeFromTop (46);
        const int w = (row1.getWidth() - 12) / 2;
        instrument.setBounds (row1.removeFromLeft (w));
        row1.removeFromLeft (12);
        sample.setBounds (row1);
        left.removeFromTop (10);
        auto row2 = left.removeFromTop (46);
        source.setBounds (row2.removeFromLeft (w));
        row2.removeFromLeft (12);
        mode.setBounds (row2);
        left.removeFromTop (10);
        auto row3 = left.removeFromTop (28);
        repOn.setBounds (row3.removeFromLeft (110));
        sampleInfo.setBounds (row3);
    }
    {
        auto right = c;
        auto btns = right.removeFromBottom (28);
        advancedBtn.setBounds (btns.removeFromRight (100));
        btns.removeFromRight (8);
        auditionBtn.setBounds (btns.removeFromRight (90));
        right.removeFromBottom (6);
        layoutRows (right, { &sensitivity, &velResp, &blend, &output }, 28, 4);
    }
}

void ReplacePage::timerCallback() { updateStatus(); }

void ReplacePage::updateStatus()
{
    undoBtn.setEnabled (proc.getUndoManager().canUndo());
    redoBtn.setEnabled (proc.getUndoManager().canRedo());
    analyzeBtn.setEnabled (! proc.isAnalysing());
    if (captureBtn.getToggleState() != proc.isCaptureArmed())
        captureBtn.setToggleState (proc.isCaptureArmed(), juce::dontSendNotification);

    juce::String s;
    if (proc.isAnalysing()) s = "Analyzing...";
    else
    {
        const auto& c = proc.getCapture();
        const double secs = c.length.load() / juce::jmax (1.0, proc.getCurrentSampleRate());
        s << (int) proc.getTriggers().size() << " triggers";
        if (wave.getNumSelected() > 0) s << " (" << wave.getNumSelected() << " selected)";
        s << "   |   " << juce::String (secs, 1) << " s captured";
        if (proc.isCaptureArmed()) s << (proc.isTransportPlaying() ? "  - recording" : "  - waiting for playback");
    }
    status.setText (s, juce::dontSendNotification);

    const int slot = [this]
    {
        const int sl = (int) proc.apvts.getRawParameterValue (ids::repSample)->load() - 1;
        return sl >= 0 ? sl : (int) proc.apvts.getRawParameterValue (ids::instrument)->load();
    }();
    const auto m = proc.getKitManager().getSlot (slot);
    int files = 0;
    for (auto& l : m.layers) files += l.files.size();
    sampleInfo.setText (files == 0 ? juce::String ("No samples for ") + getInstrumentName ((Instrument) slot) + " - drop WAVs here or use SAMPLES"
                                   : juce::String (files) + " samples, " + juce::String ((int) m.layers.size()) + " velocity layers",
                        juce::dontSendNotification);
}

void ReplacePage::exportMidi()
{
    chooser = std::make_unique<juce::FileChooser> ("Export triggers as MIDI",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Thruma.mid"),
                                                   "*.mid");
    juce::Component::SafePointer<ReplacePage> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (! f.hasFileExtension ("mid")) f = f.withFileExtension ("mid");
        safe->status.setText (safe->proc.exportMidi (f) ? "Exported " + f.getFileName() : "Could not write " + f.getFullPathName(),
                              juce::dontSendNotification);
    });
}

bool ReplacePage::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (KitManager::isAudioFile (juce::File (f))) return true;
    return false;
}

void ReplacePage::filesDropped (const juce::StringArray& files, int, int)
{
    const int slot = [this]
    {
        const int sl = (int) proc.apvts.getRawParameterValue (ids::repSample)->load() - 1;
        return sl >= 0 ? sl : (int) proc.apvts.getRawParameterValue (ids::instrument)->load();
    }();
    juce::Array<juce::File> all;
    for (auto& l : proc.getKitManager().getSlot (slot).layers) all.addArray (l.files);
    for (auto& f : files)
        if (KitManager::isAudioFile (juce::File (f))) all.addIfNotAlreadyThere (juce::File (f));
    proc.getKitManager().assignFiles (slot, all);
    proc.rebuildKit();
    updateStatus();
}

} // namespace thruma::ui
