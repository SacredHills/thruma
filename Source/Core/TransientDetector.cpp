#include "TransientDetector.h"

#include <algorithm>
#include <cmath>
#ifdef THRUMA_DETECTOR_DEBUG
#include <cstdio>
#endif

namespace thruma
{

namespace
{
    inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }
    inline float fastDb (float v) noexcept { return v > 1.0e-9f ? 20.0f * std::log10 (v) : -180.0f; }
}

void TransientDetector::prepare (double sampleRate)
{
    sr = sampleRate;
    const int maxWin = msToSamples (25.0, sr) + 4;
    bodyEnv.prepare (maxWin);
    atkEnv.prepare (maxWin);
    fullEnv.prepare (maxWin);
    posEnv.prepare (maxWin);
    bodyDbHist.prepare (msToSamples (60.0, sr));
    atkDbHist.prepare (msToSamples (60.0, sr));
    posEnvRing.prepare (msToSamples (60.0, sr));
    floorBlockLen = std::max (16, msToSamples (20.0, sr));
    prepared = true;
    updateDerived();
    reset (0);
}

void TransientDetector::setSettings (const DetectorSettings& s) noexcept
{
    const bool instrumentChanged = s.instrument != settings.instrument;
    settings = s;
    settings.sensitivity      = std::clamp (settings.sensitivity, 0.0f, 1.0f);
    settings.ghostSensitivity = std::clamp (settings.ghostSensitivity, 0.0f, 1.0f);
    settings.dynamicRangeDb   = std::clamp (settings.dynamicRangeDb, 6.0f, 90.0f);
    if (prepared)
    {
        updateDerived();
        if (instrumentChanged)
        {
            state = State::Idle;
            if (! templateFixed) tmpl = {};
        }
    }
}

void TransientDetector::setVelocityReference (float r) noexcept
{
    if (std::isfinite (r) && r > -150.0f) { refDb = r; refFixed = true; }
    else refFixed = false;
}

void TransientDetector::setSpectralTemplate (const SpectralTemplate& t) noexcept
{
    tmpl = t;
    templateFixed = t.isValid();
    if (! templateFixed) tmpl = {};
}

float TransientDetector::shapeDistance (const std::array<float, SpectralTemplate::kBands>& a, const SpectralTemplate& t) noexcept
{
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < SpectralTemplate::kBands; ++i)
    {
        const double d = std::clamp ((double) a[(size_t) i] - t.shape[(size_t) i], -20.0, 20.0);
        sum += d * d;
        ++n;
    }
    return n > 0 ? (float) std::sqrt (sum / n) : 0.0f;
}

