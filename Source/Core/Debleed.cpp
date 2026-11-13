#include "Debleed.h"
#include "Triggers.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace thruma
{

namespace
{
    constexpr float kEps = 1.0e-12f;

    std::vector<float> makeSqrtHann (int n)
    {
        std::vector<float> w ((size_t) n);
        for (int i = 0; i < n; ++i)
            w[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * kPi * i / n));
        return w;
    }
}

int debleedFftSizeFor (double sampleRate) noexcept
{
    if (sampleRate <= 50000.0)  return 1024;
    if (sampleRate <= 100000.0) return 2048;
    return 4096;
}

DebleedModel DebleedModel::remapped (double newRate, int newFftSize) const
{
    if (matches (newRate, newFftSize) || fftSize == 0) return *this;

    DebleedModel m;
    m.sampleRate = newRate;
    m.fftSize = newFftSize;
    m.velocityRefDb = velocityRefDb;
    m.targetTemplate = targetTemplate;
    m.hasTransfer = hasTransfer;
    const int newBins = newFftSize / 2 + 1;
    const int oldBins = getNumBins();

    auto remap = [&] (const std::vector<float>& src, float powerScale)
    {
        std::vector<float> dst ((size_t) newBins, 0.0f);
        if ((int) src.size() != oldBins) return dst;
        for (int k = 0; k < newBins; ++k)
        {
            const double hz = (double) k * newRate / newFftSize;
            const double pos = hz * fftSize / sampleRate;
            const int i = (int) std::floor (pos);
            const double f = pos - i;
            float v;
            if (i >= oldBins - 1) v = src.back();
            else v = (float) (src[(size_t) i] * (1.0 - f) + src[(size_t) i + 1] * f);
            dst[(size_t) k] = v * powerScale;
        }
        return dst;
    };

    // Bleed is broadband/noise-like: its power per bin scales with the FFT length.
    const float floorScale = (float) newFftSize / (float) fftSize;
    m.transfer   = remap (transfer, 1.0f);
    m.bleedFloor = remap (bleedFloor, floorScale);
    m.decayTau   = remap (decayTau, 1.0f);
    return m;
}

//==============================================================================
void DebleedProcessor::prepare (double sampleRate, int numChannels)
{
    sr = sampleRate;
    channels = std::clamp (numChannels, 1, kMaxChannels);
    fftSize = debleedFftSizeFor (sr);
    hop = fftSize / 4;
    numBins = fftSize / 2 + 1;

    fft.prepare (fftSize);
    window = makeSqrtHann (fftSize);
    for (int c = 0; c < kMaxChannels; ++c)
    {
        inFifo[(size_t) c].assign ((size_t) fftSize, 0.0f);
        outAccum[(size_t) c].assign ((size_t) fftSize, 0.0f);
        outFifo[(size_t) c].assign ((size_t) hop, 0.0f);
        spec[(size_t) c].assign ((size_t) numBins, {});
    }
    refFifo.assign ((size_t) fftSize, 0.0f);
    frame.assign ((size_t) fftSize, 0.0f);
    refSpec.assign ((size_t) numBins, {});

    pxr.assign ((size_t) numBins, {});
    for (auto* v : { &prr, &bleedFloor, &targetEnv, &bleedSmooth, &gainSmooth, &prevClean, &tau, &decayPerFrame,
                     &gainRaw, &gainTmp })
        v->assign ((size_t) numBins, 0.0f);
    confidence.assign ((size_t) numBins, 0.0f);
    modelTransfer.assign ((size_t) numBins, 0.0f);
    prefix.assign ((size_t) numBins + 1, 0.0f);

    detector.prepare (sr);
    prepared = true;

    // meter bands: log spaced 30 Hz .. min(20 kHz, Nyquist)
    const double lo = 30.0, hi = std::min (20000.0, sr * 0.5);
    for (int b = 0; b <= kMeterBands; ++b)
    {
        const double hz = lo * std::pow (hi / lo, (double) b / kMeterBands);
        meterEdges[(size_t) b] = std::clamp ((int) std::lround (hz * fftSize / sr), 1, numBins - 1);
    }

    setSettings (settings);
    reset();
}

