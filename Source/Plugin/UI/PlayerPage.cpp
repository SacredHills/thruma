#include "PlayerPage.h"
#include "BinaryData.h"

namespace thruma::ui
{

static constexpr float kPhotoW = 1672.0f, kPhotoH = 941.0f;   // the hotspots are measured on the original photo
static constexpr int kPadH = 58, kPadMinW = 100, kPadGap = 8, kMaxPadRows = 4, kFooterH = 26, kTopH = 40;

PlayerPage::PlayerPage (ThrumaProcessor& p) : proc (p)
{
    photo = juce::ImageCache::getFromMemory (BinaryData::kit_jpg, BinaryData::kit_jpgSize);

    using I = Instrument;
    auto e = [this] (I i, float cx, float cy, float rx, float ry) { spots.push_back ({ (int) i, true, cx, cy, rx, ry }); };
    auto r = [this] (I i, float x1, float y1, float x2, float y2) { spots.push_back ({ (int) i, false, x1, y1, x2, y2 }); };
    // front-most first (hit testing takes the first match)
    r (I::Snare, 380, 413, 686, 588);
    r (I::Tom1, 578, 246, 795, 440);
    r (I::Tom2, 822, 242, 1042, 440);
    r (I::Tom3, 990, 423, 1258, 752);
    r (I::Tom4, 1242, 445, 1556, 792);
    e (I::Kick, 810, 615, 190, 182);
    r (I::Kick, 760, 715, 860, 895);        // pedal
    e (I::HiHat, 240, 290, 158, 36);
    r (I::HiHat, 175, 770, 320, 895);       // pedal
    e (I::Splash, 559, 207, 92, 20);
    e (I::Crash, 447, 133, 172, 40);
    e (I::Crash2, 1097, 133, 162, 42);
    e (I::China, 1413, 113, 150, 38);
    e (I::Ride, 1425, 263, 212, 56);

    importKitBtn.setTooltip ("Load a whole kit from a folder: drums by name, every other sound onto pads.");
    importKitBtn.onClick = [this] { chooseFiles (-1, false, true); };
    loadKitBtn.setTooltip ("Load a saved Thruma kit (.thrumakit).");
    loadKitBtn.onClick = [this]
    {
        juce::Component::SafePointer<PlayerPage> safe (this);
        chooser = std::make_unique<juce::FileChooser> ("Load kit", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.thrumakit;*.json");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult() != juce::File() && safe->proc.getKitManager().loadKitFile (fc.getResult())) safe->kitEdited();
        });
    };
    kitLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    kitLabel.setColour (juce::Label::textColourId, colours::text);
    for (auto* c : { (juce::Component*) &importKitBtn, (juce::Component*) &loadKitBtn, (juce::Component*) &kitLabel }) addAndMakeVisible (c);

    for (int i = 0; i < kNumKitSlots; ++i) seen[(size_t) i] = proc.getHitMonitor().slots[(size_t) i].count.load();
    startTimerHz (40);
}

void PlayerPage::resized()
{
    auto top = getLocalBounds().removeFromTop (kTopH).reduced (12, 6);
    loadKitBtn.setBounds (top.removeFromRight (110));
    top.removeFromRight (8);
    importKitBtn.setBounds (top.removeFromRight (160));
    kitLabel.setBounds (top);
}

int PlayerPage::numPadTiles() const
{
    return juce::jmin (kNumPads, proc.getKitManager().getNumUsedPads() + 1);   // + one empty pad to fill
}

int PlayerPage::padsPerRow() const
{
    return juce::jmax (1, (getWidth() - 24 + kPadGap) / (kPadMinW + kPadGap));
}

int PlayerPage::padRows() const
{
    return juce::jlimit (1, kMaxPadRows, (numPadTiles() + padsPerRow() - 1) / padsPerRow());
}

int PlayerPage::padsHeight() const
{
    return padRows() * (kPadH + 6);
}

juce::Rectangle<float> PlayerPage::imageBounds() const
{
    auto area = getLocalBounds().toFloat().withTrimmedTop ((float) kTopH).withTrimmedBottom ((float) (padsHeight() + kFooterH + 8));
    const float scale = juce::jmin (area.getWidth() / kPhotoW, area.getHeight() / kPhotoH);
    return juce::Rectangle<float> (kPhotoW * scale, kPhotoH * scale).withCentre (area.getCentre());
}

