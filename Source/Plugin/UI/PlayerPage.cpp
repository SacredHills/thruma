#include "PlayerPage.h"
#include "BinaryData.h"

namespace thruma::ui
{

// the room picture is 16:9; the kit sits in it where the artwork places it (measured on 1920 x 1080)
static constexpr float kRoomAspect = 1920.0f / 1080.0f;
static constexpr float kKitX = 348.0f / 1920.0f, kKitY = 408.0f / 1080.0f, kKitW = 1194.0f / 1920.0f, kKitH = 672.0f / 1080.0f;
static constexpr int kPadH = 54, kPadMinW = 100, kPadGap = 8, kMaxPadRows = 4, kBottomMargin = 10;

namespace
{
    // the area each drum takes in the kit picture (1672 x 941 pixels); only its own visible pixels light up.
    // Front-most first: a pixel belongs to the first drum whose area contains it.
    bool inEllipse (float x, float y, float cx, float cy, float rx, float ry) { const float dx = (x - cx) / rx, dy = (y - cy) / ry; return dx * dx + dy * dy <= 1.0f; }
    bool inRect (float x, float y, float x1, float y1, float x2, float y2) { return x >= x1 && x <= x2 && y >= y1 && y <= y2; }
    bool inShell (float x, float y, float x1, float x2, float top, float topR, float bottom, float bottomR)   // a drum seen from the front
    {
        const float cx = 0.5f * (x1 + x2), rx = 0.5f * (x2 - x1);
        return inEllipse (x, y, cx, top, rx, topR) || inRect (x, y, x1, top, x2, bottom) || inEllipse (x, y, cx, bottom, rx, bottomR);
    }
    int drumAt (float x, float y)
    {
        using I = Instrument;
        if (inShell (x, y, 381, 688, 428, 24, 570, 20))   return (int) I::Snare;
        if (inShell (x, y, 576, 797, 268, 26, 422, 20))   return (int) I::Tom1;
        if (inShell (x, y, 820, 1042, 263, 26, 422, 20))  return (int) I::Tom2;
        if (inShell (x, y, 988, 1260, 448, 28, 735, 24))  return (int) I::Tom3;
        if (inShell (x, y, 1240, 1558, 470, 30, 772, 26)) return (int) I::Tom4;
        if (inEllipse (x, y, 810, 615, 195, 188) || inRect (x, y, 755, 712, 865, 900)) return (int) I::Kick;
        if (inEllipse (x, y, 240, 292, 172, 50)) return (int) I::HiHat;   // cymbals (the pedal is clickable, never lit)
        if (inEllipse (x, y, 559, 207, 102, 28))   return (int) I::Splash;
        if (inEllipse (x, y, 447, 133, 182, 52))   return (int) I::Crash;
        if (inEllipse (x, y, 1097, 133, 172, 54))  return (int) I::Crash2;
        if (inEllipse (x, y, 1388, 113, 142, 50))  return (int) I::China;
        if (inEllipse (x, y, 1420, 268, 228, 84))  return (int) I::Ride;
        return -1;
    }
}

PlayerPage::PlayerPage (ThrumaProcessor& p) : proc (p)
{
    room = juce::ImageCache::getFromMemory (BinaryData::room_jpg, BinaryData::room_jpgSize);
    drums = juce::ImageCache::getFromMemory (BinaryData::drums_png, BinaryData::drums_pngSize);
    buildMasks();

    importKitBtn.setTooltip ("Load a whole kit from a folder: drums by name, every other sound onto pads.");
    importKitBtn.onClick = [this] { chooseFiles (-1, false, true); };
    loadKitBtn.setTooltip ("Load a saved Thruma kit (.thrumakit).");
    loadKitBtn.onClick = [this]
    {
        juce::Component::SafePointer<PlayerPage> safe (this);
        chooser = std::make_unique<juce::FileChooser> ("Load kit", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.thrumakit;*.json");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            const auto file = fc.getResult();
            safe->proc.runKitJob ([file] (KitManager& km) { km.loadKitFile (file); }, [safe] { if (safe != nullptr) safe->kitLoaded(); });
        });
    };
    kitLabel.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    kitLabel.setColour (juce::Label::textColourId, colours::text);
    kitLabel.setTooltip ("Click a drum to play it | Right-click to load samples | Drop audio files or a folder on a drum | Drums light up when hit");
    for (auto* c : { (juce::Component*) &importKitBtn, (juce::Component*) &loadKitBtn, (juce::Component*) &kitLabel }) addAndMakeVisible (c);

