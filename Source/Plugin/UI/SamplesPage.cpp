#include "SamplesPage.h"

namespace thruma::ui
{

//==============================================================================
LayerRow::LayerRow (SamplesPage& owner, int layerIndex) : page (owner), layer (layerIndex)
{
    const auto slot = page.getProcessor().getKitManager().getSlot (page.getSelectedInstrument());
    const auto& L = slot.layers[(size_t) layer];

    title.setText ("Layer " + juce::String (layer + 1), juce::dontSendNotification);
    title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addAndMakeVisible (title);

    for (auto* s : { &velLo, &velHi })
    {
        s->setSliderStyle (juce::Slider::IncDecButtons);
        s->setTextBoxStyle (juce::Slider::TextBoxLeft, false, 40, 22);
        s->setRange (1, 127, 1);
        s->setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        addAndMakeVisible (s);
    }
    velLo.setValue (juce::jlimit (1, 127, (int) std::lround (L.velLo * 127.0f)), juce::dontSendNotification);
    velHi.setValue (juce::jlimit (1, 127, (int) std::lround (L.velHi * 127.0f)), juce::dontSendNotification);
    velLo.setTooltip ("Lowest velocity for this layer");
    velHi.setTooltip ("Highest velocity for this layer");
    auto apply = [this]
    {
        auto& km = page.getProcessor().getKitManager();
        auto s = km.getSlot (page.getSelectedInstrument());
        if (layer >= (int) s.layers.size()) return;
        s.layers[(size_t) layer].velLo = (float) velLo.getValue() / 127.0f;
        s.layers[(size_t) layer].velHi = (float) juce::jmax (velLo.getValue(), velHi.getValue()) / 127.0f;
        km.setSlot (page.getSelectedInstrument(), s);
        page.getProcessor().rebuildKit();
    };
    velLo.onValueChange = apply;
    velHi.onValueChange = apply;

    removeBtn.onClick = [this]
    {
        page.getProcessor().getKitManager().removeLayer (page.getSelectedInstrument(), layer);
        juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<SamplesPage> (&page)] { if (sp != nullptr) sp->kitChanged(); });
    };
    addAndMakeVisible (removeBtn);

    for (int i = 0; i < L.files.size(); ++i)
    {
        const auto f = L.files[i];
        hashes.push_back ((uint64_t) std::hash<std::string>{} (f.getFullPathName().toStdString()));
        auto* b = chips.add (new juce::TextButton (f.getFileNameWithoutExtension()));
        b->setTooltip (f.getFullPathName() + (f.existsAsFile() ? "\nClick to preview, Shift-click to remove." : "\nMISSING FILE"));
        if (! f.existsAsFile()) b->setColour (juce::TextButton::buttonColourId, colours::danger.withAlpha (0.4f));
        b->onClick = [this, b, f, i]
        {
            const auto mods = juce::ModifierKeys::currentModifiers;
            if (mods.isShiftDown() || mods.isPopupMenu())
            {
                page.getProcessor().getKitManager().removeFile (page.getSelectedInstrument(), layer, i);
                juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<SamplesPage> (&page)] { if (sp != nullptr) sp->kitChanged(); });
            }
            else page.getProcessor().previewSample (f);
            juce::ignoreUnused (b);
        };
        b->setTriggeredOnMouseDown (false);
        addAndMakeVisible (b);
    }
}

void LayerRow::flash (uint64_t sampleHash, float velocity)
{
    for (size_t i = 0; i < hashes.size() && i < (size_t) chips.size(); ++i)
        if (hashes[i] == sampleHash)
        {
            flashIdx = (int) i;
            flashLevel = 1.0f;
            flashVel = velocity;
            repaint();
            return;
        }
}

bool LayerRow::decay()
{
    if (flashLevel <= 0.0f) return false;
    flashLevel = juce::jmax (0.0f, flashLevel - 0.06f);
    repaint();
    return flashLevel > 0.0f;
}