juce::Path PlayerPage::spotPath (const Spot& s) const
{
    const auto ib = imageBounds();
    const float k = ib.getWidth() / kPhotoW;
    juce::Path p;
    if (s.ellipse) p.addEllipse (ib.getX() + (s.a - s.c) * k, ib.getY() + (s.b - s.d) * k, 2.0f * s.c * k, 2.0f * s.d * k);
    else p.addRoundedRectangle (ib.getX() + s.a * k, ib.getY() + s.b * k, (s.c - s.a) * k, (s.d - s.b) * k, 10.0f * k);
    return p;
}

juce::Path PlayerPage::slotPath (int slot) const
{
    juce::Path p;
    for (const auto& s : spots) if (s.slot == slot) p.addPath (spotPath (s));
    return p;
}

juce::Rectangle<float> PlayerPage::padBounds (int pad) const
{
    const int perRow = padsPerRow();
    auto strip = getLocalBounds().toFloat().withTrimmedBottom ((float) kFooterH).removeFromBottom ((float) padsHeight()).reduced (12.0f, 0.0f);
    const float w = juce::jmin (130.0f, (strip.getWidth() - (float) (kPadGap * (perRow - 1))) / (float) perRow);
    const int row = pad / perRow, col = pad % perRow;
    return { strip.getX() + (float) col * (w + (float) kPadGap), strip.getY() + (float) row * (float) (kPadH + 6) + 3.0f, w, (float) kPadH };
}

int PlayerPage::slotAt (juce::Point<float> p) const
{
    for (int i = 0; i < numPadTiles(); ++i)
        if (padBounds (i).contains (p)) return kNumInstruments + i;
    for (const auto& s : spots)
        if (spotPath (s).contains (p)) return s.slot;
    return -1;
}

juce::String PlayerPage::describe (int slot) const
{
    auto& km = proc.getKitManager();
    const auto m = km.getSlot (slot);
    int files = 0;
    for (auto& l : m.layers) files += l.files.size();
    juce::String s = km.getSlotName (slot);
    if (files == 0) return s + " - no samples (right-click or drop files to load)";
    s << " - " << files << (files == 1 ? " sample" : " samples");
    if (m.layers.size() > 1) s << " in " << (int) m.layers.size() << " velocity layers";
    if (m.midiNote >= 0) s << " - MIDI " << m.midiNote;
    return s;
}

juce::String PlayerPage::getTooltip()
{
    return hover >= 0 ? describe (hover) + "\nClick: play  |  Right-click: samples" : juce::String();
}

void PlayerPage::timerCallback()
{
    if (! isVisible()) return;   // the hidden tab skips the work
    bool dirty = false;
    const auto& hm = proc.getHitMonitor();
    for (int i = 0; i < kNumKitSlots; ++i)
    {
        const auto c = hm.slots[(size_t) i].count.load (std::memory_order_acquire);
        if (c != seen[(size_t) i])
        {
            seen[(size_t) i] = c;
            glow[(size_t) i] = 0.35f + 0.65f * hm.slots[(size_t) i].velocity.load();
            dirty = true;
        }
        else if (glow[(size_t) i] > 0.0f)
        {
            glow[(size_t) i] = juce::jmax (0.0f, glow[(size_t) i] - 0.07f);
            dirty = true;
        }
    }
    const auto name = proc.getKitManager().getKitName();
    if (kitLabel.getText() != name) kitLabel.setText (name, juce::dontSendNotification);
    if (dirty) repaint();
}