    for (int i = 0; i < kNumKitSlots; ++i) seen[(size_t) i] = proc.getHitMonitor().slots[(size_t) i].count.load();
    startTimerHz (40);
}

void PlayerPage::resized()
{
    auto top = getLocalBounds().removeFromTop (46).reduced (14, 8);   // over the picture
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
    // the room fills the whole page (cropping what does not fit); anchored at the floor so the kit stays in view
    const auto area = getLocalBounds().toFloat();
    const float scale = juce::jmax (area.getWidth() / kRoomAspect, area.getHeight());   // = picture height
    const float w = scale * kRoomAspect, h = scale;
    return { area.getCentreX() - 0.5f * w, area.getBottom() - h, w, h };
}

juce::Rectangle<float> PlayerPage::kitBounds() const
{
    const auto ib = imageBounds();
    return { ib.getX() + kKitX * ib.getWidth(), ib.getY() + kKitY * ib.getHeight(), kKitW * ib.getWidth(), kKitH * ib.getHeight() };
}

juce::Rectangle<float> PlayerPage::slotBounds (int slot) const
{
    if (slot < 0 || slot >= kNumInstruments || kitW == 0) return {};
    const auto kb = kitBounds();
    const float k = kb.getWidth() / (float) kitW;
    const auto a = masks[(size_t) slot].area.toFloat();
    return { kb.getX() + a.getX() * k, kb.getY() + a.getY() * k, a.getWidth() * k, a.getHeight() * k };
}

void PlayerPage::buildMasks()
{
    if (! drums.isValid()) return;
    kitW = drums.getWidth();
    kitH = drums.getHeight();
    owner.assign ((size_t) (kitW * kitH), kNone);
    std::array<juce::Rectangle<int>, kNumInstruments> box {};
    {
        const juce::Image::BitmapData px (drums, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < kitH; ++y)
            for (int x = 0; x < kitW; ++x)
            {
                if (px.getPixelColour (x, y).getAlpha() <= 40) continue;
                const int d = drumAt ((float) x, (float) y);
                if (d < 0) continue;
                owner[(size_t) (y * kitW + x)] = (uint8_t) d;
                auto& b = box[(size_t) d];
                b = b.isEmpty() ? juce::Rectangle<int> (x, y, 1, 1) : b.getUnion ({ x, y, 1, 1 });
            }
    }
    const juce::Image::BitmapData src (drums, juce::Image::BitmapData::readOnly);
    for (int d = 0; d < kNumInstruments; ++d)
    {
        auto& m = masks[(size_t) d];
        m.area = box[(size_t) d];
        if (m.area.isEmpty()) continue;
        // the drum's own pixels in the highlight colour
        m.lit = juce::Image (juce::Image::ARGB, m.area.getWidth(), m.area.getHeight(), true);
        {
            juce::Image::BitmapData dst (m.lit, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < m.area.getHeight(); ++y)
                for (int x = 0; x < m.area.getWidth(); ++x)
                {
                    const int sx = m.area.getX() + x, sy = m.area.getY() + y;
                    if (owner[(size_t) (sy * kitW + sx)] != d) continue;
                    dst.setPixelColour (x, y, juce::Colours::white.withAlpha (src.getPixelColour (sx, sy).getFloatAlpha()));
                }
        }
        // a soft glow around it: blurred at quarter size (cheap), drawn scaled up
        const int pad = 24;
        m.glowArea = m.area.expanded (pad);
        juce::Image small (juce::Image::ARGB, juce::jmax (1, m.glowArea.getWidth() / 4), juce::jmax (1, m.glowArea.getHeight() / 4), true);
        {
            juce::Graphics g (small);
            g.drawImage (m.lit, juce::Rectangle<float> ((float) pad / 4.0f, (float) pad / 4.0f, (float) m.area.getWidth() / 4.0f, (float) m.area.getHeight() / 4.0f));
        }
        m.glow = juce::Image (juce::Image::ARGB, small.getWidth(), small.getHeight(), true);
        juce::ImageConvolutionKernel blur (9);
        blur.createGaussianBlur (3.0f);
        blur.applyToImage (m.glow, small, small.getBounds());
    }
}