float DebleedProcessor::getMeterBandHz (int band) const noexcept
{
    const double lo = 30.0, hi = std::min (20000.0, sr * 0.5);
    return (float) (lo * std::pow (hi / lo, (band + 0.5) / kMeterBands));
}

float DebleedProcessor::defaultTau (int bin) const noexcept
{
    const double hz = std::max (20.0, (double) bin * sr / fftSize);
    const double tau0 = getProfile (settings.instrument).decayMs * 0.001;
    return (float) (tau0 * std::clamp (std::sqrt (500.0 / hz), 0.5, 1.5));
}

void DebleedProcessor::setSettings (const DebleedSettings& s) noexcept
{
    const bool tauChanged = s.instrument != settings.instrument || s.decayScale != settings.decayScale;
    settings = s;
    settings.amount         = std::clamp (settings.amount, 0.0f, 1.0f);
    settings.attackPreserve = std::clamp (settings.attackPreserve, 0.0f, 1.0f);
    settings.releaseMs      = std::clamp (settings.releaseMs, 5.0f, 3000.0f);
    settings.maxReductionDb = std::clamp (settings.maxReductionDb, 0.0f, 80.0f);
    settings.freqSmoothing  = std::clamp (settings.freqSmoothing, 0.0f, 1.0f);
    settings.decayScale     = std::clamp (settings.decayScale, 0.1f, 10.0f);

    DetectorSettings ds;
    ds.instrument = settings.instrument;
    ds.sensitivity = settings.sensitivity;
    ds.ghostSensitivity = std::clamp (settings.sensitivity * 0.9f, 0.0f, 1.0f);
    ds.doubleHits = true;
    ds.maxReportDelayMs = 10.0f;
    detector.setSettings (ds);

    if (! tau.empty() && (tauChanged || ! modelHasTau))
    {
        for (int k = 0; k < numBins; ++k)
        {
            const float t = (modelHasTau ? tau[(size_t) k] : defaultTau (k));
            if (! modelHasTau) tau[(size_t) k] = t;
            decayPerFrame[(size_t) k] = (float) std::exp (-2.0 * hop / (std::max (0.005f, t * settings.decayScale) * sr));
        }
    }
}

void DebleedProcessor::adoptModel (const DebleedModel& m) noexcept
{
    if (! m.matches (sr, fftSize) || (int) m.decayTau.size() != numBins) return;

    modelHasTau = true;
    modelHasTransfer = m.hasTransfer && (int) m.transfer.size() == numBins;
    for (int k = 0; k < numBins; ++k)
    {
        tau[(size_t) k] = std::max (0.005f, m.decayTau[(size_t) k]);
        decayPerFrame[(size_t) k] = (float) std::exp (-2.0 * hop / (tau[(size_t) k] * settings.decayScale * sr));
        if ((int) m.bleedFloor.size() == numBins) bleedFloor[(size_t) k] = m.bleedFloor[(size_t) k];
        modelTransfer[(size_t) k] = modelHasTransfer ? m.transfer[(size_t) k] : 0.0f;
    }
    if (m.velocityRefDb > -150.0f) detector.setVelocityReference (m.velocityRefDb);
    if (m.targetTemplate.isValid()) detector.setSpectralTemplate (m.targetTemplate);
}

void DebleedProcessor::reset() noexcept
{
    if (! prepared) return;
    for (int c = 0; c < kMaxChannels; ++c)
    {
        std::fill (inFifo[(size_t) c].begin(), inFifo[(size_t) c].end(), 0.0f);
        std::fill (outAccum[(size_t) c].begin(), outAccum[(size_t) c].end(), 0.0f);
        std::fill (outFifo[(size_t) c].begin(), outFifo[(size_t) c].end(), 0.0f);
    }
    std::fill (refFifo.begin(), refFifo.end(), 0.0f);
    std::fill (pxr.begin(), pxr.end(), std::complex<float> {});
    std::fill (prr.begin(), prr.end(), 0.0f);
    std::fill (confidence.begin(), confidence.end(), 0.0f);
    std::fill (targetEnv.begin(), targetEnv.end(), 0.0f);
    std::fill (bleedSmooth.begin(), bleedSmooth.end(), 0.0f);
    std::fill (gainSmooth.begin(), gainSmooth.end(), 1.0f);
    std::fill (prevClean.begin(), prevClean.end(), 0.0f);
    hopCount = 0;
    samplePos = 0;
    onsetWrite = onsetCount = 0;
    detector.reset (0);
    for (auto& m : meter) m.store (0.0f);
}

