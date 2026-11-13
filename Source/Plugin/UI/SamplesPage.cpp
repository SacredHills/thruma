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
        if (x + w > r.getRight() && x > r.getX()) { x = r.getX(); y += 28; }
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
    refreshDetail();
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
        const int perLine = juce::jmax (1, (w - 16) / 120);
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
    g.setColour (files > 0 ? colours::text : colours::textDim);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (getInstrumentName ((Instrument) row), 10, 0, w - 60, h, juce::Justification::centredLeft);
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
    instrumentList.repaint();
    refreshDetail();
}

void SamplesPage::refreshDetail()
{
    kitName.setText (proc.getKitManager().getKitName(), juce::dontSendNotification);
    detailTitle.setText (getInstrumentName ((Instrument) selectedInstrument), juce::dontSendNotification);
    const auto slot = proc.getKitManager().getSlot (selectedInstrument);
    midiNote.setValue (slot.midiNote, juce::dontSendNotification);
    rrMode.setSelectedId (slot.rrMode == RoundRobinMode::Random ? 2 : 1, juce::dontSendNotification);

    rows.clear();
    for (int i = 0; i < (int) slot.layers.size(); ++i)
        layerHolder.addAndMakeVisible (rows.add (new LayerRow (*this, i)));
    resized();
}

} // namespace thruma::ui