void LayerRow::paintOverChildren (juce::Graphics& g)
{
    if (flashIdx < 0 || flashIdx >= chips.size()) return;
    const auto b = chips[flashIdx]->getBounds().toFloat().expanded (2.0f);
    // the last-played sample keeps a thin outline; a fresh hit lights it up
    g.setColour (colours::accent.withAlpha (0.25f + 0.55f * flashLevel));
    g.drawRoundedRectangle (b, 5.0f, 2.0f);
    if (flashLevel > 0.0f)
    {
        g.setColour (colours::accent.withAlpha (0.35f * flashLevel));
        g.fillRoundedRectangle (b, 5.0f);
    }
    g.setColour (colours::accent);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("PLAYED  vel " + juce::String (juce::roundToInt (flashVel * 127.0f)),
                juce::Rectangle<int> (getWidth() - 136, (int) b.getY(), 120, (int) b.getHeight()), juce::Justification::centredRight);
}

void LayerRow::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (dragOver ? colours::accent.withAlpha (0.15f) : colours::panelRaised);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (dragOver ? colours::accent : colours::outline);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
    g.setColour (colours::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("vel", 96, 8, 24, 22, juce::Justification::centredLeft);
    g.drawText ("-", 214, 8, 12, 22, juce::Justification::centred);
}

void LayerRow::resized()
{
    auto r = getLocalBounds().reduced (8, 6);
    auto top = r.removeFromTop (24);
    title.setBounds (top.removeFromLeft (84));
    top.removeFromLeft (30);
    velLo.setBounds (top.removeFromLeft (92));
    top.removeFromLeft (14);
    velHi.setBounds (top.removeFromLeft (92));
    removeBtn.setBounds (top.removeFromRight (70));
    r.removeFromTop (6);
    int x = r.getX(), y = r.getY();
    for (auto* c : chips)
    {
        const int w = juce::jlimit (60, 180, juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (13.0f), c->getButtonText()) + 20);
        if (x + w > r.getRight() - 130 && x > r.getX()) { x = r.getX(); y += 28; }   // right: "PLAYED"
        c->setBounds (x, y, w, 24);
        x += w + 6;
    }
}

void LayerRow::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    juce::Array<juce::File> fs;
    for (auto& f : files) if (KitManager::isAudioFile (juce::File (f))) fs.add (juce::File (f));
    page.getProcessor().getKitManager().addFilesToLayer (page.getSelectedInstrument(), layer, fs);
    juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<SamplesPage> (&page)] { if (sp != nullptr) sp->kitChanged(); });
}

