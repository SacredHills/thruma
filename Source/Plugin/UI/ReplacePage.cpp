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
      dragMidi (p)
{
    addAndMakeVisible (trackPanel);
    addAndMakeVisible (controlPanel);
    for (juce::Component* c : std::initializer_list<juce::Component*> { &instrument, &sample, &hits, &info, &lane, &sensitivity, &velocity,
                                                                        &blend, &output, &dragMidi, &saveBtn, &clearBtn })
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
    auto bottom = r.removeFromBottom (112);
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
    info.setBounds (t);
    lane.setBounds (r);

    controlPanel.setBounds (bottom);
    auto c = controlPanel.getContentBounds();
    auto buttons = c.removeFromRight (150);
    c.removeFromRight (20);
    dragMidi.setBounds (buttons.removeFromTop (34));
    buttons.removeFromTop (8);
    auto row = buttons.removeFromTop (30);
    saveBtn.setBounds (row.removeFromLeft (row.getWidth() / 2 + 10));
    row.removeFromLeft (6);
    clearBtn.setBounds (row);
    const int colW = (c.getWidth() - 24) / 2;
    auto col1 = c.removeFromLeft (colW);
    c.removeFromLeft (24);
    layoutRows (col1, { &sensitivity, &velocity }, 30, 10);
    layoutRows (c, { &blend, &output }, 30, 10);
}

void ReplacePage::timerCallback()
{
    hits.setText (juce::String ((int) proc.getTriggers().size()) + " notes", juce::dontSendNotification);
    const auto src = (TriggerSource) (int) proc.apvts.getRawParameterValue (ids::repSource)->load();
    const auto slot = proc.getKitManager().getSlot (currentSlot());
    int files = 0;
    for (auto& l : slot.layers) files += l.files.size();

    juce::String s;
    s << (proc.isTransportPlaying() ? "Listening..." : "Stopped") << "\n\n";
    s << (src == TriggerSource::Edited ? "Playing your edited notes.\nClear to return to live detection."
                                       : "Live: hits are detected as the track plays.") << "\n\n";
    if (files == 0) s << "No samples for " << getInstrumentName ((Instrument) currentSlot()) << " yet: drop WAV files here or use SAMPLES. "
                         "The original drum plays until then.";
    else s << files << " samples, " << (int) slot.layers.size() << " velocity layers";
    if (message.isNotEmpty() && juce::Time::getMillisecondCounter() - messageTime < 6000)
        s = message + "\n\n" + s;
    info.setText (s, juce::dontSendNotification);
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
    proc.getKitManager().assignFiles (currentSlot(), all);
    proc.rebuildKit();
}

} // namespace thruma::ui