void PlayerPage::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
    const auto ib = imageBounds();
    if (photo.isValid()) g.drawImage (photo, ib, juce::RectanglePlacement::stretchToFit);

    auto& km = proc.getKitManager();
    // drums: light up when played; outline on hover / drop
    for (int slot = 0; slot < kNumInstruments; ++slot)
    {
        const float gl = glow[(size_t) slot];
        const bool hot = slot == dropSlot;   // only while dropping files on it
        if (gl <= 0.0f && ! hot) continue;
        const auto path = slotPath (slot);
        if (path.isEmpty()) continue;
        if (gl > 0.0f)
        {
            for (int k = 3; k >= 1; --k)   // soft glow
            {
                g.setColour (colours::accent.withAlpha (0.12f * gl));
                g.strokePath (path, juce::PathStrokeType (6.0f * (float) k));
            }
            g.setColour (colours::accent.withAlpha (0.38f * gl));
            g.fillPath (path);
        }
        if (hot)
        {
            g.setColour (colours::trigger);
            g.strokePath (path, juce::PathStrokeType (2.0f));
            const auto b = path.getBounds();
            const auto label = km.getSlotName (slot);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            const auto lb = juce::Rectangle<float> (b.getCentreX() - 60.0f, b.getY() - 24.0f, 120.0f, 20.0f);
            g.setColour (colours::background.withAlpha (0.75f));
            g.fillRoundedRectangle (lb, 4.0f);
            g.setColour (colours::text);
            g.drawText (label, lb, juce::Justification::centred);
        }
    }

    // pads (extra kit sounds) under the picture
    for (int i = 0; i < numPadTiles(); ++i)
    {
        const int slot = kNumInstruments + i;
        const auto b = padBounds (i);
        const auto m = km.getSlot (slot);
        const bool empty = m.isEmpty() && m.name.isEmpty();
        const float gl = glow[(size_t) slot];
        g.setColour (colours::panelRaised.interpolatedWith (colours::accent, 0.55f * gl));
        g.fillRoundedRectangle (b, 8.0f);
        g.setColour (slot == dropSlot ? colours::trigger : colours::outline);
        if (empty)
        {
            juce::Path outline;
            outline.addRoundedRectangle (b.reduced (1.0f), 8.0f);
            juce::Path dashed;
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, outline, dashes, 2);
            g.fillPath (dashed);
            g.setColour (colours::textDim);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("+ Pad", b, juce::Justification::centred);
            continue;
        }
        g.drawRoundedRectangle (b.reduced (0.5f), 8.0f, 1.5f);
        g.setColour (m.isEmpty() ? colours::textDim : colours::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (km.getSlotName (slot), b.reduced (8.0f, 6.0f).withTrimmedBottom (16.0f).toNearestInt(), juce::Justification::centred, 2);
        g.setColour (colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (m.midiNote >= 0 ? "MIDI " + juce::String (m.midiNote) : juce::String(), b.reduced (6.0f).removeFromBottom (14.0f), juce::Justification::centred);
    }

    g.setColour (colours::textDim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Click a drum to play it  |  Right-click to load samples  |  Drop audio files or a folder on a drum  |  Drums light up when played",
                getLocalBounds().removeFromBottom (kFooterH), juce::Justification::centred);
}

void PlayerPage::mouseMove (const juce::MouseEvent& e)
{
    const int s = slotAt (e.position);
    if (s != hover) { hover = s; setMouseCursor (s >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor); }
}

void PlayerPage::mouseExit (const juce::MouseEvent&) { hover = -1; }

void PlayerPage::mouseDown (const juce::MouseEvent& e)
{
    const int slot = slotAt (e.position);
    if (slot < 0) return;
    if (e.mods.isPopupMenu()) { showMenu (slot); return; }
    const auto m = proc.getKitManager().getSlot (slot);
    if (m.isEmpty()) { chooseFiles (slot, false, false); return; }   // empty: load something first
    // louder towards the middle of the drum, softer at the edge (Shift: soft)
    float vel = 0.9f;
    if (! isPadSlot (slot))
    {
        const auto b = slotPath (slot).getBounds();
        const float d = b.getCentre().getDistanceFrom (e.position) / juce::jmax (1.0f, 0.5f * juce::jmin (b.getWidth(), b.getHeight()));
        vel = juce::jlimit (0.35f, 1.0f, 1.05f - 0.45f * d);
    }
    if (e.mods.isShiftDown()) vel *= 0.45f;
    proc.auditionSlot (slot, vel);
}

void PlayerPage::showMenu (int slot)
{
    auto& km = proc.getKitManager();
    const auto m = km.getSlot (slot);
    juce::PopupMenu menu;
    menu.addSectionHeader (km.getSlotName (slot));
    menu.addItem (1, "Load samples...");
    menu.addItem (2, "Add samples...", ! m.isEmpty());
    menu.addItem (3, "Load folder...");
    menu.addSeparator();
    menu.addItem (4, "Edit layers in SAMPLES...");
    if (! isPadSlot (slot)) menu.addItem (5, "Track this drum on REPLACE");
    else menu.addItem (6, "Rename pad...");
    menu.addItem (7, "Clear samples", ! m.isEmpty());
    juce::PopupMenu files;
    int id = 100;
    std::vector<juce::File> fileIds;
    for (auto& l : m.layers)
        for (auto& f : l.files) { files.addItem (id++, f.getFileNameWithoutExtension()); fileIds.push_back (f); }
    if (! fileIds.empty())
    {
        menu.addSeparator();
        menu.addSubMenu ("Samples (" + juce::String ((int) fileIds.size()) + ") - click to hear", files);
    }

    juce::Component::SafePointer<PlayerPage> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                        [safe, slot, fileIds] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& self = *safe;
        auto& kitm = self.proc.getKitManager();
        if (r == 1) self.chooseFiles (slot, false, false);
        else if (r == 2) self.chooseFiles (slot, true, false);
        else if (r == 3) self.chooseFiles (slot, false, true);
        else if (r == 4) { if (self.onEditSlot) self.onEditSlot (slot); }
        else if (r == 5)
        {
            if (auto* prm = self.proc.apvts.getParameter (ids::instrument)) prm->setValueNotifyingHost (prm->convertTo0to1 ((float) slot));
        }
        else if (r == 6)
        {
            auto* w = new juce::AlertWindow ("Rename pad", {}, juce::MessageBoxIconType::NoIcon, &self);
            w->addTextEditor ("name", kitm.getSlotName (slot));
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, slot, w] (int ok)
            {
                if (safe != nullptr && ok == 1)
                {
                    safe->proc.getKitManager().setSlotName (slot, w->getTextEditorContents ("name"));
                    safe->kitEdited();
                }
            }), true);
        }
        else if (r == 7) { kitm.clearSlot (slot); self.kitEdited(); }
        else if (r >= 100 && r - 100 < (int) fileIds.size()) self.proc.previewSample (fileIds[(size_t) (r - 100)]);
    });
}

