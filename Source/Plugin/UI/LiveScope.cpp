#include "LiveScope.h"

namespace thruma::ui
{

LiveScope::LiveScope (ThrumaProcessor& p) : proc (p)
{
    setTooltip ("Live trigger view. Teal: drum level, amber line: trigger threshold (drag it), grey line: bleed floor.\n"
                "Amber marks: triggered hits. Grey x: candidates rejected as bleed.");
    startTimerHz (30);
}

void LiveScope::timerCallback()
{
    const auto w = proc.getScope().pointsWritten.load (std::memory_order_acquire);
    if (w != lastWritten || dragging) { lastWritten = w; repaint(); }
}

float LiveScope::dbToY (float db) const
{
    const auto r = getLocalBounds().toFloat().reduced (0.0f, 8.0f);
    return juce::jmap (juce::jlimit (kMinDb, kMaxDb, db), kMinDb, kMaxDb, r.getBottom(), r.getY());
}

float LiveScope::yToDb (float y) const
{
    const auto r = getLocalBounds().toFloat().reduced (0.0f, 8.0f);
    return juce::jmap (y, r.getBottom(), r.getY(), kMinDb, kMaxDb);
}

float LiveScope::currentThresholdDb() const
{
    const auto& sc = proc.getScope();
    const auto w = sc.pointsWritten.load (std::memory_order_acquire);
    if (w <= 0) return -40.0f;
    return sc.threshold[(size_t) ((w - 1) % DetectionScope::kPoints)].load (std::memory_order_relaxed);
}

void LiveScope::setThresholdDb (float db)
{
    // invert the detector's mapping: threshold = ref - range(sensitivity) - ghostRange
    const auto inst = (Instrument) (int) proc.apvts.getRawParameterValue (ids::instrument)->load();
    const auto& prof = getProfile (inst);
    const float ref = proc.getScope().referenceDb.load();
    const float ghost = proc.apvts.getRawParameterValue (ids::repGhost)->load() * 0.01f * 18.0f * prof.ghostWeight;
    const float range = ref - db - ghost;
    const float s = juce::jlimit (0.0f, 1.0f, ((range - prof.levelRangeDb) / 50.0f + 0.4f) / 0.8f);
    if (auto* prm = proc.apvts.getParameter (ids::repSens))
        prm->setValueNotifyingHost (prm->convertTo0to1 (s * 100.0f));
}

void LiveScope::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (colours::panel);
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.0f);

    g.setFont (juce::FontOptions (10.0f));
    for (float db = -60.0f; db <= -12.0f; db += 12.0f)
    {
        const float y = dbToY (db);
        g.setColour (colours::outline.withAlpha (0.6f));
        g.drawHorizontalLine ((int) y, 34.0f, bounds.getRight() - 4.0f);
        g.setColour (colours::textDim.withAlpha (0.7f));
        g.drawText (juce::String ((int) db), 4, (int) y - 6, 28, 12, juce::Justification::centredRight);
    }

    const auto& sc = proc.getScope();
    const auto written = sc.pointsWritten.load (std::memory_order_acquire);
    const auto plot = bounds.withTrimmedLeft (36.0f).withTrimmedRight (4.0f);
    const int visible = juce::jlimit (16, DetectionScope::kPoints - 64,
                                      (int) (kSeconds * juce::jmax (1.0, proc.getCurrentSampleRate()) / DetectionScope::kDecim));
    const int64_t first = written - visible;
    auto xOf = [&] (double point) { return plot.getX() + (float) ((point - (double) first) / visible) * plot.getWidth(); };

    if (written > 0)
    {
        juce::Path levelPath, thrPath, floorPath;
        const float bottom = dbToY (kMinDb);
        bool started = false;
        for (int64_t i = std::max<int64_t> (first, written - DetectionScope::kPoints + 1); i < written; ++i)
        {
            const auto k = (size_t) (i % DetectionScope::kPoints);
            const float x = xOf ((double) i);
            const float l = dbToY (sc.level[k].load (std::memory_order_relaxed));
            const float t = dbToY (sc.threshold[k].load (std::memory_order_relaxed));
            const float f = dbToY (sc.floor[k].load (std::memory_order_relaxed));
            if (! started)
            {
                levelPath.startNewSubPath (x, bottom);
                thrPath.startNewSubPath (x, t);
                floorPath.startNewSubPath (x, f);
                started = true;
            }
            levelPath.lineTo (x, l);
            thrPath.lineTo (x, t);
            floorPath.lineTo (x, f);
        }
        if (started)
        {
            levelPath.lineTo (xOf ((double) written - 1), bottom);
            levelPath.closeSubPath();
            g.setColour (colours::accent.withAlpha (0.35f));
            g.fillPath (levelPath);
            g.setColour (colours::textDim.withAlpha (0.8f));
            g.strokePath (floorPath, juce::PathStrokeType (1.0f));
            g.setColour (colours::trigger.withAlpha (overThreshold || dragging ? 1.0f : 0.85f));
            g.strokePath (thrPath, juce::PathStrokeType (overThreshold || dragging ? 2.5f : 1.5f));
        }

        // hit / rejection events
        const auto ev = sc.eventsWritten.load (std::memory_order_acquire);
        for (int64_t e = std::max<int64_t> (0, ev - DetectionScope::kEvents); e < ev; ++e)
        {
            const auto k = (size_t) (e % DetectionScope::kEvents);
            const double point = (double) sc.eventPos[k].load (std::memory_order_relaxed) / DetectionScope::kDecim;
            if (point < (double) first) continue;
            const float x = xOf (point);
            const float v = sc.eventVel[k].load (std::memory_order_relaxed);
            if (v >= 0.0f)
            {
                g.setColour (colours::trigger);
                g.drawVerticalLine ((int) x, plot.getY() + 4.0f, plot.getBottom() - 4.0f);
                g.fillEllipse (x - 3.0f, plot.getY() + 4.0f + (1.0f - v) * 20.0f, 6.0f, 6.0f);
            }
            else
            {
                g.setColour (colours::textDim);
                const float y = plot.getBottom() - 14.0f;
                g.drawLine (x - 3.0f, y - 3.0f, x + 3.0f, y + 3.0f, 1.2f);
                g.drawLine (x - 3.0f, y + 3.0f, x + 3.0f, y - 3.0f, 1.2f);
            }
        }
    }

    // labels
    const float thr = currentThresholdDb();
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (colours::trigger);
    g.drawText ("THRESHOLD " + juce::String (thr, 1) + " dB", (int) plot.getRight() - 170, (int) dbToY (thr) - 16, 164, 14,
                juce::Justification::centredRight);
    g.setColour (colours::textDim);
    g.drawText ("LIVE TRIGGER VIEW", (int) plot.getX() + 6, 6, 200, 14, juce::Justification::centredLeft);
}

void LiveScope::mouseMove (const juce::MouseEvent& e)
{
    const bool over = std::abs (e.position.y - dbToY (currentThresholdDb())) < 7.0f;
    if (over != overThreshold) { overThreshold = over; repaint(); }
    setMouseCursor (over ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void LiveScope::mouseDown (const juce::MouseEvent& e)
{
    dragging = std::abs (e.position.y - dbToY (currentThresholdDb())) < 7.0f;
    if (dragging)
        if (auto* p = proc.apvts.getParameter (ids::repSens)) p->beginChangeGesture();
}

void LiveScope::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging) setThresholdDb (yToDb (e.position.y));
}

void LiveScope::mouseUp (const juce::MouseEvent&)
{
    if (dragging)
        if (auto* p = proc.apvts.getParameter (ids::repSens)) p->endChangeGesture();
    dragging = false;
}

} // namespace thruma::ui
