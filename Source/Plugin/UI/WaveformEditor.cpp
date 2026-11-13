#include "WaveformEditor.h"

namespace thruma::ui
{

namespace
{
    constexpr int kRulerH = 20;
    constexpr int kScrollH = 12;

    juce::String formatTime (double seconds)
    {
        const int m = (int) (seconds / 60.0);
        const double s = seconds - m * 60.0;
        return juce::String (m) + ":" + juce::String (s, 2).paddedLeft ('0', 5);
    }
}

WaveformEditor::WaveformEditor (ThrumaProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    hScroll.setAutoHide (false);
    hScroll.addListener (this);
    addAndMakeVisible (hScroll);
    startTimerHz (30);
}

WaveformEditor::~WaveformEditor()
{
    hScroll.removeListener (this);
}

juce::Rectangle<int> WaveformEditor::waveArea() const
{
    return getLocalBounds().withTrimmedTop (kRulerH).withTrimmedBottom (kScrollH);
}

int64_t WaveformEditor::dataOrigin() const
{
    const auto& c = proc.getCapture();
    if (c.length.load() > 0) return c.origin.load();
    return proc.getStoredOverviewOrigin();
}

int64_t WaveformEditor::dataLength() const
{
    const auto& c = proc.getCapture();
    if (const auto l = c.length.load(); l > 0) return l;
    return (int64_t) (proc.getStoredOverview().size() / 2) * kBin;
}

void WaveformEditor::updatePeakCache()
{
    const auto& c = proc.getCapture();
    const auto len = c.length.load (std::memory_order_acquire);
    if (len <= 0)
    {
        if (peakCacheLen != 0) { peakCache.clear(); peakCacheLen = 0; }
        dataPeak = 0.0f;
        for (auto v : proc.getStoredOverview()) dataPeak = std::max (dataPeak, std::abs (v));
        return;
    }
    if (len < peakCacheLen) { peakCache.clear(); peakCacheLen = 0; dataPeak = 0.0f; }

    // recompute from the last (possibly partial) bin onwards
    int64_t startBin = peakCacheLen / kBin;
    peakCache.resize ((size_t) startBin * 2);
    const auto& d = c.data[0];
    for (int64_t b = startBin; b * kBin < len; ++b)
    {
        float lo = 0.0f, hi = 0.0f;
        const int64_t e = std::min (len, (b + 1) * kBin);
        for (int64_t i = b * kBin; i < e; ++i) { lo = std::min (lo, d[(size_t) i]); hi = std::max (hi, d[(size_t) i]); }
        peakCache.push_back (lo);
        peakCache.push_back (hi);
        dataPeak = std::max ({ dataPeak, -lo, hi });
    }
    peakCacheLen = len;
}

bool WaveformEditor::minMax (int64_t from, int64_t to, float& lo, float& hi) const
{
    const auto origin = dataOrigin();
    from -= origin; to -= origin;
    const auto len = dataLength();
    from = std::max<int64_t> (0, from);
    to = std::min (len, to);
    if (to <= from) return false;
    lo = hi = 0.0f;

    const auto& c = proc.getCapture();
    if (c.length.load() > 0 && (to - from) < kBin * 2)
    {
        const auto& d = c.data[0];
        for (int64_t i = from; i < to; ++i) { lo = std::min (lo, d[(size_t) i]); hi = std::max (hi, d[(size_t) i]); }
        return true;
    }
    const auto& pc = c.length.load() > 0 ? peakCache : proc.getStoredOverview();
    const int64_t nb = (int64_t) pc.size() / 2;
    for (int64_t b = from / kBin; b <= (to - 1) / kBin && b < nb; ++b)
    {
        lo = std::min (lo, pc[(size_t) b * 2]);
        hi = std::max (hi, pc[(size_t) b * 2 + 1]);
    }
    return true;
}

const std::vector<Trigger>& WaveformEditor::displayed() const
{
    return editing ? working : proc.getTriggers().getAll();
}

double WaveformEditor::xToPos (float x) const { return viewStart + (double) x * spp; }
float WaveformEditor::posToX (double pos) const { return (float) ((pos - viewStart) / spp); }

int WaveformEditor::hitTest (juce::Point<float> p) const
{
    const auto& t = displayed();
    int best = -1;
    float bestD = 6.0f;
    const auto lo = (int64_t) xToPos (p.x - 8.0f), hi = (int64_t) xToPos (p.x + 8.0f);
    auto it = std::lower_bound (t.begin(), t.end(), lo, [] (const Trigger& a, int64_t v) { return a.position < v; });
    for (; it != t.end() && it->position <= hi; ++it)
    {
        const float d = std::abs (posToX ((double) it->position) - p.x);
        if (d < bestD) { bestD = d; best = (int) (it - t.begin()); }
    }
    return best;
}

void WaveformEditor::zoomToFit()
{
    const auto len = dataLength();
    const int w = juce::jmax (1, getWidth());
    if (len > 0)
    {
        viewStart = (double) dataOrigin();
        spp = juce::jmax (1.0, (double) len / w);
    }
    else if (! proc.getTriggers().getAll().empty())
    {
        const auto& t = proc.getTriggers().getAll();
        const double a = (double) t.front().position, b = (double) t.back().position;
        spp = juce::jmax (1.0, (b - a + proc.getCurrentSampleRate()) / w);
        viewStart = a - 0.5 * proc.getCurrentSampleRate();
    }
    else
    {
        viewStart = 0.0;
        spp = juce::jmax (1.0, proc.getCurrentSampleRate() * 10.0 / w);
    }
    hasFitted = len > 0;
    updateScrollBar();
    repaint();
}

void WaveformEditor::zoomBy (double factor, float anchorX)
{
    const double anchor = xToPos (anchorX);
    spp = juce::jlimit (0.05, 1.0e6, spp / factor);
    viewStart = anchor - anchorX * spp;
    updateScrollBar();
    repaint();
}

void WaveformEditor::updateScrollBar()
{
    const double origin = (double) dataOrigin();
    const double len = (double) juce::jmax<int64_t> (dataLength(), (int64_t) (getWidth() * spp));
    const double visible = getWidth() * spp;
    hScroll.setRangeLimits (std::min (origin, viewStart), std::max (origin + len, viewStart + visible), juce::dontSendNotification);
    hScroll.setCurrentRange (viewStart, visible, juce::dontSendNotification);
}

void WaveformEditor::scrollBarMoved (juce::ScrollBar*, double newStart)
{
    viewStart = newStart;
    repaint();
}

void WaveformEditor::resized()
{
    hScroll.setBounds (getLocalBounds().removeFromBottom (kScrollH));
    updateScrollBar();
}

void WaveformEditor::timerCallback()
{
    const auto prevLen = peakCacheLen;
    updatePeakCache();
    if (! hasFitted && dataLength() > 0) zoomToFit();

    bool dirty = prevLen != peakCacheLen || proc.isTransportPlaying();
    if (proc.getTriggers().size() != lastTriggerCount)
    {
        lastTriggerCount = proc.getTriggers().size();
        // drop selections of triggers that no longer exist (undo/redo, analyze)
        for (auto it = selected.begin(); it != selected.end();)
            it = proc.getTriggers().find (*it) == nullptr ? selected.erase (it) : std::next (it);
        dirty = true;
    }

    // follow the playhead while capturing
    if (proc.isCaptureArmed() && proc.isTransportPlaying())
    {
        const double ph = (double) proc.getPlayheadPosition();
        const double visible = getWidth() * spp;
        if (ph > viewStart + visible * 0.9 || ph < viewStart)
            viewStart = ph - visible * 0.1;
        updateScrollBar();
    }
    if (dirty) repaint();
}

void WaveformEditor::paint (juce::Graphics& g)
{
    const auto area = waveArea();
    g.setColour (colours::panel);
    g.fillRect (getLocalBounds());

    // --- ruler ---
    const double sr = juce::jmax (1.0, proc.getCurrentSampleRate());
    {
        auto ruler = getLocalBounds().removeFromTop (kRulerH);
        g.setColour (colours::panelRaised);
        g.fillRect (ruler);
        const double visibleSec = getWidth() * spp / sr;
        const double steps[] = { 0.001, 0.005, 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60 };
        double step = 60.0;
        for (auto s : steps) if (visibleSec / s <= 12.0) { step = s; break; }
        g.setFont (juce::FontOptions (11.0f));
        const double t0 = std::floor (viewStart / sr / step) * step;
        for (double t = t0; t * sr < viewStart + getWidth() * spp; t += step)
        {
            const float x = posToX (t * sr);
            g.setColour (colours::outline);
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
            g.setColour (colours::textDim);
            g.drawText (formatTime (t), (int) x + 3, 2, 80, kRulerH - 4, juce::Justification::centredLeft);
        }
    }

    // --- waveform ---
    const float mid = (float) area.getCentreY();
    const float halfH = area.getHeight() * 0.45f;
    const float scale = dataPeak > 1.0e-6f ? 1.0f / dataPeak : 1.0f;
    if (dataLength() > 0)
    {
        g.setColour (colours::accent.withAlpha (0.55f));
        for (int x = 0; x < getWidth(); ++x)
        {
            float lo, hi;
            const auto a = (int64_t) xToPos ((float) x), b = (int64_t) xToPos ((float) x + 1.0f);
            if (! minMax (a, std::max (a + 1, b), lo, hi)) continue;
            const float y0 = mid - hi * scale * halfH, y1 = mid - lo * scale * halfH;
            g.drawVerticalLine (x, y0, std::max (y1, y0 + 1.0f));
        }
        g.setColour (colours::outline);
        g.drawHorizontalLine ((int) mid, 0.0f, (float) getWidth());
    }
    else
    {
        g.setColour (colours::textDim);
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText ("No audio captured yet.\nArm CAPTURE and play your track, then press ANALYZE.\n"
                          "Double-click to add a trigger, drag to move, Ctrl/Cmd-drag for velocity, Delete to remove.",
                          area.reduced (20), juce::Justification::centred, 4);
    }

    // --- triggers ---
    const auto& t = displayed();
    const double v0 = xToPos (-10.0f), v1 = xToPos ((float) getWidth() + 10.0f);
    auto it = std::lower_bound (t.begin(), t.end(), (int64_t) v0, [] (const Trigger& a, int64_t v) { return a.position < v; });
    const float laneTop = (float) area.getY() + 6.0f, laneH = (float) area.getHeight() - 12.0f;
    for (int idx = (int) (it - t.begin()); it != t.end() && (double) it->position <= v1; ++it, ++idx)
    {
        const float x = posToX ((double) it->position);
        const bool sel = selected.count (it->id) > 0;
        const auto col = sel ? colours::selection : colours::trigger.withAlpha (it->ghost ? 0.55f : 0.95f);
        g.setColour (col.withAlpha (sel ? 0.9f : 0.5f));
        g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
        const float hy = laneTop + laneH * (1.0f - it->velocity);
        g.setColour (col);
        g.fillRoundedRectangle (x - 4.0f, hy - 4.0f, 8.0f, 8.0f, 2.0f);
        if (idx == hoverIndex)
        {
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("vel " + juce::String (velocityToMidi (it->velocity)), (int) x + 6, (int) hy - 8, 60, 16, juce::Justification::centredLeft);
        }
    }

    // --- rubber band, playhead ---
    if (drag == Drag::Rubber)
    {
        g.setColour (colours::accent.withAlpha (0.15f));
        g.fillRect (rubber);
        g.setColour (colours::accent.withAlpha (0.6f));
        g.drawRect (rubber, 1.0f);
    }
    if (proc.isTransportPlaying())
    {
        const float x = posToX ((double) proc.getPlayheadPosition());
        g.setColour (proc.isCaptureArmed() ? colours::record : colours::text);
        g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
    }
    g.setColour (colours::outline);
    g.drawRect (getLocalBounds());
}

float WaveformEditor::estimateVelocity (int64_t pos) const
{
    float lo = 0.0f, hi = 0.0f;
    const auto w = (int64_t) (0.005 * proc.getCurrentSampleRate());
    if (! minMax (pos, pos + w, lo, hi) || dataPeak <= 0.0f) return 0.8f;
    const float db = juce::Decibels::gainToDecibels (std::max (-lo, hi) / dataPeak, -100.0f);
    return juce::jlimit (0.05f, 1.0f, (db + 40.0f) / 40.0f);
}

void WaveformEditor::mouseMove (const juce::MouseEvent& e)
{
    const int h = hitTest (e.position);
    if (h != hoverIndex) { hoverIndex = h; repaint(); }
    setMouseCursor (h >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void WaveformEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const int idx = hitTest (e.position);
    if (e.mods.isPopupMenu())
    {
        if (idx >= 0) showContextMenu (idx);
        return;
    }
    if (idx >= 0)
    {
        const auto tr = displayed()[(size_t) idx];
        if (e.mods.isShiftDown())
        {
            if (! selected.erase (tr.id)) selected.insert (tr.id);
        }
        else if (selected.count (tr.id) == 0)
        {
            selected = { tr.id };
        }
        if (e.mods.isAltDown()) proc.auditionReplacement (tr.velocity);
        else proc.auditionCapture (tr.position);

        working = proc.getTriggers().getAll();
        dragOriginal = working;
        editing = true;
        dragChanged = false;
        drag = e.mods.isCommandDown() ? Drag::Velocity : Drag::Move;
        dragStartPos = xToPos (e.position.x);
        dragStartY = e.position.y;
    }
    else
    {
        if (! e.mods.isShiftDown()) selected.clear();
        drag = Drag::Rubber;
        rubber = { e.position, e.position };
    }
    if (onEdited) onEdited();
    repaint();
}

void WaveformEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::Move || drag == Drag::Velocity)
    {
        const auto delta = (int64_t) std::llround (xToPos (e.position.x) - dragStartPos);
        const float dv = -(e.position.y - dragStartY) / juce::jmax (1.0f, (float) waveArea().getHeight());
        working = dragOriginal;
        for (auto& tr : working)
        {
            if (selected.count (tr.id) == 0) continue;
            if (drag == Drag::Move) tr.position = std::max<int64_t> (0, tr.position + delta);
            else tr.velocity = juce::jlimit (0.01f, 1.0f, tr.velocity + dv);
        }
        std::stable_sort (working.begin(), working.end(), [] (const Trigger& a, const Trigger& b) { return a.position < b.position; });
        dragChanged = (drag == Drag::Move && delta != 0) || (drag == Drag::Velocity && std::abs (dv) > 0.001f);
        repaint();
    }
    else if (drag == Drag::Rubber)
    {
        rubber = juce::Rectangle<float> (e.mouseDownPosition, e.position);
        const auto a = (int64_t) xToPos (rubber.getX()), b = (int64_t) xToPos (rubber.getRight());
        if (! e.mods.isShiftDown()) selected.clear();
        for (auto& tr : proc.getTriggers().getAll())
            if (tr.position >= a && tr.position <= b) selected.insert (tr.id);
        repaint();
    }
}