juce::Rectangle<float> PlayerPage::padBounds (int pad) const
{
    const int perRow = padsPerRow();
    auto strip = getLocalBounds().toFloat().withTrimmedBottom ((float) kBottomMargin).removeFromBottom ((float) padsHeight()).reduced (14.0f, 0.0f);
    const float w = juce::jmin (130.0f, (strip.getWidth() - (float) (kPadGap * (perRow - 1))) / (float) perRow);
    const int row = pad / perRow, col = pad % perRow;
    return { strip.getX() + (float) col * (w + (float) kPadGap), strip.getY() + (float) row * (float) (kPadH + 6) + 3.0f, w, (float) kPadH };
}

int PlayerPage::slotAt (juce::Point<float> p) const
{
    for (int i = 0; i < numPadTiles(); ++i)
        if (padBounds (i).contains (p)) return kNumInstruments + i;
    if (kitW == 0) return -1;
    const auto kb = kitBounds();
    const float k = (float) kitW / kb.getWidth();
    const int x0 = (int) ((p.x - kb.getX()) * k), y0 = (int) ((p.y - kb.getY()) * k);
    // the drum under the pointer, or the nearest one within a few pixels (thin cymbal edges)
    for (int r = 0; r <= 8; r += 4)
        for (int dy = -r; dy <= r; dy += 4)
            for (int dx = -r; dx <= r; dx += 4)
            {
                const int x = x0 + dx, y = y0 + dy;
                if (x < 0 || y < 0 || x >= kitW || y >= kitH) continue;
                if (const auto o = owner[(size_t) (y * kitW + x)]; o != kNone) return (int) o;
            }
    if (inRect ((float) x0, (float) y0, 170, 770, 325, 900)) return (int) Instrument::HiHat;   // hi-hat pedal
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
    g.fillAll (juce::Colours::black);
    const auto ib = imageBounds();
    if (room.isValid()) g.drawImage (room, ib, juce::RectanglePlacement::stretchToFit);
    const auto kb = kitBounds();
    if (drums.isValid()) g.drawImage (drums, kb, juce::RectanglePlacement::stretchToFit);

    auto& km = proc.getKitManager();
    // drums light up (their own pixels, plus a soft glow) when hit; amber while dropping files on one
    const float k = kitW > 0 ? kb.getWidth() / (float) kitW : 1.0f;
    auto toScreen = [&] (juce::Rectangle<int> r)
    {
        return juce::Rectangle<float> (kb.getX() + (float) r.getX() * k, kb.getY() + (float) r.getY() * k, (float) r.getWidth() * k, (float) r.getHeight() * k);
    };
    for (int slot = 0; slot < kNumInstruments; ++slot)
    {
        const auto& m = masks[(size_t) slot];
        if (! m.lit.isValid()) continue;
        const float gl = glow[(size_t) slot];
        if (gl > 0.0f)
        {
            g.setOpacity (0.45f * gl);
            g.drawImage (m.glow, toScreen (m.glowArea));
            g.setOpacity (0.275f * gl);
            g.drawImage (m.lit, toScreen (m.area));
        }
        if (slot == dropSlot)
        {
            g.setOpacity (0.35f);
            g.drawImage (m.lit, toScreen (m.area));
        }
        g.setOpacity (1.0f);
    }

    // a soft shade at the top so the kit name and buttons read over the picture
    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.65f), 0.0f, 0.0f, juce::Colours::transparentBlack, 0.0f, 70.0f, false));
    g.fillRect (getLocalBounds().removeFromTop (70));

    // pads (extra kit sounds) over the bottom of the picture
    for (int i = 0; i < numPadTiles(); ++i)
    {
        const int slot = kNumInstruments + i;
        const auto b = padBounds (i);
        const auto m = km.getSlot (slot);
        const bool empty = m.isEmpty() && m.name.isEmpty();
        const float gl = glow[(size_t) slot];
        g.setColour (juce::Colours::black.withAlpha (empty ? 0.35f : 0.6f).interpolatedWith (juce::Colours::white.withAlpha (0.85f), 0.3f * gl));
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
        const auto b = slotBounds (slot);
        const float d = b.getCentre().getDistanceFrom (e.position) / juce::jmax (1.0f, 0.5f * juce::jmin (b.getWidth(), b.getHeight()));
        vel = juce::jlimit (0.35f, 1.0f, 1.05f - 0.45f * d);
    }
    if (e.mods.isShiftDown()) vel *= 0.45f;
    proc.auditionSlot (slot, vel);
}

