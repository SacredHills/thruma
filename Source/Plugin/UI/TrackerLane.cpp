#include "TrackerLane.h"

namespace thruma::ui
{

namespace
{
    constexpr int kRulerH = 20, kScrollH = 12, kMidiH = 84, kGap = 6;
    constexpr float kMinDb = -66.0f, kMaxDb = 0.0f;

    juce::String timeText (double s)
    {
        const int m = (int) (s / 60.0);
        return juce::String (m) + ":" + juce::String (s - m * 60.0, 1).paddedLeft ('0', 4);
    }
}

TrackerLane::TrackerLane (ThrumaProcessor& p) : proc (p)
{
    setWantsKeyboardFocus (true);
    hScroll.setAutoHide (false);
    hScroll.addListener (this);
    addAndMakeVisible (hScroll);
    zoom.setSliderStyle (juce::Slider::LinearHorizontal);
    zoom.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    zoom.setRange (0.0, 1.0);
    zoom.setTooltip ("Zoom: all the way left shows the whole song");
    zoom.onValueChange = [this] { zoomChanged(); };
    addAndMakeVisible (zoom);
    startTimerHz (30);
}

TrackerLane::~TrackerLane() { hScroll.removeListener (this); }

juce::Rectangle<int> TrackerLane::audioArea() const
{
    return getLocalBounds().withTrimmedTop (kRulerH).withTrimmedBottom (kScrollH + kMidiH + kGap);
}

juce::Rectangle<int> TrackerLane::midiArea() const
{
    return getLocalBounds().withTrimmedBottom (kScrollH).removeFromBottom (kMidiH);
}

float TrackerLane::dbToY (float db) const
{
    const auto a = audioArea().toFloat().reduced (0.0f, 4.0f);
    return juce::jmap (juce::jlimit (kMinDb, kMaxDb, db), kMinDb, kMaxDb, a.getBottom(), a.getY());
}

float TrackerLane::yToDb (float y) const
{
    const auto a = audioArea().toFloat().reduced (0.0f, 4.0f);
    return juce::jmap (y, a.getBottom(), a.getY(), kMinDb, kMaxDb);
}

float TrackerLane::velToY (float v) const
{
    const auto m = midiArea().toFloat().reduced (0.0f, 6.0f);
    return m.getBottom() - v * m.getHeight();
}

int64_t TrackerLane::historyStart() const { return std::max<int64_t> (0, proc.getCapture().origin.load()); }
int64_t TrackerLane::historyEnd() const
{
    const auto o = proc.getCapture().origin.load();
    return o < 0 ? 0 : o + proc.getCapture().length.load();
}

void TrackerLane::resized()
{
    auto bottom = getLocalBounds().removeFromBottom (kScrollH + 4);
    zoom.setBounds (bottom.removeFromRight (150).reduced (2, 0));
    bottom.removeFromRight (42);   // "ZOOM" label
    hScroll.setBounds (bottom.withTrimmedTop (2));
    updateScrollBar();
}

double TrackerLane::fitSpp() const
{
    const double sr = juce::jmax (1.0, proc.getCurrentSampleRate());
    const double len = juce::jmax ((double) (historyEnd() - historyStart()), 20.0 * sr);
    return juce::jmax (2.0, len / juce::jmax (1, getWidth()));
}

void TrackerLane::zoomChanged()
{
    const double centre = viewStart + getWidth() * spp * 0.5;
    const double fit = fitSpp();
    spp = fit * std::pow (2.0 / fit, zoom.getValue());   // 0 = whole song, 1 = sample-level
    viewStart = zoom.getValue() <= 0.001 ? (double) historyStart() : centre - getWidth() * spp * 0.5;
    lastInteractionMs = juce::Time::getMillisecondCounter();
    updateScrollBar();
    repaint();
}

void TrackerLane::syncZoomSlider()
{
    const double fit = fitSpp();
    zoom.setValue (juce::jlimit (0.0, 1.0, std::log (spp / fit) / std::log (2.0 / fit)), juce::dontSendNotification);
}

static bool isMidiFile (const juce::File& f) { return f.hasFileExtension ("mid;midi;smf"); }

bool TrackerLane::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files) if (KitManager::isAudioFile (juce::File (f)) || isMidiFile (juce::File (f))) return true;
    return false;
}

void TrackerLane::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
    {
        if (isMidiFile (juce::File (f))) { if (onMidiFileDropped) onMidiFileDropped (juce::File (f)); return; }
        if (KitManager::isAudioFile (juce::File (f))) { if (onAudioFileDropped) onAudioFileDropped (juce::File (f)); return; }
    }
}