void DebleedProcessor::process (float* const* io, int numCh, const float* ref, int n) noexcept
{
    if (! prepared) return;
    numCh = std::clamp (numCh, 1, channels);
    const float monoScale = 1.0f / (float) numCh;

    for (int i = 0; i < n; ++i)
    {
        float mono = 0.0f;
        for (int c = 0; c < numCh; ++c) mono += io[c][i];
        mono *= monoScale;

        DetectedHit hit;
        if (detector.processSample (mono, hit))
        {
            onsets[(size_t) onsetWrite] = hit.position;
            onsetWrite = (onsetWrite + 1) % kMaxOnsets;
            onsetCount = std::min (onsetCount + 1, kMaxOnsets);
        }

        const int w = fftSize - hop + hopCount;
        for (int c = 0; c < numCh; ++c)
        {
            inFifo[(size_t) c][(size_t) w] = io[c][i];
            io[c][i] = outFifo[(size_t) c][(size_t) hopCount];
        }
        refFifo[(size_t) w] = ref != nullptr ? ref[i] : 0.0f;

        ++samplePos;
        if (++hopCount == hop)
        {
            hopCount = 0;
            activeChannels = numCh;
            processFrame (ref != nullptr);
        }
    }
}

void DebleedProcessor::processFrame (bool hasRef) noexcept
{
    const int nch = activeChannels;
    const bool useRef = hasRef && settings.useReference;

    for (int c = 0; c < nch; ++c)
    {
        const auto& in = inFifo[(size_t) c];
        for (int i = 0; i < fftSize; ++i) frame[(size_t) i] = in[(size_t) i] * window[(size_t) i];
        fft.forward (frame.data(), spec[(size_t) c].data());
    }
    if (useRef)
    {
        for (int i = 0; i < fftSize; ++i) frame[(size_t) i] = refFifo[(size_t) i] * window[(size_t) i];
        fft.forward (frame.data(), refSpec.data());
    }

    // --- target activity from the (look-ahead) detector ---
    const int64_t centre = samplePos - fftSize / 2;
    const double attackLen = (0.005 + 0.060 * settings.attackPreserve) * sr;
    const float protectStrength = std::min (1.0f, settings.attackPreserve * 1.5f);
    bool hitFrame = false;
    float protect = 0.0f;
    for (int j = 0; j < onsetCount; ++j)
    {
        const auto o = onsets[(size_t) j];
        const double d = (double) (centre - o);
        if (d >= -fftSize / 4 && d <= fftSize / 4) hitFrame = true;
        if (d >= -fftSize / 2 && d <= attackLen)
        {
            const double r = d <= 0.0 ? 0.0 : d / attackLen;
            protect = std::max (protect, (float) ((1.0 - r * r) * protectStrength));
        }
    }

    const float a      = 0.5f + 2.5f * settings.amount;              // over-subtraction
    const float gmin   = dbToGain (-settings.maxReductionDb * settings.amount);
    const float frameS = (float) hop / (float) sr;
    const float beta   = 1.0f - std::exp (-frameS / 1.5f);            // transfer learning rate
    const float betaB  = 1.0f - std::exp (-frameS / 3.0f);            // bleed floor learning rate
    const float decB   = std::exp (-frameS / 0.06f);                  // bleed estimate release
    const float relC   = std::exp (-frameS / (settings.releaseMs * 0.001f));
    const int lowProtectBin = (int) std::ceil (settings.lowProtectHz * fftSize / sr);

    for (int k = 0; k < numBins; ++k)
    {
        float P = 0.0f;
        for (int c = 0; c < nch; ++c) P += std::norm (spec[(size_t) c][(size_t) k]);
        P /= (float) nch;

        const float Pr = useRef ? std::norm (refSpec[(size_t) k]) : 0.0f;

        // reference-predicted bleed (from the previous estimate of the transfer)
        float bref = 0.0f;
        if (useRef)
        {
            const float measured = std::min (4.0f, std::norm (pxr[(size_t) k]) / (prr[(size_t) k] * prr[(size_t) k] + kEps));
            const float c = settings.adaptive ? confidence[(size_t) k] : 0.0f;
            const float G2 = modelHasTransfer ? c * measured + (1.0f - c) * modelTransfer[(size_t) k] : measured;
            bref = G2 * Pr;
        }

        // target envelope model: capture at hits, decay per bin with the drum's decay time
        float E = targetEnv[(size_t) k] * decayPerFrame[(size_t) k];
        if (hitFrame) E = std::max (E, P - bref);
        targetEnv[(size_t) k] = E;

        // learning weight: how bleed-dominated this bin currently is
        float wgt = (hitFrame || protect > 0.5f) ? 0.0f : std::clamp (1.0f - E / (P + kEps), 0.0f, 1.0f);
        wgt *= wgt;
        if (settings.adaptive && wgt > 0.0f)
        {
            if (useRef)
            {
                const float b = beta * wgt;
                pxr[(size_t) k] += b * (spec[0][(size_t) k] * std::conj (refSpec[(size_t) k]) - pxr[(size_t) k]);
                prr[(size_t) k] += b * (Pr - prr[(size_t) k]);
                confidence[(size_t) k] += b * (1.0f - confidence[(size_t) k]);
            }
            bleedFloor[(size_t) k] += betaB * wgt * (P - bleedFloor[(size_t) k]);
        }

        // bleed power estimate
        const float excess = std::max (P - E, 0.0f);
        float B;
        if (useRef)
        {
            const float bs = std::max (bref, bleedSmooth[(size_t) k] * decB);
            bleedSmooth[(size_t) k] = bs;
            B = std::max (bs, 0.35f * excess * (1.0f - protect));
        }
        else
        {
            B = excess;
        }

        // decision-directed Wiener gain
        const float aB = a * B + kEps;
        const float xiMl = std::max (P - aB, 0.0f) / aB;
        const float xi = 0.85f * prevClean[(size_t) k] / aB + 0.15f * xiMl;
        float g = xi / (1.0f + xi);

        if (P > kEps) g = std::max (g, std::sqrt (std::min (1.0f, E / P)));  // never cut what the drum model explains
        g = std::max (g, protect);                                             // attack window
        if (k < lowProtectBin) g = 1.0f;
        gainRaw[(size_t) k] = std::max (g, gmin);
    }

    // --- smoothing across frequency (constant-Q box, via prefix sums) ---
    const double oct = 1.0 / 24.0 + settings.freqSmoothing * (1.0 / 3.0 - 1.0 / 24.0);
    const double fracHalf = std::pow (2.0, oct * 0.5) - 1.0;
    prefix[0] = 0.0f;
    for (int k = 0; k < numBins; ++k) prefix[(size_t) k + 1] = prefix[(size_t) k] + gainRaw[(size_t) k];
    for (int k = 0; k < numBins; ++k)
    {
        const int w = (int) (k * fracHalf);
        const int lo = std::max (0, k - w), hi = std::min (numBins - 1, k + w);
        gainTmp[(size_t) k] = (prefix[(size_t) hi + 1] - prefix[(size_t) lo]) / (float) (hi - lo + 1);
    }

    // --- smoothing in time: open immediately (lookahead already placed it), release slowly ---
    for (int k = 0; k < numBins; ++k)
    {
        const float g = gainTmp[(size_t) k];
        float& gs = gainSmooth[(size_t) k];
        gs = g > gs ? g : relC * gs + (1.0f - relC) * g;
        float P = 0.0f;
        for (int c = 0; c < nch; ++c)
        {
            P += std::norm (spec[(size_t) c][(size_t) k]);
            spec[(size_t) c][(size_t) k] *= gs;
        }
        prevClean[(size_t) k] = gs * gs * P / (float) nch;
    }

    // --- meters ---
    for (int b = 0; b < kMeterBands; ++b)
    {
        const int lo = meterEdges[(size_t) b], hi = std::max (lo + 1, meterEdges[(size_t) b + 1]);
        float sum = 0.0f;
        for (int k = lo; k < hi; ++k) sum += gainSmooth[(size_t) k];
        meter[(size_t) b].store (gainToDb (sum / (float) (hi - lo), -60.0f), std::memory_order_relaxed);
    }

    // --- synthesis: IFFT, window, overlap-add ---
    const float norm = 0.5f;  // sqrt-Hann^2 at 75% overlap sums to 2
    for (int c = 0; c < nch; ++c)
    {
        fft.inverse (spec[(size_t) c].data(), frame.data());
        auto& acc = outAccum[(size_t) c];
        for (int i = 0; i < fftSize; ++i) acc[(size_t) i] += frame[(size_t) i] * window[(size_t) i] * norm;

        auto& of = outFifo[(size_t) c];
        std::copy (acc.begin(), acc.begin() + hop, of.begin());
        std::move (acc.begin() + hop, acc.end(), acc.begin());
        std::fill (acc.end() - hop, acc.end(), 0.0f);

        auto& in = inFifo[(size_t) c];
        std::move (in.begin() + hop, in.end(), in.begin());
    }
    std::move (refFifo.begin() + hop, refFifo.end(), refFifo.begin());
}