void PlayerPage::showMenu (int slot)
{
    std::vector<juce::File> fileIds;
    auto menu = buildMenu (slot, fileIds);
    showBuiltMenu (menu, slot, fileIds);
}

juce::PopupMenu PlayerPage::buildMenu (int slot, std::vector<juce::File>& fileIds)
{
    auto& km = proc.getKitManager();
    const auto m = km.getSlot (slot);
    juce::PopupMenu menu;
    menu.addSectionHeader (km.getSlotName (slot));
    menu.addItem (1, "Load samples...");
    menu.addItem (2, "Add samples...", ! m.isEmpty());
    menu.addItem (3, "Load folder...");
    menu.addItem (8, "Load TDE...");
    menu.addItem (9, "Save as TDE...", ! m.isEmpty());
    menu.addSeparator();
    {
        // MIDI note: General MIDI drum notes, the current one ticked; one used by another drum or pad is swapped
        juce::PopupMenu notes;
        for (int n = 27; n <= 87; ++n)
        {
            juce::String label = juce::MidiMessage::getMidiNoteName (n, true, true, 3) + "  (" + juce::String (n) + ")";
            if (const char* gm = juce::MidiMessage::getRhythmInstrumentName (n)) label << "  " << gm;   // only 35 - 81 have names (else nullptr)
            for (int other = 0; other < kNumKitSlots; ++other)
                if (other != slot && km.getSlot (other).midiNote == n && (other < kNumInstruments || ! km.getSlot (other).isEmpty()))
                    label << "  - now " << km.getSlotName (other);
            notes.addItem (1000 + n, label, true, m.midiNote == n);
        }
        notes.addSeparator();
        notes.addItem (999, "Other note...");
        menu.addSubMenu ("MIDI note: " + (m.midiNote >= 0 ? juce::MidiMessage::getMidiNoteName (m.midiNote, true, true, 3) + " (" + juce::String (m.midiNote) + ")" : juce::String ("none")), notes);
    }
    menu.addItem (4, "Edit layers in SAMPLES...");
    if (! isPadSlot (slot)) menu.addItem (5, "Track this drum on REPLACE");
    else menu.addItem (6, "Rename pad...");
    if (isPadSlot (slot)) menu.addItem (7, "Remove pad");
    else menu.addItem (7, "Clear samples", ! m.isEmpty());
    juce::PopupMenu files;
    int id = 100;
    fileIds.clear();
    for (auto& l : m.layers)
        for (auto& f : l.files) { files.addItem (id++, f.getFileNameWithoutExtension()); fileIds.push_back (f); }
    if (! fileIds.empty())
    {
        menu.addSeparator();
        menu.addSubMenu ("Samples (" + juce::String ((int) fileIds.size()) + ") - click to hear", files);
    }

    return menu;
}

void PlayerPage::showBuiltMenu (juce::PopupMenu& menu, int slot, std::vector<juce::File> fileIds)
{
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
        else if (r == 8) self.chooseTde (slot, false);
        else if (r == 9) self.chooseTde (slot, true);
        else if (r >= 1000 && r <= 1127) self.setMidiNote (slot, r - 1000);
        else if (r == 999)
        {
            auto* w = new juce::AlertWindow ("MIDI note for " + kitm.getSlotName (slot), "Note number (0 - 127)", juce::MessageBoxIconType::NoIcon, &self);
            w->addTextEditor ("note", juce::String (juce::jmax (0, kitm.getSlot (slot).midiNote)));
            w->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            w->enterModalState (true, juce::ModalCallbackFunction::create ([safe, slot, w] (int ok)
            {
                if (safe != nullptr && ok == 1) safe->setMidiNote (slot, juce::jlimit (0, 127, w->getTextEditorContents ("note").getIntValue()));
            }), true);
        }
        else if (r >= 100 && r - 100 < (int) fileIds.size()) self.proc.previewSample (fileIds[(size_t) (r - 100)]);
    });
}