void WaveformEditor::mouseUp (const juce::MouseEvent&)
{
    if ((drag == Drag::Move || drag == Drag::Velocity) && dragChanged)
        proc.commitTriggerEdit (working, drag == Drag::Move ? "Move triggers" : "Change velocity");
    editing = false;
    drag = Drag::None;
    if (onEdited) onEdited();
    repaint();
}

void WaveformEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int idx = hitTest (e.position);
    if (idx >= 0) { proc.auditionReplacement (displayed()[(size_t) idx].velocity); return; }
    if (! waveArea().contains (e.getPosition())) return;

    const auto pos = (int64_t) std::max (0.0, xToPos (e.position.x));
    TriggerList copy;
    copy.setAll (proc.getTriggers().getAll());
    const auto id = copy.add (pos, estimateVelocity (pos));
    proc.commitTriggerEdit (copy.getAll(), "Add trigger");
    selected = { id };
    if (onEdited) onEdited();
}

void WaveformEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
    {
        const float d = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        viewStart -= d * getWidth() * spp * 0.5;
        updateScrollBar();
        repaint();
    }
    else
    {
        zoomBy (std::exp (w.deltaY * 1.5), e.position.x);
    }
}

void WaveformEditor::deleteSelection()
{
    if (selected.empty()) return;
    std::vector<Trigger> keep;
    for (auto& t : proc.getTriggers().getAll())
        if (selected.count (t.id) == 0) keep.push_back (t);
    selected.clear();
    proc.commitTriggerEdit (std::move (keep), "Delete triggers");
    if (onEdited) onEdited();
}