void TrackerLane::updateScrollBar()
{
    const double visible = getWidth() * spp;
    const double a = std::min ((double) historyStart(), viewStart), b = std::max ((double) historyEnd(), viewStart + visible);
    hScroll.setRangeLimits (a, b, juce::dontSendNotification);
    hScroll.setCurrentRange (viewStart, visible, juce::dontSendNotification);
}

void TrackerLane::scrollBarMoved (juce::ScrollBar*, double start)
{
    viewStart = start;
    lastInteractionMs = juce::Time::getMillisecondCounter();
    repaint();
}

void TrackerLane::zoomToFit()
{
    const auto a = historyStart(), b = historyEnd();
    if (b > a) { viewStart = (double) a; spp = juce::jmax (1.0, (double) (b - a) / juce::jmax (1, getWidth())); }
    syncZoomSlider();
    updateScrollBar();
    repaint();
}

void TrackerLane::timerCallback()
{
    const double sr = juce::jmax (1.0, proc.getCurrentSampleRate());
    if (! initialised && getWidth() > 0)
    {
        spp = 8.0 * sr / getWidth();   // ~8 seconds visible
        viewStart = (double) proc.getPlayheadPosition() - getWidth() * spp * 0.85;
        initialised = true;
        syncZoomSlider();
    }
    if (proc.isTransportPlaying() && drag == Drag::None && juce::Time::getMillisecondCounter() - lastInteractionMs > 2500)
    {
        const double ph = (double) proc.getPlayheadPosition(), visible = getWidth() * spp;
        if (ph > viewStart + visible * 0.92 || ph < viewStart) viewStart = ph - visible * 0.15;
    }
    // when stopped and nothing of the heard song is in view, show the most recent part of it
    if (! proc.isTransportPlaying() && drag == Drag::None && juce::Time::getMillisecondCounter() - lastInteractionMs > 2500
        && historyEnd() > historyStart())
    {
        const double visible = getWidth() * spp;
        if (viewStart > (double) historyEnd() || viewStart + visible < (double) historyStart())
            viewStart = std::max ((double) historyStart(), (double) historyEnd() - visible * 0.85);
    }
    for (auto it = selected.begin(); it != selected.end();)
        it = proc.getTriggers().find (*it) == nullptr ? selected.erase (it) : std::next (it);
    updateScrollBar();
    repaint();
}