//==============================================================================
DebleedAnalysis analyseDebleed (const float* target, int length,
                                const std::vector<const float*>& refs,
                                double sr, Instrument instrument, float sensitivity)
{
    DebleedAnalysis result;
    const int N = debleedFftSizeFor (sr);
    const int H = N / 4;
    const int bins = N / 2 + 1;
    length = std::min (length, (int) (sr * 180.0));  // analyse at most 3 minutes
    auto& model = result.model;
    model.sampleRate = sr;
    model.fftSize = N;
    model.transfer.assign ((size_t) bins, 0.0f);
    model.bleedFloor.assign ((size_t) bins, 0.0f);
    model.decayTau.assign ((size_t) bins, 0.0f);
    result.referenceScores.assign (refs.size(), 0.0f);
    if (length < N * 4) return result;

    // 1) target hits: two-pass take analysis (take-level reference + spectral fingerprint)
    DetectorSettings ds;
    ds.instrument = instrument;
    ds.sensitivity = sensitivity;
    ds.ghostSensitivity = sensitivity * 0.9f;
    const auto hits = analyseTake (target, length, sr, ds, &model.velocityRefDb, &model.targetTemplate);
    result.numHits = (int) hits.size();

    // 2) frame activity: inactive = no target hit in [frameStart - decayLen, frameEnd]
    const int numFrames = (length - N) / H + 1;
    std::vector<int64_t> hitPos;
    for (auto& h : hits) hitPos.push_back (h.position);

    auto countInactive = [&] (double decayLen, std::vector<char>& inactive)
    {
        inactive.assign ((size_t) numFrames, 0);
        int count = 0;
        size_t hi = 0;
        for (int f = 0; f < numFrames; ++f)
        {
            const int64_t s = (int64_t) f * H, e = s + N;
            while (hi < hitPos.size() && hitPos[hi] < s - (int64_t) decayLen) ++hi;
            const bool active = hi < hitPos.size() && hitPos[hi] < e;
            inactive[(size_t) f] = active ? 0 : 1;
            count += active ? 0 : 1;
        }
        return count;
    };

    std::vector<char> inactive;
    double decayLen = getProfile (instrument).decayMs * 0.001 * sr;
    int inactiveCount = countInactive (decayLen, inactive);
    while (inactiveCount < 30 && decayLen > 0.04 * sr)
    {
        decayLen *= 0.6;
        inactiveCount = countInactive (decayLen, inactive);
    }

    // 3) single STFT pass: bleed floor, cross-spectra, band energies for decay fitting
    RealFFT fft;
    fft.prepare (N);
    const auto win = makeSqrtHann (N);
    std::vector<float> fr ((size_t) N);
    std::vector<std::complex<float>> X ((size_t) bins), R ((size_t) bins);
    const size_t nr = refs.size();
    std::vector<std::vector<std::complex<double>>> sxr (nr, std::vector<std::complex<double>> ((size_t) bins));
    std::vector<std::vector<double>> srr (nr, std::vector<double> ((size_t) bins, 0.0));
    std::vector<double> sxx ((size_t) bins, 0.0);

    constexpr int kBands = 24;
    std::array<int, kBands + 1> edges {};
    for (int b = 0; b <= kBands; ++b)
        edges[(size_t) b] = std::clamp ((int) std::lround (40.0 * std::pow (std::min (18000.0, sr * 0.45) / 40.0, (double) b / kBands) * N / sr), 1, bins - 1);
    std::vector<std::array<float, kBands>> bandDb ((size_t) numFrames);
    double hitEnergy = 0.0; int hitFrames = 0;

    for (int f = 0; f < numFrames; ++f)
    {
        const int s = f * H;
        for (int i = 0; i < N; ++i) fr[(size_t) i] = target[s + i] * win[(size_t) i];
        fft.forward (fr.data(), X.data());

        for (int b = 0; b < kBands; ++b)
        {
            double e = 0.0;
            for (int k = edges[(size_t) b]; k < std::max (edges[(size_t) b] + 1, edges[(size_t) b + 1]); ++k) e += std::norm (X[(size_t) k]);
            bandDb[(size_t) f][(size_t) b] = (float) (10.0 * std::log10 (e + 1e-20));
        }

        if (inactive[(size_t) f])
        {
            for (int k = 0; k < bins; ++k) sxx[(size_t) k] += std::norm (X[(size_t) k]);
            for (size_t r = 0; r < nr; ++r)
            {
                for (int i = 0; i < N; ++i) fr[(size_t) i] = refs[r][s + i] * win[(size_t) i];
                fft.forward (fr.data(), R.data());
                for (int k = 0; k < bins; ++k)
                {
                    sxr[r][(size_t) k] += std::complex<double> (X[(size_t) k] * std::conj (R[(size_t) k]));
                    srr[r][(size_t) k] += std::norm (R[(size_t) k]);
                }
            }
        }
        else
        {
            double e = 0.0;
            for (int k = 0; k < bins; ++k) e += std::norm (X[(size_t) k]);
            hitEnergy += e; ++hitFrames;
        }
    }

    const double invInactive = 1.0 / std::max (1, inactiveCount);
    double floorTotal = 0.0;
    for (int k = 0; k < bins; ++k)
    {
        model.bleedFloor[(size_t) k] = (float) (sxx[(size_t) k] * invInactive);
        floorTotal += model.bleedFloor[(size_t) k];
    }
    if (hitFrames > 0 && inactiveCount > 0)
        result.bleedLevelDb = (float) (10.0 * std::log10 ((floorTotal + 1e-20) / (hitEnergy / hitFrames + 1e-20)));

    // 4) reference detection: weighted mean magnitude-squared coherence in 100 Hz .. 8 kHz
    const int kLo = std::max (1, (int) (100.0 * N / sr)), kHi = std::min (bins - 1, (int) (8000.0 * N / sr));
    for (size_t r = 0; r < nr; ++r)
    {
        double num = 0.0, den = 0.0;
        for (int k = kLo; k <= kHi; ++k)
        {
            const double coh = std::norm (sxr[r][(size_t) k]) / (srr[r][(size_t) k] * sxx[(size_t) k] + 1e-30);
            num += coh * sxx[(size_t) k];
            den += sxx[(size_t) k];
        }
        result.referenceScores[r] = (float) (den > 0.0 ? num / den : 0.0);
        if (result.referenceScores[r] > 0.15f
            && (result.bestReference < 0 || result.referenceScores[r] > result.referenceScores[(size_t) result.bestReference]))
            result.bestReference = (int) r;
    }
    if (result.bestReference >= 0 && inactiveCount >= 8)
    {
        const auto r = (size_t) result.bestReference;
        for (int k = 0; k < bins; ++k)
            model.transfer[(size_t) k] = (float) std::min (4.0, std::norm (sxr[r][(size_t) k]) / (srr[r][(size_t) k] * srr[r][(size_t) k] + 1e-30));
        model.hasTransfer = true;
    }

    // 5) decay times per band from isolated hits
    std::array<std::vector<float>, kBands> taus;
    std::array<float, kBands> floorBandDb {};
    for (int b = 0; b < kBands; ++b)
    {
        double e = 0.0;
        for (int k = edges[(size_t) b]; k < std::max (edges[(size_t) b] + 1, edges[(size_t) b + 1]); ++k) e += model.bleedFloor[(size_t) k];
        floorBandDb[(size_t) b] = (float) (10.0 * std::log10 (e + 1e-20));
    }
    const double frameSec = (double) H / sr;
    for (size_t h = 0; h < hitPos.size(); ++h)
    {
        const int64_t gap = (h + 1 < hitPos.size() ? hitPos[h + 1] : (int64_t) length) - hitPos[h];
        if (gap < (int64_t) (0.12 * sr)) continue;
        const int f0 = (int) std::max<int64_t> (0, (hitPos[h] - N / 2) / H);
        const int fEnd = (int) std::min<int64_t> (numFrames - 1, (hitPos[h] + std::min<int64_t> (gap, (int64_t) (1.5 * sr)) - N) / H);
        for (int b = 0; b < kBands; ++b)
        {
            int fp = f0;
            for (int f = f0; f <= std::min (fEnd, f0 + 3); ++f)
                if (bandDb[(size_t) f][(size_t) b] > bandDb[(size_t) fp][(size_t) b]) fp = f;
            double sx = 0, sy = 0, sxx2 = 0, sxy = 0; int cnt = 0;
            for (int f = fp; f <= fEnd; ++f)
            {
                const double y = bandDb[(size_t) f][(size_t) b];
                if (y < floorBandDb[(size_t) b] + 6.0 || y < bandDb[(size_t) fp][(size_t) b] - 40.0) break;
                const double x = (f - fp) * frameSec;
                sx += x; sy += y; sxx2 += x * x; sxy += x * y; ++cnt;
            }
            if (cnt >= 4)
            {
                const double slope = (cnt * sxy - sx * sy) / (cnt * sxx2 - sx * sx + 1e-20);
                if (slope < -1.0) taus[(size_t) b].push_back ((float) (-8.686 / slope));
            }
        }
    }

    const double defTau = getProfile (instrument).decayMs * 0.001;
    std::array<float, kBands> bandTau {};
    for (int b = 0; b < kBands; ++b)
    {
        auto& v = taus[(size_t) b];
        const double hz = 40.0 * std::pow (std::min (18000.0, sr * 0.45) / 40.0, (b + 0.5) / kBands);
        const float fallback = (float) (defTau * std::clamp (std::sqrt (500.0 / hz), 0.5, 1.5));
        if (v.size() >= 2)
        {
            std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
            bandTau[(size_t) b] = std::clamp (v[v.size() / 2], 0.01f, 8.0f);
        }
        else
        {
            bandTau[(size_t) b] = fallback;
        }
    }
    for (int k = 0; k < bins; ++k)
    {
        int b = 0;
        while (b < kBands - 1 && k >= edges[(size_t) b + 1]) ++b;
        model.decayTau[(size_t) k] = bandTau[(size_t) b];
    }

    result.suggestedAmount = std::clamp (0.4f + (result.bleedLevelDb + 40.0f) / 40.0f * 0.5f, 0.3f, 0.9f);
    return result;
}

} // namespace thruma
