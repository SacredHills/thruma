#include "ReplacePage.h"

namespace thruma::ui
{

void MidiDragSource::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (dragging ? colours::accent.withAlpha (0.35f) : colours::panelRaised);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (colours::accent);
    g.drawRoundedRectangle (r, 5.0f, 1.2f);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("Drag MIDI", getLocalBounds(), juce::Justification::centred);
}

void MidiDragSource::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging || e.getDistanceFromDragStart() < 6) return;
    if (proc.getTriggers().size() == 0)
    {
        if (onMessage) onMessage ("No notes yet - play the track first.");
        return;
    }
    const auto inst = (int) proc.apvts.getRawParameterValue (ids::instrument)->load();
    auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile (juce::String ("Thruma ") + getInstrumentName ((Instrument) inst) + ".mid");
    if (! proc.exportMidi (file)) { if (onMessage) onMessage ("Could not write the MIDI file."); return; }
    dragging = true;
    repaint();
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                               [safe = juce::Component::SafePointer<MidiDragSource> (this)]
                                                               { if (safe != nullptr) { safe->dragging = false; safe->repaint(); } });
}

ReplacePage::ReplacePage (ThrumaProcessor& p)
    : proc (p),
      instrument (p.apvts, ids::instrument, "Instrument", "The drum on this track."),
      sample (p.apvts, ids::repSample, "Sample", "Which kit sound plays for the detected hits."),
      lane (p),
      sensitivity (p.apvts, ids::repSens, "Sensitivity", "Same as dragging the threshold line: higher catches quieter hits, lower ignores more bleed."),
      velocity (p.apvts, ids::repVelResp, "Velocity", "0 % = every hit at full level, 100 % = follow the played dynamics."),
      blend (p.apvts, ids::repBlend, "Blend", "0 % = original drum, 100 % = replacement sample."),
      output (p.apvts, ids::repOutput, "Output", "Output level."),
      amount (p.apvts, ids::repAmount, "Amount", "How easily a possible hit is noticed. Raise it when quiet or fast hits are missed "
                                                  "(more dim trigger points appear); lower it if notes appear where nothing was played."),
      match (p.apvts, ids::repMatch, "Tolerance", "How different from this drum's own sound a hit may be. Raise it when real hits are "
                                                    "marked 'sounded like bleed'; lower it when bleed from other drums becomes notes."),
      dragMidi (p),
      advanced (p.apvts)
{
    addAndMakeVisible (trackPanel);
    addAndMakeVisible (controlPanel);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &instrument, &sample, &hits, &info, &lane, &sensitivity, &velocity,
                                                                        &blend, &output, &dragMidi, &saveBtn, &clearBtn, &finalizeBtn, &importBtn,
                                                                        &analyzeBtn, &importMidiBtn, &playBtn, &amount, &match, &advancedBtn })
        addAndMakeVisible (c);
    trackPanel.toBack();
    controlPanel.toBack();

    hits.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    info.setFont (juce::FontOptions (12.0f));
    info.setColour (juce::Label::textColourId, colours::textDim);
    info.setJustificationType (juce::Justification::topLeft);
    dragMidi.setTooltip ("Drag into your DAW to drop the detected hits as a MIDI clip (positions from the start of the song).");
    dragMidi.onMessage = [this] (const juce::String& m) { showMessage (m); };
    saveBtn.onClick = [this] { saveMidi(); };
    clearBtn.setTooltip ("Remove all notes and go back to live detection.");
    clearBtn.onClick = [this] { proc.clearAll(); };
    finalizeBtn.setClickingTogglesState (true);
    finalizeBtn.setTooltip ("Keep the current notes exactly as they are: playback uses them and nothing is re-detected. "
                            "Click again to go back to live detection.");
    finalizeBtn.onClick = [this] { proc.setFinalized (finalizeBtn.getToggleState()); timerCallback(); };
    importBtn.setTooltip ("Load an audio file of this drum track (exported from the start of the song) and detect its hits offline.");
    importBtn.onClick = [this]
    {
        juce::Component::SafePointer<ReplacePage> safe (this);
        chooser = std::make_unique<juce::FileChooser> ("Import drum track", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult() != juce::File()) safe->importFile (fc.getResult());
        });
    };
    lane.onAudioFileDropped = [this] (const juce::File& f) { importFile (f); };
    lane.onMidiFileDropped = [this] (const juce::File& f) { importMidi (f); };
    lane.onMessage = [this] (const juce::String& m) { showMessage (m); };

    analyzeBtn.setTooltip ("Track the whole song at once, without playing it (where your DAW lets plugins read the track; "
                           "otherwise analyses what has been played so far). The result is finalized; changing Sensitivity "
                           "or the threshold re-analyses it, keeping notes you edited by hand.");
    analyzeBtn.onClick = [this]
    {
        showMessage ("Analysing...");
        juce::Component::SafePointer<ReplacePage> safe (this);
        proc.analyseTrack ([safe] (const juce::String& m)
        {
            if (safe == nullptr) return;
            safe->showMessage (m);
            safe->lane.zoomToFit();
        });
    };
    importMidiBtn.setTooltip ("Load a MIDI file: its notes play the kit in time with your song (no analysis). "
                              "Or put Thruma on a MIDI/instrument track and play it from MIDI clips.");
    importMidiBtn.onClick = [this]
    {
        juce::Component::SafePointer<ReplacePage> safe (this);
        chooser = std::make_unique<juce::FileChooser> ("Import MIDI file", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.mid;*.midi");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult() != juce::File()) safe->importMidi (fc.getResult());
        });
    };
    playBtn.setTooltip ("Play / stop. Imported audio plays inside Thruma (click the time ruler to choose where); "
                        "in DAWs that allow it, this runs the DAW's transport.");
    playBtn.onClick = [this] { proc.togglePlay(); timerCallback(); };
    advancedBtn.setTooltip ("Ghost notes, retrigger time, dynamic range, double hits and frequency focus.");
    advancedBtn.setClickingTogglesState (true);
    advancedBtn.onClick = [this] { showAdvanced(); };
    addChildComponent (advanced);
    startTimerHz (6);
    timerCallback();
}