void TrackerLane::paint (juce::Graphics& g)
{
    const double sr = juce::jmax (1.0, proc.getCurrentSampleRate());
    g.fillAll (colours::panel);
    const auto audio = audioArea(), midi = midiArea();

    // ruler
    {
        auto ruler = getLocalBounds().removeFromTop (kRulerH);
        g.setColour (colours::panelRaised);
        g.fillRect (ruler);
        const double visibleSec = getWidth() * spp / sr;
        double step = 60.0;
        for (double s : { 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0 }) if (visibleSec / s <= 14.0) { step = s; break; }
        g.setFont (juce::FontOptions (11.0f));
        for (double t = std::floor (viewStart / sr / step) * step; t * sr < viewStart + getWidth() * spp; t += step)
        {
            const float x = posToX (t * sr);
            g.setColour (colours::outline.withAlpha (0.7f));
            g.drawVerticalLine ((int) x, (float) audio.getY(), (float) midi.getBottom());
            g.setColour (colours::textDim);
            g.drawText (timeText (t), (int) x + 3, 2, 70, kRulerH - 4, juce::Justification::centredLeft);
        }
    }

    // lane backgrounds
    g.setColour (colours::background.withAlpha (0.35f));
    g.fillRect (midi);
    g.setColour (colours::outline);
    g.drawHorizontalLine (midi.getY() - kGap / 2, 0.0f, (float) getWidth());

    // detection level history
    const auto& lvl = proc.getEnvelopeLevel();
    const auto origin = proc.getCapture().origin.load();
    const int bin = ThrumaProcessor::kEnvBin;
    if (origin >= 0 && ! lvl.empty())
    {
        juce::Path area;
        bool open = false;
        const float bottom = (float) audio.getBottom();
        for (int x = 0; x < getWidth(); ++x)
        {
            const auto a = ((int64_t) xToPos ((float) x) - origin) / bin, b = ((int64_t) xToPos ((float) x + 1.0f) - origin) / bin;
            float m = -200.0f;
            const auto validBins = proc.getCapture().length.load() / bin;
            for (auto i = std::max<int64_t> (0, a); i <= std::max (a, b) && i < (int64_t) lvl.size() && i < validBins; ++i) m = std::max (m, lvl[(size_t) i]);
            if (m < -150.0f) { if (open) { area.lineTo ((float) x, bottom); area.closeSubPath(); open = false; } continue; }
            if (! open) { area.startNewSubPath ((float) x, bottom); open = true; }
            area.lineTo ((float) x, dbToY (m));
        }
        if (open) { area.lineTo ((float) getWidth(), bottom); area.closeSubPath(); }
        g.setColour (colours::accent.withAlpha (0.45f));
        g.fillPath (area);
    }

    // hits: lines in the audio lane, velocity bars in the MIDI lane
    const auto& t = shown();
    const auto lo = (int64_t) xToPos (-8.0f), hi = (int64_t) xToPos ((float) getWidth() + 8.0f);
    for (auto it = std::lower_bound (t.begin(), t.end(), lo, [] (const Trigger& a, int64_t v) { return a.position < v; });
         it != t.end() && it->position <= hi; ++it)
    {
        const float x = posToX ((double) it->position);
        const bool sel = selected.count (it->id) > 0;
        const auto c = sel ? colours::selection : colours::trigger.withAlpha (it->ghost ? 0.6f : 1.0f);
        g.setColour (c.withAlpha (sel ? 0.8f : 0.45f));
        g.drawVerticalLine ((int) x, (float) audio.getY(), (float) audio.getBottom());
        const float top = velToY (it->velocity);
        g.setColour (c);
        g.fillRect (x - 2.0f, top, 4.0f, (float) midi.getBottom() - 6.0f - top);
        g.fillEllipse (x - 4.0f, top - 4.0f, 8.0f, 8.0f);
    }

    // candidates ignored as bleed (recent ones): small grey crosses at the bottom of the audio lane
    {
        const auto& sc = proc.getScope();
        const auto off = proc.getScopeToTimelineOffset();
        const auto ev = sc.eventsWritten.load (std::memory_order_acquire);
        g.setColour (colours::textDim.withAlpha (0.8f));
        for (int64_t e = std::max<int64_t> (0, ev - DetectionScope::kEvents); e < ev; ++e)
        {
            const auto k = (size_t) (e % DetectionScope::kEvents);
            if (sc.eventVel[k].load (std::memory_order_relaxed) >= 0.0f) continue;
            const float x = posToX ((double) (sc.eventPos[k].load (std::memory_order_relaxed) + off));
            if (x < 0.0f || x > (float) getWidth()) continue;
            const float y = (float) audio.getBottom() - 10.0f;
            g.drawLine (x - 3.0f, y - 3.0f, x + 3.0f, y + 3.0f, 1.2f);
            g.drawLine (x - 3.0f, y + 3.0f, x + 3.0f, y - 3.0f, 1.2f);
        }
    }

    // threshold (current) - drag to set
    {
        const float thr = proc.getCurrentThresholdDb();
        const float y = dbToY (thr);
        const bool hot = hoverThreshold || drag == Drag::Threshold;
        g.setColour (colours::trigger.withAlpha (hot ? 1.0f : 0.8f));
        const float dashes[] = { 6.0f, 4.0f };
        g.drawDashedLine (juce::Line<float> (0.0f, y, (float) getWidth(), y), dashes, 2, hot ? 2.5f : 1.5f);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ("THRESHOLD " + juce::String (thr, 1) + " dB  (drag)", getWidth() - 216, (int) y - 17, 210, 14, juce::Justification::centredRight);
    }

    if (drag == Drag::Rubber)
    {
        g.setColour (colours::accent.withAlpha (0.15f));
        g.fillRect (rubber);
    }
    if (proc.isTransportPlaying() || proc.getOfflineSource() != ThrumaProcessor::OfflineSource::None || proc.canControlHostTransport())
    {
        g.setColour (proc.isTransportPlaying() ? colours::record : colours::record.withAlpha (0.8f));
        const float x = posToX ((double) proc.getPlayheadPosition());
        g.fillPath ([x] { juce::Path p; p.addTriangle (x - 5.0f, 0.0f, x + 5.0f, 0.0f, x, 8.0f); return p; }());
        g.drawVerticalLine ((int) x, (float) audio.getY(), (float) midi.getBottom());
    }

    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    for (auto [label, y] : { std::pair<const char*, int> { "AUDIO", audio.getY() + 4 }, { "MIDI", midi.getY() + 4 } })
    {
        g.setColour (colours::background.withAlpha (0.75f));
        g.fillRoundedRectangle (3.0f, (float) y, 44.0f, 14.0f, 3.0f);
        g.setColour (colours::textDim);
        g.drawText (label, 6, y, 40, 14, juce::Justification::centredLeft);
    }
    {
        auto zb = zoom.getBounds();
        g.setFont (juce::FontOptions (10.0f));
        g.setColour (colours::textDim);
        g.drawText ("ZOOM", zb.getX() - 40, zb.getY(), 36, zb.getHeight(), juce::Justification::centredRight);
    }
    if (origin < 0 && t.empty())
    {
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText (juce::String (proc.canReadHostTrack() ? "Press Analyze to track the whole song at once, or press play: hits appear here as MIDI notes.\n"
                                                                : "Press play in your DAW: the drum's hits appear here as MIDI notes in real time.\n")
                          + "Or drop an audio file of the drum track here (Import Audio), or a MIDI file to play the kit.",
                          audio.reduced (20), juce::Justification::centred, 3);
    }
    g.setColour (colours::outline);
    g.drawRect (getLocalBounds());
}