//==============================================================================
SamplesPage::SamplesPage (ThrumaProcessor& p) : proc (p)
{
    browser = std::make_unique<juce::FileBrowserComponent> (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::canSelectMultipleItems | juce::FileBrowserComponent::filenameBoxIsReadOnly,
        juce::File::getSpecialLocation (juce::File::userMusicDirectory), &filter, nullptr);
    browser->addListener (this);
    addAndMakeVisible (browserPanel);
    addAndMakeVisible (*browser);

    addBtn.setTooltip ("Add the selected files to the selected instrument (auto-sorted into velocity layers by loudness).");
    importBtn.setTooltip ("Import a whole folder: files are assigned to instruments by name (kick, snare, tom, hat, crash, ride, china, splash).");
    addBtn.onClick = [this] { addFiles (browserSelection()); };
    importBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Import sample folder", browser->getRoot());
        juce::Component::SafePointer<SamplesPage> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            juce::StringArray unassigned;
            const int n = safe->proc.getKitManager().importFolder (fc.getResult(), &unassigned);
            safe->hint.setText ("Imported " + juce::String (n) + " files" + (unassigned.isEmpty() ? juce::String() : ", " + juce::String (unassigned.size()) + " not recognised by name"),
                                juce::dontSendNotification);
            safe->kitChanged();
        });
    };
    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load kit", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), "*.thrumakit");
        juce::Component::SafePointer<SamplesPage> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            if (! safe->proc.getKitManager().loadKitFile (fc.getResult()))
                safe->hint.setText ("Not a Thruma kit file.", juce::dontSendNotification);
            safe->kitChanged();
        });
    };
    saveBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Save kit", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                                        .getChildFile (proc.getKitManager().getKitName() + ".thrumakit"), "*.thrumakit");
        juce::Component::SafePointer<SamplesPage> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            auto f = fc.getResult().withFileExtension ("thrumakit");
            safe->proc.getKitManager().setKitName (f.getFileNameWithoutExtension());
            safe->hint.setText (safe->proc.getKitManager().saveKitFile (f) ? "Saved " + f.getFileName() : "Could not save kit.", juce::dontSendNotification);
            safe->kitChanged();
        });
    };
    clearAllBtn.onClick = [this]
    {
        proc.getKitManager().clearAll();
        proc.getKitManager().setKitName ("Untitled Kit");
        kitChanged();
    };
    for (auto* b : { &addBtn, &importBtn, &loadBtn, &saveBtn, &clearAllBtn }) addAndMakeVisible (b);

    addAndMakeVisible (kitPanel);
    kitName.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    addAndMakeVisible (kitName);
    instrumentList.setModel (this);
    instrumentList.setRowHeight (30);
    instrumentList.setColour (juce::ListBox::backgroundColourId, colours::panel);
    addAndMakeVisible (instrumentList);
    instrumentList.selectRow (selectedInstrument);

    addAndMakeVisible (detailPanel);
    detailTitle.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    addAndMakeVisible (detailTitle);
    midiLabel.setText ("MIDI note", juce::dontSendNotification);
    midiLabel.setColour (juce::Label::textColourId, colours::textDim);
    addAndMakeVisible (midiLabel);
    midiNote.setSliderStyle (juce::Slider::IncDecButtons);
    midiNote.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 24);
    midiNote.setRange (0, 127, 1);
    midiNote.onValueChange = [this]
    {
        auto s = proc.getKitManager().getSlot (selectedInstrument);
        s.midiNote = (int) midiNote.getValue();
        proc.getKitManager().setSlot (selectedInstrument, s);
        proc.rebuildKit();
    };
    addAndMakeVisible (midiNote);
    rrMode.addItem ("Round robin", 1);
    rrMode.addItem ("Random", 2);
    rrMode.setTooltip ("How samples within a velocity layer alternate.");
    rrMode.onChange = [this]
    {
        auto s = proc.getKitManager().getSlot (selectedInstrument);
        s.rrMode = rrMode.getSelectedId() == 2 ? RoundRobinMode::Random : RoundRobinMode::Cycle;
        proc.getKitManager().setSlot (selectedInstrument, s);
        proc.rebuildKit();
    };
    addAndMakeVisible (rrMode);
    autoVelBtn.setTooltip ("Spread velocity ranges evenly across the layers.");
    autoVelBtn.onClick = [this] { proc.getKitManager().autoDistributeVelocities (selectedInstrument); kitChanged(); };
    clearInstBtn.onClick = [this] { proc.getKitManager().clearSlot (selectedInstrument); kitChanged(); };
    addAndMakeVisible (autoVelBtn);
    addAndMakeVisible (clearInstBtn);

    layerViewport.setViewedComponent (&layerHolder, false);
    layerViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (layerViewport);

    hint.setColour (juce::Label::textColourId, colours::textDim);
    hint.setFont (juce::FontOptions (12.0f));
    hint.setText ("Drop WAV files on a layer to add round-robin samples, or anywhere here to auto-layer them by loudness.",
                  juce::dontSendNotification);
    addAndMakeVisible (hint);

    for (auto* panel : { &browserPanel, &kitPanel, &detailPanel }) panel->toBack();

    // pads can be renamed (double-click the title)
    detailTitle.onTextChange = [this]
    {
        if (! isPadSlot (selectedInstrument)) return;
        proc.getKitManager().setSlotName (selectedInstrument, detailTitle.getText());
        instrumentList.repaint();
    };
    for (int i = 0; i < kNumKitSlots; ++i) seenHits[(size_t) i] = proc.getHitMonitor().slots[(size_t) i].count.load();
    refreshDetail();
    startTimerHz (30);
}

int SamplesPage::getNumRows()
{
    return kNumInstruments + juce::jmin (kNumPads, proc.getKitManager().getNumUsedPads() + 1);   // + one empty pad
}