int ReplacePage::currentSlot() const
{
    const int sl = (int) proc.apvts.getRawParameterValue (ids::repSample)->load() - 1;
    return sl >= 0 ? sl : (int) proc.apvts.getRawParameterValue (ids::instrument)->load();
}

void ReplacePage::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto bottom = r.removeFromBottom (200);
    r.removeFromBottom (10);

    auto left = r.removeFromLeft (190);
    r.removeFromLeft (10);
    trackPanel.setBounds (left);
    auto t = trackPanel.getContentBounds();
    instrument.setBounds (t.removeFromTop (46));
    t.removeFromTop (8);
    sample.setBounds (t.removeFromTop (46));
    t.removeFromTop (16);
    hits.setBounds (t.removeFromTop (30));
    saveBtn.setBounds (t.removeFromBottom (30));
    t.removeFromBottom (6);
    dragMidi.setBounds (t.removeFromBottom (30));
    t.removeFromBottom (8);
    info.setBounds (t);
    lane.setBounds (r);

    controlPanel.setBounds (bottom);
    auto c = controlPanel.getContentBounds();
    auto buttons = c.removeFromRight (360);
    c.removeFromRight (20);
    auto place = [] (juce::Rectangle<int> row, std::initializer_list<juce::Component*> cs)
    {
        const int w = (row.getWidth() - 6 * ((int) cs.size() - 1)) / (int) cs.size();
        for (auto* comp : cs) { comp->setBounds (row.removeFromLeft (w)); row.removeFromLeft (6); }
    };
    place (buttons.removeFromTop (34), { &analyzeBtn, &importBtn, &importMidiBtn });
    buttons.removeFromTop (10);
    place (buttons.removeFromTop (34), { &playBtn, &finalizeBtn, &clearBtn });
    const int colW = (c.getWidth() - 24) / 2;
    auto col1 = c.removeFromLeft (colW);
    c.removeFromLeft (24);
    layoutRows (col1.removeFromTop (70), { &sensitivity, &velocity }, 30, 10);
    layoutRows (c.removeFromTop (70), { &blend, &output }, 30, 10);

    // Amount and Tolerance: two knobs centred under the Velocity slider's track
    col1.removeFromTop (8);
    const int nameW = juce::jmin (130, velocity.getWidth() / 3);
    const int trackL = velocity.getX() + nameW, trackR = velocity.getRight() - 72;
    const int knobW = 84, gap = 22, cx = (trackL + trackR) / 2;
    const int knobsH = juce::jmin (col1.getHeight(), 100);
    amount.setBounds (cx - gap / 2 - knobW, col1.getY(), knobW, knobsH);
    match.setBounds (cx + gap / 2, col1.getY(), knobW, knobsH);
    c.removeFromTop (8);
    advancedBtn.setBounds (c.getX() + juce::jmin (130, c.getWidth() / 3), c.getY() + 4, 130, 30);
    const int advW = 430, advH = 5 * 28 + 4 * 6 + 46;
    advanced.setBounds (juce::jlimit (lane.getX() + 8, lane.getRight() - advW - 8, advancedBtn.getX() - 40),
                        controlPanel.getY() - advH - 6, advW, advH);
}