int TrackerLane::hitTestBar (juce::Point<float> p, bool& onTop) const
{
    const auto& t = shown();
    int best = -1;
    float bestD = 6.0f;
    for (auto it = std::lower_bound (t.begin(), t.end(), (int64_t) xToPos (p.x - 8.0f), [] (const Trigger& a, int64_t v) { return a.position < v; });
         it != t.end() && it->position <= (int64_t) xToPos (p.x + 8.0f); ++it)
    {
        const float d = std::abs (posToX ((double) it->position) - p.x);
        if (d < bestD) { bestD = d; best = (int) (it - t.begin()); }
    }
    onTop = best >= 0 && std::abs (p.y - velToY (t[(size_t) best].velocity)) < 8.0f;
    return best;
}

void TrackerLane::setThresholdDb (float db)
{
    // invert the detector's mapping: threshold = reference - range(Sensitivity) - ghost range
    const auto inst = (Instrument) (int) proc.apvts.getRawParameterValue (ids::instrument)->load();
    const auto& prof = getProfile (inst);
    const float ghost = proc.apvts.getRawParameterValue (ids::repGhost)->load() * 0.01f * 18.0f * prof.ghostWeight;
    const float range = proc.getCurrentReferenceDb() - db - ghost;
    const float s = juce::jlimit (0.0f, 1.0f, ((range - prof.levelRangeDb) / 50.0f + 0.4f) / 0.8f);
    if (auto* prm = proc.apvts.getParameter (ids::repSens))
        prm->setValueNotifyingHost (prm->convertTo0to1 (s * 100.0f));
}