void TransientDetector::updateDerived() noexcept
{
    const auto& p = getProfile (settings.instrument);
    // Sensitivity 0..1 maps onto the useful internal range 0..0.8 (0.5 -> 0.4: balanced default).
    const float s = settings.sensitivity * 0.8f;

    bodyHp1.setHighPass (sr, p.bandLowHz, 0.5412);  // 4th-order Butterworth HP
    bodyHp2.setHighPass (sr, p.bandLowHz, 1.3066);
    hasBodyLp = p.bandHighHz > 0.0f && p.bandHighHz < sr * 0.45;
    if (hasBodyLp) bodyLp.setLowPass (sr, p.bandHighHz); else bodyLp.setBypass();

    atkHp1.setHighPass (sr, p.attackLowHz, 0.5412);
    atkHp2.setHighPass (sr, p.attackLowHz, 1.3066);
    hasAtkLp = p.attackHighHz > 0.0f && p.attackHighHz < sr * 0.45;
    if (hasAtkLp) atkLp.setLowPass (sr, p.attackHighHz); else atkLp.setBypass();

    posHp.setHighPass (sr, std::max (20.0f, p.attackLowHz * 0.5f));  // positioning ignores the previous hit's low ring
    fullHp.setHighPass (sr, 20.0);

    // fingerprint bands: log-spaced from the body band up to 12 kHz
    const double lo = std::max (30.0f, p.bandLowHz), hi = std::min (12000.0, sr * 0.42);
    for (int b = 0; b < SpectralTemplate::kBands; ++b)
    {
        const double fc = lo * std::pow (hi / lo, (b + 0.5) / SpectralTemplate::kBands);
        specBp[(size_t) b].setBandPass (sr, fc, 1.2);
    }

    holdBody = std::max (1, msToSamples (p.holdMs, sr));
    holdAtk  = std::max (1, msToSamples (std::min (p.holdMs, 1.0f), sr));
    posHold  = std::max (1, msToSamples (0.7, sr));
    riseWin  = std::max (1, msToSamples (p.riseWindowMs, sr));

    const int budget = std::max (4, msToSamples (settings.maxReportDelayMs, sr));
    backtrack = msToSamples (3.0, sr);
    velWindow = std::max (1, msToSamples (p.velocityWindowMs, sr));
    if (backtrack + velWindow + 1 > budget)
    {
        backtrack = std::max (msToSamples (0.5, sr), std::min (backtrack, (int) (budget * 0.3f)));
        velWindow = std::max (msToSamples (0.5, sr), budget - backtrack - 1);
    }

    const float retrigMs = settings.retriggerMs > 0.0f ? settings.retriggerMs : p.retriggerMs;
    retrig = std::max (1, msToSamples (retrigMs, sr));

    bodyEnv.setWindow (holdBody);
    fullEnv.setWindow (holdBody);
    atkEnv.setWindow (holdAtk);
    posEnv.setWindow (posHold);

    riseThrDb     = lerp (14.0f, 4.0f, s);
    mainRiseMinDb = lerp (3.0f, 2.0f, s);
    floorMarginDb = lerp (12.0f, 4.0f, s);
    relRangeDb    = p.levelRangeDb + (s - 0.5f) * 16.0f;
    ghostRelDb    = settings.ghostSensitivity * 18.0f * p.ghostWeight;
    focusMinDb    = p.focusMinDb - s * 6.0f;
    specThrDb     = lerp (3.0f, 8.0f, s);

    refDecayPerSample = (float) (1.0 / sr);   // adaptive reference falls 1 dB/s
    smoothCoeff = onePoleCoeff (0.010 * sr);
}

void TransientDetector::reset (int64_t startPosition) noexcept
{
    if (! prepared) return;
    bodyHp1.reset(); bodyHp2.reset(); bodyLp.reset();
    atkHp1.reset(); atkHp2.reset(); atkLp.reset();
    posHp.reset(); fullHp.reset();
    for (auto& b : specBp) b.reset();
    bodyEnv.reset(); atkEnv.reset(); fullEnv.reset(); posEnv.reset();
    bodyDbHist.reset(); atkDbHist.reset(); posEnvRing.reset();
    for (int i = 0; i < bodyDbHist.capacity(); ++i) { bodyDbHist.push (-180.0f); atkDbHist.push (-180.0f); }

    bodyBlockMin.fill (-120.0f);
    atkBlockMin.fill (-120.0f);
    bodyCurMin = atkCurMin = 200.0f;
    floorBlockPos = floorBlockIdx = 0;

    pos = startPosition;
    bodyFloorDb = atkFloorDb = -120.0f;
    if (! refFixed) refDb = -12.0f;
    atkRefDb = -12.0f;
    if (! templateFixed) tmpl = {};
    state = State::Idle;
    lastOnset = startPosition - 1000000000LL;
    cooldownUntil = startPosition;
    rearmed = true;
    lastHitPeakDb = -150.0f;
    bodySmoothDb = -180.0f;
}

float TransientDetector::velocityFor (float peakDb) const noexcept
{
    const float range = settings.dynamicRangeDb;
    return std::clamp ((peakDb - (refDb - range)) / range, 0.01f, 1.0f);
}

