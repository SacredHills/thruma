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

void TransientDetector::seedVelocityReference (float r) noexcept
{
    if (std::isfinite (r) && r > -150.0f && ! refFixed) refDb = r;
}

void TransientDetector::seedSpectralTemplate (const SpectralTemplate& t) noexcept
{
    if (t.isValid() && ! templateFixed) { tmpl = t; tmpl.count = std::min (t.count, 8); }
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

float TransientDetector::shapeDistanceNoTilt (const std::array<float, SpectralTemplate::kBands>& a, const SpectralTemplate& t) noexcept
{
    // remove the best-fitting straight line (overall brightness) from the difference, then measure what is left
    constexpr int N = SpectralTemplate::kBands;
    std::array<float, N> d;
    float mean = 0.0f;
    for (int i = 0; i < N; ++i) { d[(size_t) i] = std::clamp (a[(size_t) i] - t.shape[(size_t) i], -20.0f, 20.0f); mean += d[(size_t) i]; }
    mean /= (float) N;
    const float mx = 0.5f * (float) (N - 1);
    float sxy = 0.0f, sxx = 0.0f;
    for (int i = 0; i < N; ++i) { sxy += ((float) i - mx) * (d[(size_t) i] - mean); sxx += ((float) i - mx) * ((float) i - mx); }
    const float slope = sxy / sxx;
    double sum = 0.0;
    for (int i = 0; i < N; ++i) { const double r = d[(size_t) i] - mean - slope * ((float) i - mx); sum += r * r; }
    return (float) std::sqrt (sum / N);
}

void TransientDetector::setBleedLevel (float relDb) noexcept
{
    if (std::isfinite (relDb) && relDb < 0.0f) { bleedRelDb = relDb; bleedCount = 1000; bleedFixed = true; }
    else { bleedFixed = false; bleedCount = 0; bleedHist.fill (0); }
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

    // Amount shifts how easily a possible hit is noticed, Match how drum-like it must sound; both neutral at 50 %.
    // Sensitivity is the master (threshold line, and how strict the other checks are); Amount and Match shift
    // those other checks from there.
    const float sA = std::clamp (s + (std::clamp (settings.amount, 0.0f, 1.0f) - 0.5f) * 0.8f, 0.0f, 0.95f);
    const float sM = std::clamp (s - (std::clamp (settings.match, 0.0f, 1.0f) - 0.5f) * 0.8f, 0.0f, 1.2f);

    riseThrDb     = lerp (14.0f, 4.0f, sA);
    riseFastDb    = lerp (9.0f, 3.0f, sA);    // right after real hits (fast playing) a smaller fresh attack suffices
    fastWindow    = msToSamples (160.0, sr);
    mainRiseMinDb = lerp (3.0f, 2.0f, sA);
    floorMarginDb = lerp (12.0f, 4.0f, s);
    trigFloorMarginDb = lerp (12.0f, 4.0f, sA);
    relRangeDb    = std::max (6.0f, p.levelRangeDb + (s - 0.4f) * 50.0f);   // Sensitivity 0 % -> threshold just below the loudest hits
    ghostRelDb    = settings.ghostSensitivity * 18.0f * p.ghostWeight;
    focusMinDb    = p.focusMinDb - sM * 6.0f;
    specThrDb     = lerp (2.0f, 6.0f, sM);

    refDecayPerSample = (float) (0.05 / sr);  // adaptive reference falls very slowly (breaks must not lower it)
    smoothCoeff = onePoleCoeff (0.010 * sr);
    establishAfter = (int64_t) (1.5 * sr);
    staleAfter = (int64_t) (8.0 * sr);
}

float TransientDetector::thresholdNow() const noexcept
{
    // The floor term protects against noise/bleed, but in fast playing the measured "floor" is the drum's own
    // ring between hits, so it may never climb to within 10 dB of the real hits.
    const bool playingFast = (pos - lastOnset) < fastWindow;
    const float floorTerm = playingFast ? std::min (bodyFloorDb + floorMarginDb, refDb - 10.0f) : bodyFloorDb + floorMarginDb;
    return std::max (floorTerm, refDb - relRangeDb - ghostRelDb);
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
    atkRefDb = -12.0f;    // until the first confident hit sets it
    atkRefSet = false;
    recentCount = 0;
    firstHitPos = -1;
    lastConfidentPos = -1;
    if (! templateFixed) tmpl = {};
    state = State::Idle;
    lastOnset = startPosition - 1000000000LL;
    cooldownUntil = startPosition;
    rearmed = true;
    lastHitPeakDb = -150.0f;
    bodySmoothDb = -180.0f;
    lastHitShapeValid = false;
    lastHitAtkDb = -150.0f;
    hasRejection = false;
    tooSoonReportedFor = -1;
    if (! bleedFixed) { bleedCount = 0; bleedRelDb = 0.0f; bleedHist.fill (0); }
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
    lastBodyDb = bodyDb;
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
    atkRefDb = std::max (-100.0f, atkRefDb - 20.0f * refDecayPerSample);   // 1 dB/s: tracks the current playing intensity

    // smoothed level: robust against momentary dips (beating modes, noisy wires) for re-arming
    bodySmoothDb = bodyDb + (bodySmoothDb - bodyDb) * smoothCoeff;
    if (! rearmed && bodySmoothDb < lastHitPeakDb - 8.0f)
        rearmed = true;

    bool emitted = false;

    if (state == State::Idle)
    {
        const bool timeOk = (pos - lastOnset) >= retrig && pos >= cooldownUntil;
        if (! timeOk && pos >= cooldownUntil && tooSoonReportedFor != lastOnset && (pos - lastOnset) > velWindow
            && atkRise >= riseThrDb + 3.0f && atkDb >= atkFloorDb + trigFloorMarginDb && bodyDb >= thresholdNow())
        {
            // a clear fresh attack inside the retrigger window of the last hit: report it once
            tooSoonReportedFor = lastOnset;
            rejection = { pos, bodyDb, thresholdNow(), velocityFor (bodyDb), RejectReason::TooSoon };
            hasRejection = true;
        }
        if (timeOk)
        {
            // fast playing: a re-hit on a still-ringing drum shows a smaller fresh rise. Once the drum has decayed
            // (re-armed) the normal rise is required again, so noise in a decaying tail cannot start a candidate.
            const float riseThrDb = (! rearmed && (pos - lastOnset) < fastWindow) ? riseFastDb : this->riseThrDb;
            const bool trigA = atkRise >= riseThrDb && atkDb >= atkFloorDb + trigFloorMarginDb;
            const bool trigB = bodyRise >= riseThrDb + 3.0f && bodyDb >= bodyFloorDb + trigFloorMarginDb;
            const bool rearmOk = rearmed
                              || (settings.doubleHits && (atkRise >= riseThrDb || bodyRise >= riseThrDb + 3.0f));
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
        // a much stronger attack inside the window means the candidate started early (on bleed): restart at the real hit
        if (atkE > candPeakAtk * 2.0f && atkRise >= riseFastDb && pos - candStart >= 2)
        {
            candStart = pos;
            candPeakBody = bodyE;
            candPeakFull = fullE;
            candPeakPos = std::abs (xp);
            candPeakPosIdx = pos;
            candPeakAtk = atkE;
            candLevelBeforeDb = std::max (bodyDbHist.get (std::min (riseWin + 2, bodyDbHist.capacity() - 1)), bodySmoothDb);
            candAtkBeforeDb = atkDbHist.get (std::min (riseWin + 2, atkDbHist.capacity() - 1));
            specAcc.fill (0.0);
            specCount = 0;
        }
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
            // No confident hit yet, 1.5 s after the first one: the track is simply quieter than the starting
            // reference assumes. Adopt the levels actually heard (a real drum after early bleed comes much sooner).
            if (! atkRefSet && firstHitPos >= 0 && pos - firstHitPos > establishAfter)
            {
                atkRefDb = maxAtkSoFar;
                atkRefSet = true;
                if (! refFixed && maxPeakSoFar < refDb - 6.0f) refDb = maxPeakSoFar;
            }
            const float peakDb = fastDb (candPeakBody);
            const float fullDb = fastDb (candPeakFull);
            const float absThr = thresholdNow();
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
            // a very strong fresh attack (20+ dB out of the previous hit's tail) is a new hit even when the body
            // hardly rises, e.g. a compressor before Thruma flattened the body and holds up the tail
            const float atkRefMargin = atkRise >= riseThrDb + 14.0f ? 12.0f : 6.0f;
            const bool riseOk = bodyRiseOk
                             || (atkRise >= 0.6f * riseThrDb && peakDb >= lastHitPeakDb - 10.0f && atkPeakDb >= atkRefDb - atkRefMargin
                                 && atkPeakDb >= lastHitAtkDb - 8.0f);
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
            bool soundsLikeDrum = true;   // after allowing for a softer (darker) hit
            if (! confident && settings.frequencyFocus && tmpl.isValid())
            {
                dist = shapeDistance (shape, tmpl);
                // the quieter the candidate (closer to the bleed level), the closer it must sound like the drum
                const float depth = std::clamp ((refDb - peakDb) / std::max (6.0f, relRangeDb + ghostRelDb), 0.0f, 1.0f);
                // candidates accepted under the looser fast-playing rules must sound even more like the drum
                const float fastPenalty = (! rearmed && (candStart - lastOnset) < fastWindow) ? 0.75f : 1.0f;
                shapeOk = dist <= specThrDb * (1.0f - 0.45f * depth) * fastPenalty;
                // Clearly louder than the loud bleed on this track: it cannot be bleed. Softer hits of the drum
                // sound darker than the loud ones the fingerprint was learned from, so compare without the overall
                // tilt and without the quieter-must-match-closer penalty.
                soundsLikeDrum = shapeDistanceNoTilt (shape, tmpl) <= specThrDb * fastPenalty;
                if (! shapeOk && bleedKnown() && peakDb - refDb >= bleedRelDb + 5.0f)
                    shapeOk = soundsLikeDrum;
            }

            state = State::Idle;
#ifdef THRUMA_DETECTOR_DEBUG
            std::printf ("atk rise=%.1f vsLast=%.1f vsRef=%.1f rearmed=%d bodyRiseOk=%d sinceLast=%.1fms\n", atkRise, atkPeakDb - lastHitAtkDb, atkPeakDb - atkRefDb, (int) rearmed, (int) bodyRiseOk, (candStart - lastOnset) * 1000.0 / sr);
            std::printf ("cand %lld peak=%.1f before=%.1f full=%.1f absThr=%.1f floor=%.1f ref=%.1f dist=%.1f -> %d%d%d%d  [%.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f]\n",
                         (long long) candStart, peakDb, candLevelBeforeDb, fullDb, absThr, bodyFloorDb, refDb, dist,
                         levelOk, riseOk, focusOk, shapeOk, shape[0], shape[1], shape[2], shape[3], shape[4], shape[5], shape[6], shape[7]);
#endif

            if (levelOk && riseOk && focusOk && shapeOk)
            {
                // --- sub-ms onset positioning: walk back from the peak to the first threshold crossing ---
                const int64_t limit = std::max (candStart - backtrack, pos - (int64_t) posEnvRing.capacity() + 2);
                // threshold between the level just before the hit (bleed / previous ring) and the hit's peak
                float residual = posEnvRing.get ((int) (pos - limit));
                for (int64_t k = limit; k < candStart; ++k) residual = std::min (residual, posEnvRing.get ((int) (pos - k)));
                const float thr = residual + 0.2f * std::max (0.0f, candPeakPos - residual);
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
                lastHitShape = shape;
                lastHitShapeValid = true;

                if (peakDb >= refDb - 10.0f) lastConfidentPos = onset;   // a hit near (or above) the reference level
                if (! refFixed)
                {
                    // robust running reference: limited step up (a single freak peak cannot raise the threshold
                    // for minutes) and a slow step down when the playing gets quieter
                    // ...except a hit far louder than the reference: the reference was sitting at bleed level
                    // (a tom mic before the first real tom hit), so catch up at once
                    if (peakDb > refDb + 10.0f) refDb += 0.75f * (peakDb - refDb);
                    else if (peakDb > refDb) refDb += std::min (3.0f, 0.5f * (peakDb - refDb));
                    else if (peakDb > refDb - 12.0f) refDb -= 0.03f * (refDb - peakDb);
                    // When even the loudest of the last hits is well below the reference, the reference is wrong
                    // (a quieter track, or a plugin before Thruma turned it down): follow the real level within
                    // a few hits instead of minutes. Ghost notes alone never do this, the backbeat keeps it up.
                    recentPeaks[(size_t) (recentCount++ % kRecent)] = peakDb;
                    if (recentCount >= kRecent && referenceStale())
                    {
                        const float loudest = *std::max_element (recentPeaks.begin(), recentPeaks.end());
                        if (loudest < refDb - 3.0f) refDb = std::max (loudest, refDb - 6.0f);
                    }
                }
                // attack level of real hits, used while a drum keeps ringing (fast double bass never decays enough
                // to re-arm). Taken from the first confident hit, so it does not depend on the track's absolute level.
                if (firstHitPos < 0) { firstHitPos = onset; maxAtkSoFar = atkPeakDb; maxPeakSoFar = peakDb; }
                maxAtkSoFar = std::max (maxAtkSoFar, atkPeakDb);
                maxPeakSoFar = std::max (maxPeakSoFar, peakDb);
                const bool confidentHit = peakDb >= refDb - 6.0f;
                if (! atkRefSet && confidentHit) { atkRefDb = atkPeakDb; atkRefSet = true; }
                else if (atkPeakDb > atkRefDb) atkRefDb += 0.5f * (atkPeakDb - atkRefDb);
                lastHitAtkDb = atkPeakDb;

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
                // A clear, drum-like hit that failed only because it is quieter than the reference: the reference
                // is too high (a quieter track, or a plugin before Thruma turned the level down). Pull it down
                // quickly while such hits keep coming, instead of waiting minutes for the slow decay.
                // (only while no hit at the reference level has come for a while: on a tom mic the toms are sparse
                // and the bleed in between must not drag the reference down to its own level)
                if (! refFixed && ! levelOk && riseOk && focusOk && shapeOk && referenceStale()
                    && peakDb >= bodyFloorDb + floorMarginDb + 6.0f && peakDb >= -90.0f)
                    refDb = std::max (peakDb, refDb - std::min (6.0f, 0.5f * (absThr - peakDb) + 1.5f));
                cooldownUntil = pos + velWindow / 2;
                // how loud bleed-like events are on this track (relative to the hits): everything that started like
                // a hit but was not one. A slow average, robust to the odd rejected real hit.
                // Only candidates that start once the drum has decayed (its own ring between fast hits is not bleed);
                // a high quantile of them (the loud bleed is what matters), robust to the odd rejected real hit.
                // (a rejected candidate that does sound like a softer hit of the drum is not counted as bleed)
                if (! bleedFixed && rearmed && peakDb > bodyFloorDb + 3.0f && ! (focusOk && soundsLikeDrum && levelOk))
                {
                    const int bin = std::clamp ((int) std::floor (refDb - peakDb), 0, kBleedBins - 1);
                    ++bleedHist[(size_t) bin];
                    if (++bleedCount > 600) { bleedCount = 0; for (auto& h : bleedHist) { h /= 2; bleedCount += h; } }   // slowly forget
                    // the level the loudest quarter of them reaches (bins run from loud to quiet)
                    int acc = 0, q = 0;
                    for (int b = 0; b < kBleedBins; ++b) { acc += bleedHist[(size_t) b]; if (acc * 4 >= bleedCount) { q = b; break; } }
                    bleedRelDb = -((float) q + 0.5f);
                }
                // near misses become dim trigger points (with the reason), so they can be checked and added
                if (peakDb >= absThr - 18.0f && peakDb >= bodyFloorDb + floorMarginDb * 0.5f)
                {
                    const auto why = ! levelOk ? RejectReason::TooQuiet : ! riseOk ? RejectReason::NoAttack : RejectReason::Bleed;
                    rejection = { candStart, peakDb, absThr, velocityFor (peakDb), why };
                    hasRejection = true;
                }
            }
        }
    }

    ++pos;
    return emitted;
}

} // namespace thruma