void PlayerPage::chooseFiles (int slot, bool add, bool folder)
{
    juce::Component::SafePointer<PlayerPage> safe (this);
    const auto title = folder ? (slot < 0 ? juce::String ("Import kit folder") : "Load a folder for " + proc.getKitManager().getSlotName (slot))
                              : (add ? "Add samples to " : "Load samples for ") + proc.getKitManager().getSlotName (slot);
    chooser = std::make_unique<juce::FileChooser> (title, juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   folder ? juce::String() : juce::String ("*.wav;*.wave;*.aif;*.aiff;*.flac"));
    const int flags = juce::FileBrowserComponent::openMode
                    | (folder ? juce::FileBrowserComponent::canSelectDirectories
                              : juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems);
    chooser->launchAsync (flags, [safe, slot, add, folder] (const juce::FileChooser& fc)
    {
        if (safe == nullptr) return;
        const auto results = fc.getResults();
        if (results.isEmpty()) return;
        if (folder && slot < 0)
        {
            safe->proc.getKitManager().importFolder (results[0]);
            safe->kitEdited();
            return;
        }
        juce::Array<juce::File> files;
        for (auto& f : results)
        {
            if (f.isDirectory())
                for (const auto& entry : juce::RangedDirectoryIterator (f, true, "*.wav;*.wave;*.aif;*.aiff;*.flac")) files.add (entry.getFile());
            else if (KitManager::isAudioFile (f)) files.add (f);
        }
        safe->assign (slot, files, add);
    });
}

void PlayerPage::assign (int slot, const juce::Array<juce::File>& files, bool add)
{
    if (files.isEmpty() || slot < 0) return;
    auto& km = proc.getKitManager();
    juce::Array<juce::File> all;
    if (add) for (auto& l : km.getSlot (slot).layers) all.addArray (l.files);
    for (auto& f : files) all.addIfNotAlreadyThere (f);
    // a new pad is named after its folder / first file
    if (isPadSlot (slot) && km.getSlot (slot).name.isEmpty())
    {
        const auto& f = files.getFirst();
        km.setSlotName (slot, files.size() > 1 && f.getParentDirectory().getFileName().isNotEmpty() ? f.getParentDirectory().getFileName()
                                                                                                   : f.getFileNameWithoutExtension());
    }
    km.assignFiles (slot, all);
    kitEdited();
}

void PlayerPage::kitEdited()
{
    proc.rebuildKit();
    if (onKitChanged) onKitChanged();
    repaint();
}

void PlayerPage::fileDragMove (const juce::StringArray&, int x, int y)
{
    const int s = slotAt ({ (float) x, (float) y });
    if (s != dropSlot) { dropSlot = s; repaint(); }
}

void PlayerPage::filesDropped (const juce::StringArray& paths, int x, int y)
{
    dropSlot = -1;
    const int slot = slotAt ({ (float) x, (float) y });
    juce::Array<juce::File> files;
    for (auto& path : paths)
    {
        juce::File f (path);
        if (f.isDirectory())
        {
            if (slot < 0) { proc.getKitManager().importFolder (f); kitEdited(); return; }   // a whole kit
            for (const auto& entry : juce::RangedDirectoryIterator (f, true, "*.wav;*.wave;*.aif;*.aiff;*.flac")) files.add (entry.getFile());
        }
        else if (KitManager::isAudioFile (f)) files.add (f);
    }
    if (slot >= 0) assign (slot, files, false);
    repaint();
}

} // namespace thruma::ui