bool WaveformEditor::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
    if (mods.isCommandDown() && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z'))
    {
        if (mods.isShiftDown()) proc.getUndoManager().redo(); else proc.getUndoManager().undo();
        if (onEdited) onEdited();
        return true;
    }
    if (mods.isCommandDown() && (k.getKeyCode() == 'Y' || k.getKeyCode() == 'y')) { proc.getUndoManager().redo(); if (onEdited) onEdited(); return true; }
    if (mods.isCommandDown() && (k.getKeyCode() == 'A' || k.getKeyCode() == 'a'))
    {
        for (auto& t : proc.getTriggers().getAll()) selected.insert (t.id);
        if (onEdited) onEdited();
        repaint();
        return true;
    }
    if (k == juce::KeyPress::escapeKey) { selected.clear(); repaint(); if (onEdited) onEdited(); return true; }
    if (k.getTextCharacter() == '+' || k.getTextCharacter() == '=') { zoomBy (1.5, getWidth() * 0.5f); return true; }
    if (k.getTextCharacter() == '-') { zoomBy (1.0 / 1.5, getWidth() * 0.5f); return true; }
    if (k.getTextCharacter() == 'f' || k.getTextCharacter() == 'F') { zoomToFit(); return true; }
    if ((k.isKeyCode (juce::KeyPress::leftKey) || k.isKeyCode (juce::KeyPress::rightKey)) && ! selected.empty())
    {
        const int64_t step = mods.isShiftDown() ? (int64_t) (0.001 * proc.getCurrentSampleRate()) : 1;
        const int64_t d = k.isKeyCode (juce::KeyPress::leftKey) ? -step : step;
        auto list = proc.getTriggers().getAll();
        for (auto& t : list) if (selected.count (t.id)) t.position = std::max<int64_t> (0, t.position + d);
        proc.commitTriggerEdit (std::move (list), "Nudge triggers");
        return true;
    }
    return false;
}

void WaveformEditor::showContextMenu (int index)
{
    const auto tr = displayed()[(size_t) index];
    juce::PopupMenu m;
    m.addItem (1, "Audition original");
    m.addItem (2, "Audition replacement");
    m.addSeparator();
    m.addItem (3, selected.count (tr.id) && selected.size() > 1 ? "Delete selected" : "Delete");
    juce::Component::SafePointer<WaveformEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe, tr] (int r)
    {
        if (safe == nullptr) return;
        if (r == 1) safe->proc.auditionCapture (tr.position);
        else if (r == 2) safe->proc.auditionReplacement (tr.velocity);
        else if (r == 3)
        {
            if (safe->selected.count (tr.id) == 0) safe->selected = { tr.id };
            safe->deleteSelection();
        }
    });
}

} // namespace thruma::ui