bool TransientDetector::processSample (float x, DetectedHit& hit) noexcept
{
    // --- filtering & envelopes ---
    float b = bodyHp2.process (bodyHp1.process (x));
    if (hasBodyLp) b = bodyLp.process (b);
    float a = atkHp2.process (atkHp1.process (x));
    if (hasAtkLp) a = atkLp.process (a);
    const float xp = posHp.process (x);
    const float xf = fullHp.process (x);

    std::array<float, SpectralTemplate::kBands> bandOut;
    for (int i = 0; i < SpectralTemplate::kBands; ++i)
        bandOut[(size_t) i] = specBp[(size_t) i].process (x);

    const float bodyE = bodyEnv.push (std::abs (b));
    const float atkE  = atkEnv.push (std::abs (a));
    const float fullE = fullEnv.push (std::abs (xf));
    const float posE  = posEnv.push (std::abs (xp));
    posEnvRing.push (posE);

    const float bodyDb = fastDb (bodyE);
    const float atkDb  = fastDb (atkE);
    bodyDbHist.push (bodyDb);
    atkDbHist.push (atkDb);
    const float bodyRise = bodyDb - bodyDbHist.get (riseWin);
    const float atkRise  = atkDb - atkDbHist.get (riseWin);

    // --- minimum-statistics floor: min of the held envelope over the last ~400 ms ---
    bodyCurMin = std::min (bodyCurMin, bodyDb);
    atkCurMin  = std::min (atkCurMin, atkDb);
    if (++floorBlockPos >= floorBlockLen)
    {
        bodyBlockMin[(size_t) floorBlockIdx] = bodyCurMin;
        atkBlockMin[(size_t) floorBlockIdx] = atkCurMin;
        floorBlockIdx = (floorBlockIdx + 1) % kFloorBlocks;
        floorBlockPos = 0;
        bodyCurMin = atkCurMin = 200.0f;
        bodyFloorDb = *std::min_element (bodyBlockMin.begin(), bodyBlockMin.end());
        atkFloorDb  = *std::min_element (atkBlockMin.begin(), atkBlockMin.end());
    }

    if (! refFixed)
        refDb = std::max (-60.0f, refDb - refDecayPerSample);
    atkRefDb = std::max (-70.0f, atkRefDb - refDecayPerSample);

    // smoothed level: robust against momentary dips (beating modes, noisy wires) for re-arming
    bodySmoothDb = bodyDb + (bodySmoothDb - bodyDb) * smoothCoeff;
    if (! rearmed && bodySmoothDb < lastHitPeakDb - 8.0f)
        rearmed = true;

    bool emitted = false;

    if (state == State::Idle)
    {
        const bool timeOk = (pos - lastOnset) >= retrig && pos >= cooldownUntil;
        if (timeOk)
        {
            const bool trigA = atkRise >= riseThrDb && atkDb >= atkFloorDb + floorMarginDb;
            const bool trigB = bodyRise >= riseThrDb + 3.0f && bodyDb >= bodyFloorDb + floorMarginDb;
            const bool rearmOk = rearmed
                              || (settings.doubleHits && (atkRise >= riseThrDb + 2.0f || bodyRise >= riseThrDb + 4.0f));
            if ((trigA || trigB) && rearmOk)
            {
                state = State::Candidate;
                candStart = pos;
                candPeakBody = bodyE;
                candPeakFull = fullE;
                candPeakPos = std::abs (xp);
                candPeakPosIdx = pos;
                candLevelBeforeDb = std::max (bodyDbHist.get (std::min (riseWin + 2, bodyDbHist.capacity() - 1)), bodySmoothDb);
                candPeakAtk = atkE;
                candAtkBeforeDb = atkDbHist.get (std::min (riseWin + 2, atkDbHist.capacity() - 1));
                specAcc.fill (0.0);
                specCount = 0;
            }
        }
    }
    else
    {
        candPeakBody = std::max (candPeakBody, bodyE);
        candPeakFull = std::max (candPeakFull, fullE);
        candPeakAtk  = std::max (candPeakAtk, atkE);
        if (std::abs (xp) > candPeakPos)
        {
            candPeakPos = std::abs (xp);
            candPeakPosIdx = pos;
        }
    }

    if (state == State::Candidate)
    {
        for (int i = 0; i < SpectralTemplate::kBands; ++i)
            specAcc[(size_t) i] += (double) bandOut[(size_t) i] * bandOut[(size_t) i];
        ++specCount;

        if (pos - candStart >= velWindow)
        {
            const float peakDb = fastDb (candPeakBody);
            const float fullDb = fastDb (candPeakFull);
            const float absThr = std::max (bodyFloorDb + floorMarginDb, refDb - relRangeDb - ghostRelDb);
            const bool levelOk = peakDb >= absThr;
            // Body must rise. While the previous hit still rings, body fluctuations (beating of overlapping
            // decays) are unreliable, so a hit-like attack is also required there. A clear fresh attack on a
            // still-ringing previous hit (double bass, drags) is accepted even without body rise.
            const float atkPeakDb = fastDb (candPeakAtk);
            const float atkRise = atkPeakDb - candAtkBeforeDb;
            const bool atkLikeHit = atkPeakDb >= atkRefDb - 12.0f;
            const bool bodyRiseOk = rearmed ? (peakDb - candLevelBeforeDb) >= mainRiseMinDb
                                            : (peakDb - candLevelBeforeDb) >= std::max (3.0f, mainRiseMinDb)
                                                  && atkLikeHit && atkRise >= 0.5f * riseThrDb;
            const bool riseOk = bodyRiseOk
                             || (atkRise >= 0.6f * riseThrDb && peakDb >= lastHitPeakDb - 10.0f && atkPeakDb >= atkRefDb - 6.0f);
            const bool focusOk = ! settings.frequencyFocus || (peakDb - fullDb) >= focusMinDb;

            // spectral fingerprint of this candidate
            // (band levels relative to their mean, each band floored 40 dB below the loudest)
            std::array<float, SpectralTemplate::kBands> shape;
            float maxBand = -300.0f;
            for (int i = 0; i < SpectralTemplate::kBands; ++i)
            {
                shape[(size_t) i] = (float) (10.0 * std::log10 (specAcc[(size_t) i] / std::max (1, specCount) + 1e-24));
                maxBand = std::max (maxBand, shape[(size_t) i]);
            }
            float mean = 0.0f;
            for (auto& v : shape) { v = std::max (v, maxBand - 40.0f); mean += v; }
            mean /= (float) SpectralTemplate::kBands;
            for (auto& v : shape) v -= mean;

            const bool confident = peakDb >= refDb - 6.0f;
            bool shapeOk = true;
            float dist = 0.0f;
            if (! confident && settings.frequencyFocus && tmpl.isValid())
            {
                dist = shapeDistance (shape, tmpl);
                shapeOk = dist <= specThrDb;
            }

            state = State::Idle;
#ifdef THRUMA_DETECTOR_DEBUG
            std::printf ("cand %lld peak=%.1f before=%.1f full=%.1f absThr=%.1f floor=%.1f ref=%.1f dist=%.1f -> %d%d%d%d  [%.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f]\n",
                         (long long) candStart, peakDb, candLevelBeforeDb, fullDb, absThr, bodyFloorDb, refDb, dist,
                         levelOk, riseOk, focusOk, shapeOk, shape[0], shape[1], shape[2], shape[3], shape[4], shape[5], shape[6], shape[7]);
#endif

            if (levelOk && riseOk && focusOk && shapeOk)
            {
                // --- sub-ms onset positioning: walk back from the peak to the first threshold crossing ---
                const int64_t limit = std::max (candStart - backtrack, pos - (int64_t) posEnvRing.capacity() + 2);
                const float residual = posEnvRing.get ((int) (pos - limit));
                const float thr = std::min (0.6f * candPeakPos, std::max (0.15f * candPeakPos, 2.0f * residual));
                int64_t onset = -1;
                for (int64_t k = candPeakPosIdx; k >= limit; --k)
                {
                    if (posEnvRing.get ((int) (pos - k)) < thr)
                    {
                        onset = k + 1;
                        break;
                    }
                }
                if (onset < 0)
                    onset = std::max (limit, candStart - holdAtk);

                hit.position = onset;
                hit.peakDb   = peakDb;
                hit.velocity = velocityFor (peakDb);
                hit.ghost    = peakDb < refDb - 14.0f;
                hit.shape    = shape;

                if (! refFixed && peakDb > refDb)
                    refDb += 0.5f * (peakDb - refDb);
                if (atkPeakDb > atkRefDb)
                    atkRefDb += 0.5f * (atkPeakDb - atkRefDb);

                // learn the fingerprint from confident hits; restart if the reference jumped (louder source found)
                if (! templateFixed && peakDb >= refDb - 6.0f)
                {
                    if (refDb > tmpl.learnedAtRefDb + 6.0f) tmpl = {};
                    const float n = (float) std::min (tmpl.count, 31);
                    for (int i = 0; i < SpectralTemplate::kBands; ++i)
                        tmpl.shape[(size_t) i] = (tmpl.shape[(size_t) i] * n + shape[(size_t) i]) / (n + 1.0f);
                    ++tmpl.count;
                    tmpl.learnedAtRefDb = std::max (tmpl.learnedAtRefDb, refDb);
                }

                lastOnset = onset;
                lastHitPeakDb = peakDb;
                rearmed = false;
                emitted = true;
            }
            else
            {
                cooldownUntil = pos + velWindow / 2;
            }
        }
    }

    ++pos;
    return emitted;
}

} // namespace thruma