ReplacePage::AdvancedPanel::AdvancedPanel (APVTS& s)
    : ghost (s, ids::repGhost, "Ghost Notes", "Extends detection to quiet ghost notes (most useful on snare and hats)."),
      retrig (s, ids::repRetrig, "Retrigger", "Minimum time between hits. 0 = the drum's default. Lower it if fast hits are missed as 'too soon'."),
      dynRange (s, ids::repDynRange, "Dynamic Range", "Level range mapped onto velocity 1-127."),
      doubleHits (s, ids::repDouble, "Double hits", "Detect flams, drags and fast doubles while the previous hit still rings."),
      focus (s, ids::repFocus, "Frequency focus", "Reject hits whose sound does not match the drum (bleed protection).")
{
    addAndMakeVisible (bg);
    for (auto* c : std::initializer_list<juce::Component*> { &ghost, &retrig, &dynRange, &doubleHits, &focus }) addAndMakeVisible (c);
}

void ReplacePage::AdvancedPanel::resized()
{
    bg.setBounds (getLocalBounds());
    layoutRows (bg.getContentBounds(), { &ghost, &retrig, &dynRange, &doubleHits, &focus }, 28, 6);
}

void ReplacePage::showAdvanced()
{
    advanced.setVisible (advancedBtn.getToggleState());
    if (advanced.isVisible()) advanced.toFront (false);
    resized();
}

