// Real-time / offline drum transient detector.
//
// Two-band design per instrument profile:
//   * an "attack" band (beater click / stick crack) whose envelope rise finds the onset quickly,
//   * a "body" band (fundamental / shell) which gives level, velocity and confirms the hit.
// A candidate is confirmed only after a short velocity window, which allows false-trigger
// checks (body-band rise, frequency focus, adaptive thresholds) and sub-millisecond onset
// positioning by backtracking. The detector therefore reports hits with a fixed maximum delay
// (getMaxReportDelay()); the host-facing processor compensates with an equal lookahead.
//
// The same class is used for live triggering and for offline analysis of captured audio,
// so both paths produce identical trigger positions.
#pragma once

#include "DspUtils.h"
#include "Instruments.h"

#include <array>
#include <cstdint>
#include <limits>

namespace thruma
{

struct DetectorSettings
{
    Instrument instrument   = Instrument::Snare;
    float sensitivity       = 0.5f;   // 0..1 main sensitivity
    float ghostSensitivity  = 0.35f;  // 0..1 extends the threshold range downwards for quiet hits
    float retriggerMs       = 0.0f;   // minimum retrigger interval, 0 = profile default
    bool  doubleHits        = true;   // allow re-trigger on a fresh attack before the previous hit decayed
    bool  frequencyFocus    = true;   // reject hits whose energy is mostly outside the body band
    float dynamicRangeDb    = 40.0f;  // level range mapped to velocity 0..1
    float maxReportDelayMs  = 10.0f;  // hard upper bound on report delay (lookahead budget)
    float amount            = 0.5f;   // 0..1 how easily a possible hit is noticed (attack rise needed); 0.5 = neutral
    float match             = 0.5f;   // 0..1 how much it must sound like this drum rather than bleed; 0.5 = neutral
};

/** Why a possible hit was not used (shown as a dim trigger point). */
enum class RejectReason : uint8_t { None = 0, TooQuiet = 1, NoAttack = 2, Bleed = 3, TooSoon = 4 };

struct Rejection
{
    int64_t position = 0;
    float   peakDb = -150.0f;
    float   thresholdDb = -150.0f;
    float   velocity = 0.0f;
    RejectReason reason = RejectReason::None;
};

/** Coarse spectral fingerprint of the target drum (band levels relative to the loudest band). */
struct SpectralTemplate
{
    static constexpr int kBands = 8;
    std::array<float, kBands> shape {};
    int count = 0;
    float learnedAtRefDb = -150.0f;
    bool isValid() const noexcept { return count >= 2; }
};

struct DetectedHit
{
    int64_t position = 0;   // onset, absolute sample index
    float   velocity = 0;   // 0..1
    float   peakDb   = -150; // body-band peak level
    bool    ghost    = false;
    std::array<float, SpectralTemplate::kBands> shape {};  // normalised band levels of this hit
};

class TransientDetector
{
public:
    void prepare (double sampleRate);
    void setSettings (const DetectorSettings& s) noexcept; // real-time safe
    const DetectorSettings& getSettings() const noexcept { return settings; }
    void reset (int64_t startPosition = 0) noexcept;

    /** Fix the velocity reference level (dBFS of a "full" hit); NaN/<= -150 re-enables adaptive tracking. */
    void setVelocityReference (float refDb) noexcept;
    float getVelocityReference() const noexcept { return refDb; }

    /** Start values for adaptive tracking (learning continues afterwards). */
    void seedVelocityReference (float refDb) noexcept;
    void seedSpectralTemplate (const SpectralTemplate& t) noexcept;

    /** Fix the spectral fingerprint (e.g. learned by offline analysis); an invalid template re-enables learning. */
    void setSpectralTemplate (const SpectralTemplate& t) noexcept;
    const SpectralTemplate& getSpectralTemplate() const noexcept { return tmpl; }

    /** Distance (dB RMS) between a hit shape and a template. */
    static float shapeDistance (const std::array<float, SpectralTemplate::kBands>& a, const SpectralTemplate& t) noexcept;
    /** The same after removing the overall spectral tilt (a softer, darker hit of the same drum matches). */
    static float shapeDistanceNoTilt (const std::array<float, SpectralTemplate::kBands>& a, const SpectralTemplate& t) noexcept;

    /** Level of bleed-like events relative to the hits (dB, < 0), e.g. measured over a whole take; NaN = learn it. */
    void setBleedLevel (float relDb) noexcept;
    float getBleedLevel() const noexcept { return bleedKnown() ? bleedRelDb : std::numeric_limits<float>::quiet_NaN(); }