void TrackerLane::mouseMove (const juce::MouseEvent& e)
{
    if (e.position.y < (float) kRulerH) { setMouseCursor (juce::MouseCursor::PointingHandCursor); return; }
    bool top = false;
    const bool overThr = audioArea().contains (e.getPosition()) && std::abs (e.position.y - dbToY (proc.getCurrentThresholdDb())) < 7.0f;
    const int bar = midiArea().contains (e.getPosition()) ? hitTestBar (e.position, top) : -1;
    hoverThreshold = overThr;
    setMouseCursor (overThr || (bar >= 0 && top) ? juce::MouseCursor::UpDownResizeCursor
                                                 : bar >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void TrackerLane::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    lastInteractionMs = juce::Time::getMillisecondCounter();
    if (e.position.y < (float) kRulerH)
    {
        // ruler: play from here
        const auto target = (int64_t) std::max (0.0, xToPos (e.position.x));
        const auto why = proc.locateTo (target);
        if (onMessage)
            onMessage (why.isNotEmpty() ? why : "Play from " + juce::String ((double) target / juce::jmax (1.0, proc.getCurrentSampleRate()), 1) + " s");
        repaint();
        return;
    }
    if (audioArea().contains (e.getPosition()) && std::abs (e.position.y - dbToY (proc.getCurrentThresholdDb())) < 7.0f)
    {
        drag = Drag::Threshold;
        if (auto* prm = proc.apvts.getParameter (ids::repSens)) prm->beginChangeGesture();
        return;
    }
    bool top = false;
    const int idx = hitTestBar (e.position, top);
    if (idx >= 0)
    {
        const auto id = shown()[(size_t) idx].id;
        if (e.mods.isShiftDown()) { if (! selected.erase (id)) selected.insert (id); }
        else if (selected.count (id) == 0) selected = { id };
        working = original = proc.getTriggers().getAll();
        editing = true;
        changed = false;
        drag = (top && midiArea().contains (e.getPosition())) ? Drag::Velocity : Drag::Move;
        dragStartPos = xToPos (e.position.x);
    }
    else
    {
        if (! e.mods.isShiftDown()) selected.clear();
        drag = Drag::Rubber;
        rubber = { e.position, e.position };
    }
    repaint();
}

void TrackerLane::mouseDrag (const juce::MouseEvent& e)
{
    lastInteractionMs = juce::Time::getMillisecondCounter();
    if (drag == Drag::Threshold) { setThresholdDb (yToDb (e.position.y)); return; }
    if (drag == Drag::Move || drag == Drag::Velocity)
    {
        const auto delta = (int64_t) std::llround (xToPos (e.position.x) - dragStartPos);
        const auto m = midiArea().toFloat().reduced (0.0f, 6.0f);
        const float vel = juce::jlimit (0.01f, 1.0f, (m.getBottom() - e.position.y) / m.getHeight());
        working = original;
        for (auto& tr : working)
        {
            if (selected.count (tr.id) == 0) continue;
            if (drag == Drag::Move) tr.position = std::max<int64_t> (0, tr.position + delta);
            else tr.velocity = vel;
        }
        std::stable_sort (working.begin(), working.end(), [] (const Trigger& a, const Trigger& b) { return a.position < b.position; });
        changed = true;
    }
    else if (drag == Drag::Rubber)
    {
        rubber = juce::Rectangle<float> (e.mouseDownPosition, e.position);
        if (! e.mods.isShiftDown()) selected.clear();
        const auto a = (int64_t) xToPos (rubber.getX()), b = (int64_t) xToPos (rubber.getRight());
        for (auto& tr : proc.getTriggers().getAll())
            if (tr.position >= a && tr.position <= b) selected.insert (tr.id);
    }
    repaint();
}

void TrackerLane::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::Threshold)
        if (auto* prm = proc.apvts.getParameter (ids::repSens)) prm->endChangeGesture();
    if ((drag == Drag::Move || drag == Drag::Velocity) && changed)
        proc.commitTriggerEdit (working, drag == Drag::Move ? "Move notes" : "Change velocity");
    editing = false;
    drag = Drag::None;
    repaint();
}

void TrackerLane::mouseDoubleClick (const juce::MouseEvent& e)
{
    bool top = false;
    if (hitTestBar (e.position, top) >= 0) return;
    const auto m = midiArea().toFloat().reduced (0.0f, 6.0f);
    const float vel = midiArea().contains (e.getPosition()) ? juce::jlimit (0.05f, 1.0f, (m.getBottom() - e.position.y) / m.getHeight()) : 0.8f;
    TriggerList copy;
    copy.setAll (proc.getTriggers().getAll());
    selected = { copy.add ((int64_t) std::max (0.0, xToPos (e.position.x)), vel) };
    proc.commitTriggerEdit (copy.getAll(), "Add note");
}

void TrackerLane::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    lastInteractionMs = juce::Time::getMillisecondCounter();
    if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
        viewStart -= (std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY) * getWidth() * spp * 0.5;
    else
    {
        const double anchor = xToPos (e.position.x);
        spp = juce::jlimit (1.0, fitSpp(), spp / std::exp (w.deltaY * 1.5));
        viewStart = anchor - e.position.x * spp;
        syncZoomSlider();
    }
    updateScrollBar();
    repaint();
}

void TrackerLane::deleteSelection()
{
    if (selected.empty()) return;
    std::vector<Trigger> keep;
    for (auto& t : proc.getTriggers().getAll()) if (selected.count (t.id) == 0) keep.push_back (t);
    selected.clear();
    proc.commitTriggerEdit (std::move (keep), "Delete notes");
}

bool TrackerLane::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
    if (mods.isCommandDown() && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z'))
    {
        if (mods.isShiftDown()) proc.getUndoManager().redo(); else proc.getUndoManager().undo();
        return true;
    }
    if (mods.isCommandDown() && (k.getKeyCode() == 'A' || k.getKeyCode() == 'a'))
    {
        for (auto& t : proc.getTriggers().getAll()) selected.insert (t.id);
        return true;
    }
    if (k.getTextCharacter() == 'f' || k.getTextCharacter() == 'F') { zoomToFit(); return true; }
    return false;
}

} // namespace thruma::ui