void ReplacePage::timerCallback()
{
    hits.setText (juce::String ((int) proc.getTriggers().size()) + " notes", juce::dontSendNotification);
    const auto src = (TriggerSource) (int) proc.apvts.getRawParameterValue (ids::repSource)->load();
    const auto slot = proc.getKitManager().getSlot (currentSlot());
    int files = 0;
    for (auto& l : slot.layers) files += l.files.size();

    if (proc.getOfflineMessageCount() != lastOfflineMessageCount)
    {
        lastOfflineMessageCount = proc.getOfflineMessageCount();
        showMessage (proc.getOfflineMessage());
        return;
    }
    playBtn.setEnabled (proc.canTogglePlay());
    const auto playText = proc.isPlayingNow() ? "Stop" : "Play";
    if (playBtn.getButtonText() != playText) playBtn.setButtonText (playText);
    analyzeBtn.setEnabled (! proc.isAnalysing());

    juce::String s;
    using OS = ThrumaProcessor::OfflineSource;
    const auto off = proc.getOfflineSource();
    if (proc.isMidiDriving()) s << "MIDI is playing the kit - audio tracking and replacing are paused.\n";
    if (off == OS::File) s << "Imported audio: " << proc.getOfflineName() << (proc.isPreviewPlaying() ? " (playing in Thruma)" : "");
    else if (off == OS::HostTrack) s << "Analysed the whole track";
    else if (off == OS::Heard) s << "Analysed what was played";
    else if (src == TriggerSource::Edited) s << "Finalized";
    else switch (proc.getTransportMode())
    {
        case 1:  s << "Listening (following your DAW)"; break;
        case 2:  s << "Listening (your DAW sends no song position, using Thruma's clock)"; break;
        default: s << "Waiting for playback..."; break;
    }
    s << "\n\n";
    if (finalizeBtn.getToggleState() != (src == TriggerSource::Edited))
        finalizeBtn.setToggleState (src == TriggerSource::Edited, juce::dontSendNotification);
    if (off != OS::None)
        s << "Change Sensitivity or drag the threshold: the notes are re-analysed (hand-edited notes stay). "
             "Click Finalize to go back to live detection.\n\n";
    else
        s << (src == TriggerSource::Edited ? "FINALIZED: these notes play back exactly as shown. Click Finalize again to detect live."
                                           : "Live: hits are detected as the track plays. Edit notes, then Finalize to keep them.") << "\n\n";
    if (proc.getMidiFileName().isNotEmpty())
        s << "MIDI file: " << proc.getMidiFileName() << " (" << proc.getMidiFileNoteCount() << " notes) plays the kit with the song.\n\n";
    if (files == 0) s << "No samples for " << getInstrumentName ((Instrument) currentSlot()) << " yet: drop WAV files here or use SAMPLES. "
                         "The original drum plays unchanged until then (Blend then only sets the level of MIDI-played samples).";
    else s << files << " samples, " << (int) slot.layers.size() << " velocity layers";
    if (message.isNotEmpty() && juce::Time::getMillisecondCounter() - messageTime < 6000)
        s = message + "\n\n" + s;
    info.setText (s, juce::dontSendNotification);
}

void ReplacePage::importFile (const juce::File& f)
{
    showMessage ("Analysing " + f.getFileName() + "...");
    proc.importAudio (f, [safe = juce::Component::SafePointer<ReplacePage> (this)] (const juce::String& m)
    {
        if (safe == nullptr) return;
        safe->showMessage (m);
        safe->lane.zoomToFit();
    });
}

void ReplacePage::importMidi (const juce::File& f)
{
    juce::String m;
    proc.importMidiFile (f, m);
    showMessage (m);
}

void ReplacePage::saveMidi()
{
    if (proc.getTriggers().size() == 0) { showMessage ("No notes yet - play the track first."); return; }
    const auto inst = (int) proc.apvts.getRawParameterValue (ids::instrument)->load();
    chooser = std::make_unique<juce::FileChooser> ("Save MIDI",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (juce::String ("Thruma ") + getInstrumentName ((Instrument) inst) + ".mid"),
        "*.mid");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe = juce::Component::SafePointer<ReplacePage> (this)] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        auto f = fc.getResult().withFileExtension ("mid");
        const bool ok = safe->proc.exportMidi (f);
        safe->showMessage (ok ? "Saved " + juce::String ((int) safe->proc.getTriggers().size()) + " notes to " + f.getFileName()
                              : "Could not write " + f.getFullPathName());
    });
}

bool ReplacePage::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (KitManager::isAudioFile (juce::File (f))) return true;
    return false;
}

void ReplacePage::filesDropped (const juce::StringArray& files, int, int)
{
    juce::Array<juce::File> all;
    for (auto& l : proc.getKitManager().getSlot (currentSlot()).layers) all.addArray (l.files);
    for (auto& f : files) if (KitManager::isAudioFile (juce::File (f))) all.addIfNotAlreadyThere (juce::File (f));
    const int slot = currentSlot();
    proc.runKitJob ([slot, all] (KitManager& km) { km.setProgressTotal (all.size()); km.assignFiles (slot, all); });
}

} // namespace thruma::ui