    /** Maximum number of samples between an onset and the moment it is reported. */
    int getMaxReportDelay() const noexcept { return backtrack + velWindow + 1; }

    int64_t getPosition() const noexcept { return pos; }

    // --- metering for the live display (read on the audio thread after processSample) ---
    float getLevelDb() const noexcept { return lastBodyDb; }      // body-band envelope (what is compared to the threshold)
    float getThresholdDb() const noexcept { return thresholdNow(); }
    float getFloorDb() const noexcept { return bodyFloorDb; }
    /** True once after a possible hit near the threshold was not used (with the reason). */
    bool takeRejection (Rejection& r) noexcept { if (! hasRejection) return false; r = rejection; hasRejection = false; return true; }

    template <typename OnHit>
    void process (const float* x, int n, OnHit&& onHit) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            DetectedHit h;
            if (processSample (x[i], h))
                onHit (h);
        }
    }

    /** Process one sample; returns true and fills 'hit' when a hit is confirmed. */
    bool processSample (float x, DetectedHit& hit) noexcept;

private:
    void updateDerived() noexcept;
    float thresholdNow() const noexcept;
    float velocityFor (float peakDb) const noexcept;

    DetectorSettings settings;
    double sr = 48000.0;
    bool prepared = false;

    // filters
    Biquad bodyHp1, bodyHp2, bodyLp, atkHp1, atkHp2, atkLp, posHp, fullHp;
    SlidingMax bodyEnv, atkEnv, fullEnv, posEnv;
    DelayLine bodyDbHist, atkDbHist, posEnvRing;

    // derived (samples / dB)
    int fastWindow = 1, holdBody = 1, holdAtk = 1, riseWin = 1, velWindow = 1, backtrack = 1, retrig = 1, posHold = 1;
    float riseThrDb = 8, riseFastDb = 6, mainRiseMinDb = 1, floorMarginDb = 8, relRangeDb = 36, ghostRelDb = 20, focusMinDb = -14;
    float refDecayPerSample = 0;
    bool hasBodyLp = false, hasAtkLp = false;

    // spectral fingerprint
    std::array<Biquad, SpectralTemplate::kBands> specBp;
    std::array<double, SpectralTemplate::kBands> specAcc {};
    int specCount = 0;
    SpectralTemplate tmpl;
    bool templateFixed = false;
    float specThrDb = 7.0f;

    // minimum-statistics floor (bleed level between hits)
    static constexpr int kFloorBlocks = 20;
    std::array<float, kFloorBlocks> bodyBlockMin {}, atkBlockMin {};
    float bodyCurMin = 0, atkCurMin = 0;
    int floorBlockLen = 960, floorBlockPos = 0, floorBlockIdx = 0;

    // running state
    int64_t pos = 0;
    float bodyFloorDb = -120, atkFloorDb = -120;
    float refDb = -12;
    bool refFixed = false;

    enum class State { Idle, Candidate } state = State::Idle;
    int64_t candStart = 0, lastOnset = -1000000000LL, cooldownUntil = 0;
    float candPeakBody = 0, candPeakFull = 0, candPeakPos = 0, candLevelBeforeDb = -150;
    float candPeakAtk = 0, candAtkBeforeDb = -150;
    int64_t candPeakPosIdx = 0;
    float lastHitPeakDb = -150;
    float atkRefDb = -12.0f;
    bool atkRefSet = false;
    static constexpr int kRecent = 12;
    std::array<float, kRecent> recentPeaks {};
    int recentCount = 0;
    int64_t firstHitPos = -1, establishAfter = 72000;
    float maxAtkSoFar = -150.0f, maxPeakSoFar = -150.0f;     // adaptive level of the attack band at real hits
    float bodySmoothDb = -180.0f, smoothCoeff = 0.99f;
    float lastBodyDb = -180.0f;
    Rejection rejection;
    bool hasRejection = false;
    int64_t tooSoonReportedFor = -1;   // onset of the hit whose retrigger window already reported a TooSoon
    float trigFloorMarginDb = 8;
    // bleed-like events on this track, relative to the hits
    float bleedRelDb = 0.0f;
    int bleedCount = 0;
    bool bleedFixed = false;
    static constexpr int kBleedBins = 60;   // 1 dB bins below the hits
    std::array<int, kBleedBins> bleedHist {};
    bool bleedKnown() const noexcept { return bleedCount >= 8; }
    std::array<float, SpectralTemplate::kBands> lastHitShape {};
    bool lastHitShapeValid = false;
    float lastHitAtkDb = -150.0f;
    bool rearmed = true;
};

} // namespace thruma