void SamplesPage::selectSlot (int slot)
{
    selectedInstrument = juce::jlimit (0, kNumKitSlots - 1, slot);
    instrumentList.updateContent();
    instrumentList.selectRow (selectedInstrument);
    refreshDetail();
}

void SamplesPage::timerCallback()
{
    if (! isVisible()) return;   // the hidden tab skips the work
    bool listDirty = false;
    const auto& hm = proc.getHitMonitor();
    for (int i = 0; i < kNumKitSlots; ++i)
    {
        const auto c = hm.slots[(size_t) i].count.load (std::memory_order_acquire);
        if (c != seenHits[(size_t) i])
        {
            seenHits[(size_t) i] = c;
            rowFlash[(size_t) i] = 1.0f;
            listDirty = true;
            if (i == selectedInstrument)
                for (auto* r : rows) r->flash (hm.slots[(size_t) i].sampleHash.load(), hm.slots[(size_t) i].velocity.load());
        }
        else if (rowFlash[(size_t) i] > 0.0f)
        {
            rowFlash[(size_t) i] = juce::jmax (0.0f, rowFlash[(size_t) i] - 0.08f);
            listDirty = true;
        }
    }
    for (auto* r : rows) r->decay();
    if (listDirty) instrumentList.repaint();
}

SamplesPage::~SamplesPage()
{
    browser->removeListener (this);
    instrumentList.setModel (nullptr);
}

void SamplesPage::paint (juce::Graphics&) {}

void SamplesPage::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto left = r.removeFromLeft (juce::jmin (330, r.getWidth() / 3));
    r.removeFromLeft (10);
    browserPanel.setBounds (left);
    auto b = browserPanel.getContentBounds();
    auto btns = b.removeFromBottom (28);
    addBtn.setBounds (btns.removeFromLeft (btns.getWidth() / 2 - 3));
    btns.removeFromLeft (6);
    importBtn.setBounds (btns);
    b.removeFromBottom (6);
    browser->setBounds (b);

    auto mid = r.removeFromLeft (210);
    r.removeFromLeft (10);
    kitPanel.setBounds (mid);
    auto k = kitPanel.getContentBounds();
    kitName.setBounds (k.removeFromTop (24));
    auto kb = k.removeFromBottom (28 * 3 + 12);
    loadBtn.setBounds (kb.removeFromTop (28)); kb.removeFromTop (6);
    saveBtn.setBounds (kb.removeFromTop (28)); kb.removeFromTop (6);
    clearAllBtn.setBounds (kb.removeFromTop (28));
    k.removeFromBottom (8);
    instrumentList.setBounds (k);

    detailPanel.setBounds (r);
    auto d = r.reduced (14, 12);
    auto top = d.removeFromTop (30);
    detailTitle.setBounds (top.removeFromLeft (150));
    clearInstBtn.setBounds (top.removeFromRight (64));
    top.removeFromRight (6);
    autoVelBtn.setBounds (top.removeFromRight (110));
    d.removeFromTop (8);
    auto second = d.removeFromTop (28);
    midiLabel.setBounds (second.removeFromLeft (70));
    midiNote.setBounds (second.removeFromLeft (100));
    second.removeFromLeft (16);
    rrMode.setBounds (second.removeFromLeft (140));
    d.removeFromTop (10);
    hint.setBounds (d.removeFromBottom (20));
    d.removeFromBottom (6);
    layerViewport.setBounds (d);

    const int w = layerViewport.getWidth() - layerViewport.getScrollBarThickness() - 2;
    int y = 0;
    for (auto* row : rows)
    {
        // height grows with the number of sample chips
        const int chips = juce::jmax (1, row->getNumChildComponents() - 4);
        const int perLine = juce::jmax (1, (w - 146) / 120);
        const int h = 46 + ((chips + perLine - 1) / perLine) * 28;
        row->setBounds (0, y, w, h);
        y += h + 8;
    }
    layerHolder.setSize (w, juce::jmax (y, layerViewport.getHeight()));
}