void PlayerPage::chooseTde (int slot, bool save)
{
    auto& km = proc.getKitManager();
    juce::Component::SafePointer<PlayerPage> safe (this);
    const auto base = KitManager::getTdeLibraryFolder().getParentDirectory();
    if (save)
    {
        chooser = std::make_unique<juce::FileChooser> ("Save " + km.getSlotName (slot) + " as TDE", base.getChildFile (km.getSlotName (slot) + ".tde"), "*.tde");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe, slot] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            juce::String err;
            const auto f = fc.getResult().withFileExtension ("tde");
            f.getParentDirectory().createDirectory();
            if (! safe->proc.getKitManager().saveSlotTde (slot, f, err))
                juce::AlertWindow::showAsync (juce::MessageBoxOptions().withTitle ("Save TDE").withMessage (err).withButton ("OK"), nullptr);
        });
        return;
    }
    chooser = std::make_unique<juce::FileChooser> ("Load TDE for " + km.getSlotName (slot), base, "*.tde");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, slot] (const juce::FileChooser& fc)
    {
        if (safe != nullptr && fc.getResult() != juce::File()) safe->loadTde (slot, fc.getResult());
    });
}

void PlayerPage::loadTde (int slot, const juce::File& f)
{
    juce::Component::SafePointer<PlayerPage> safe (this);
    proc.runKitJob ([slot, f] (KitManager& km) { juce::String err; km.loadSlotTde (slot, f, err); }, [safe] { if (safe != nullptr) safe->kitLoaded(); });
}

void PlayerPage::setMidiNote (int slot, int note)
{
    auto& km = proc.getKitManager();
    auto mine = km.getSlot (slot);
    const int old = mine.midiNote;
    // a drum or pad already on that note takes this one's old note, so no two play from the same key
    for (int other = 0; other < kNumKitSlots; ++other)
        if (other != slot && km.getSlot (other).midiNote == note)
        {
            auto o = km.getSlot (other);
            o.midiNote = old;
            km.setSlot (other, o);
        }
    mine.midiNote = note;
    km.setSlot (slot, mine);
    kitEdited();
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
            const auto folder = results[0];
            safe->proc.runKitJob ([folder] (KitManager& km) { km.importFolder (folder); }, [safe] { if (safe != nullptr) safe->kitLoaded(); });
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
    juce::Component::SafePointer<PlayerPage> safe (this);
    proc.runKitJob ([slot, all] (KitManager& k) { k.setProgressTotal (all.size()); k.assignFiles (slot, all); },
                    [safe] { if (safe != nullptr) safe->kitLoaded(); });
}

void PlayerPage::kitEdited()
{
    juce::Component::SafePointer<PlayerPage> safe (this);
    proc.runKitJob ({}, [safe] { if (safe != nullptr) safe->kitLoaded(); });
    kitLoaded();   // the model changed already
}

void PlayerPage::kitLoaded()
{
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
            if (slot < 0)   // a whole kit
            {
                juce::Component::SafePointer<PlayerPage> safe (this);
                proc.runKitJob ([f] (KitManager& km) { km.importFolder (f); }, [safe] { if (safe != nullptr) safe->kitLoaded(); });
                return;
            }
            for (const auto& entry : juce::RangedDirectoryIterator (f, true, "*.wav;*.wave;*.aif;*.aiff;*.flac")) files.add (entry.getFile());
        }
        else if (KitManager::isTdeFile (f) && slot >= 0) { loadTde (slot, f); return; }
        else if (KitManager::isAudioFile (f)) files.add (f);
    }
    if (slot >= 0) assign (slot, files, false);
    repaint();
}

} // namespace thruma::ui