void SamplesPage::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    const auto slot = proc.getKitManager().getSlot (row);
    int files = 0;
    for (auto& l : slot.layers) files += l.files.size();
    if (selected)
    {
        g.setColour (colours::accent.withAlpha (0.2f));
        g.fillRoundedRectangle (2.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 4.0f);
    }
    if (const float fl = row < kNumKitSlots ? rowFlash[(size_t) row] : 0.0f; fl > 0.0f)
    {
        g.setColour (colours::accent.withAlpha (0.3f * fl));
        g.fillRoundedRectangle (2.0f, 1.0f, (float) w - 4.0f, (float) h - 2.0f, 4.0f);
    }
    if (row == kNumInstruments)
    {
        g.setColour (colours::outline);
        g.drawHorizontalLine (0, 6.0f, (float) w - 6.0f);
    }
    g.setColour (files > 0 ? colours::text : colours::textDim);
    g.setFont (juce::FontOptions (14.0f));
    auto name = proc.getKitManager().getSlotName (row);
    if (isPadSlot (row) && files == 0 && slot.name.isEmpty()) name = "+ " + name;
    g.drawText (name, 10, 0, w - 60, h, juce::Justification::centredLeft);
    g.setColour (colours::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (files > 0 ? juce::String (files) : juce::String ("-"), w - 50, 0, 40, h, juce::Justification::centredRight);
}

void SamplesPage::selectedRowsChanged (int last)
{
    if (last < 0) return;
    selectedInstrument = last;
    refreshDetail();
}

void SamplesPage::fileClicked (const juce::File& f, const juce::MouseEvent&)
{
    if (KitManager::isAudioFile (f)) proc.previewSample (f);
}

void SamplesPage::fileDoubleClicked (const juce::File& f)
{
    if (KitManager::isAudioFile (f)) addFiles ({ f });
}

juce::Array<juce::File> SamplesPage::browserSelection() const
{
    juce::Array<juce::File> out;
    for (int i = 0; i < browser->getNumSelectedFiles(); ++i)
        if (KitManager::isAudioFile (browser->getSelectedFile (i))) out.add (browser->getSelectedFile (i));
    return out;
}

void SamplesPage::addFiles (const juce::Array<juce::File>& files)
{
    if (files.isEmpty()) return;
    auto& km = proc.getKitManager();
    juce::Array<juce::File> all;
    for (auto& l : km.getSlot (selectedInstrument).layers) all.addArray (l.files);
    for (auto& f : files) all.addIfNotAlreadyThere (f);
    km.assignFiles (selectedInstrument, all);
    kitChanged();
}

void SamplesPage::filesDropped (const juce::StringArray& files, int, int)
{
    juce::Array<juce::File> fs;
    for (auto& f : files)
    {
        juce::File file (f);
        if (file.isDirectory())
        {
            proc.getKitManager().importFolder (file);
            continue;
        }
        if (KitManager::isAudioFile (file)) fs.add (file);
    }
    if (! fs.isEmpty()) addFiles (fs);
    else kitChanged();
}

void SamplesPage::kitChanged()
{
    proc.rebuildKit();
    const auto missing = proc.getMissingSampleFiles();
    if (! missing.isEmpty())
        hint.setText (juce::String (missing.size()) + " sample file(s) missing - they are shown in red.", juce::dontSendNotification);
    instrumentList.updateContent();
    instrumentList.repaint();
    refreshDetail();
}

void SamplesPage::refreshDetail()
{
    kitName.setText (proc.getKitManager().getKitName(), juce::dontSendNotification);
    detailTitle.setText (proc.getKitManager().getSlotName (selectedInstrument), juce::dontSendNotification);
    detailTitle.setEditable (false, isPadSlot (selectedInstrument), false);
    detailTitle.setTooltip (isPadSlot (selectedInstrument) ? "Double-click to rename this pad" : juce::String());
    const auto slot = proc.getKitManager().getSlot (selectedInstrument);
    midiNote.setValue (juce::jmax (0, slot.midiNote), juce::dontSendNotification);
    rrMode.setSelectedId (slot.rrMode == RoundRobinMode::Random ? 2 : 1, juce::dontSendNotification);

    rows.clear();
    for (int i = 0; i < (int) slot.layers.size(); ++i)
        layerHolder.addAndMakeVisible (rows.add (new LayerRow (*this, i)));
    resized();
}

} // namespace thruma::ui
