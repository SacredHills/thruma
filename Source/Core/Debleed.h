// Spectral drum debleed processor.
//
// This is NOT a gate. Per STFT bin, the processor estimates how much of the close-mic signal
// is the target drum and how much is bleed, and applies a smoothed, zero-phase Wiener-style gain:
//
//   * Target model: a transient detector (running ahead of the STFT, i.e. lookahead) marks the
//     target's hits. At each hit the target's spectrum is captured and then decays per bin with
//     the drum's learned decay time. Energy explained by this model is always preserved, so the
//     natural decay of the drum survives while bleed that arrives during the decay is reduced.
//   * Bleed model: with a reference mic (sidechain or linked instance) the bleed spectrum is
//     predicted as |H(f)|^2 * |R(f)|^2, where H is learned adaptively (and/or by Analyze) only
//     while the target is quiet. Without a reference, energy not explained by the target model
//     is treated as bleed (blind mode).
//   * Gain: decision-directed Wiener gain, floor-limited by Amount/Max Reduction, protected during
//     the attack window, smoothed across frequency and with fast-open/slow-release in time to
//     avoid musical noise and pumping. Real-valued gains keep the phase of the target intact,
//     and all channels share one gain so stereo images and multi-mic phase stay coherent.
#pragma once

#include "FFT.h"
#include "TransientDetector.h"

#include <array>
#include <atomic>
#include <complex>
#include <vector>

namespace thruma
{

struct DebleedSettings
{
    Instrument instrument  = Instrument::Snare;
    float amount           = 0.6f;    // 0..1
    float sensitivity      = 0.5f;    // target hit detection (ghost-note protection)
    float attackPreserve   = 0.6f;    // 0..1 (length/strength of the fully-open attack window)
    float releaseMs        = 120.0f;  // gain release
    bool  useReference     = false;
    // advanced
    float maxReductionDb   = 30.0f;
    float freqSmoothing    = 0.4f;    // 0..1 -> 1/24 .. 1/3 octave
    float lowProtectHz     = 0.0f;    // bins below this are never attenuated
    float decayScale       = 1.0f;    // multiplies the modelled decay times
    bool  adaptive         = true;    // keep learning the bleed transfer online
};

/** Result of offline analysis; also restored from plugin state. Bin layout matches fftSize/sampleRate. */
struct DebleedModel
{
    double sampleRate = 0.0;
    int fftSize = 0;
    std::vector<float> transfer;    // |H|^2 reference->target bleed power transfer per bin
    std::vector<float> bleedFloor;  // average bleed power per bin (target quiet)
    std::vector<float> decayTau;    // amplitude decay time constant per bin, seconds
    float velocityRefDb = -150.0f;
    SpectralTemplate targetTemplate;   // fingerprint of the target drum (from take analysis)
    bool hasTransfer = false;

    int getNumBins() const noexcept { return fftSize / 2 + 1; }
    bool matches (double sr, int n) const noexcept { return fftSize == n && std::abs (sampleRate - sr) < 0.5; }
    /** Re-map to another FFT layout / sample rate by interpolating over frequency. */
    DebleedModel remapped (double newRate, int newFftSize) const;
};

int debleedFftSizeFor (double sampleRate) noexcept;

class DebleedProcessor
{
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMeterBands = 32;

    void prepare (double sampleRate, int numChannels);
    void reset() noexcept;
    void setSettings (const DebleedSettings& s) noexcept;
    const DebleedSettings& getSettings() const noexcept { return settings; }

    /** Copy model values into the running estimators (no allocation; ignored if layout differs). */
    void adoptModel (const DebleedModel& m) noexcept;

    int getLatency() const noexcept { return fftSize; }
    int getFftSize() const noexcept { return fftSize; }
    double getSampleRate() const noexcept { return sr; }

    /** In-place processing. 'ref' may be null (blind mode). */
    void process (float* const* io, int numChannels, const float* ref, int n) noexcept;

    /** Per-band average gain reduction in dB (<= 0), for the UI. Thread-safe read. */
    float getMeterDb (int band) const noexcept { return meter[(size_t) band].load (std::memory_order_relaxed); }
    float getMeterBandHz (int band) const noexcept;

    const TransientDetector& getDetector() const noexcept { return detector; }

private:
    void processFrame (bool hasRef) noexcept;
    float defaultTau (int bin) const noexcept;

    DebleedSettings settings;
    double sr = 48000.0;
    int fftSize = 1024, hop = 256, numBins = 513, channels = 1;

    RealFFT fft;
    std::vector<float> window;           // sqrt periodic Hann
    std::array<std::vector<float>, kMaxChannels> inFifo, outAccum, outFifo;
    std::vector<float> refFifo;
    int hopCount = 0;
    int64_t samplePos = 0;               // input samples consumed

    std::vector<float> frame;
    std::array<std::vector<std::complex<float>>, kMaxChannels> spec;
    std::vector<std::complex<float>> refSpec;

    // estimators
    std::vector<std::complex<float>> pxr;
    std::vector<float> prr, bleedFloor, targetEnv, bleedSmooth, gainSmooth, prevClean, tau, decayPerFrame;
    std::vector<float> gainRaw, gainTmp, prefix, confidence, modelTransfer;
    bool modelHasTransfer = false, modelHasTau = false;
    int activeChannels = 1;
    bool prepared = false;

    // detection (runs ahead of the STFT output by fftSize samples -> lookahead)
    TransientDetector detector;
    static constexpr int kMaxOnsets = 64;
    std::array<int64_t, kMaxOnsets> onsets {};
    int onsetWrite = 0, onsetCount = 0;

    std::array<std::atomic<float>, kMeterBands> meter {};
    std::array<int, kMeterBands + 1> meterEdges {};
};

/** Offline analysis of captured audio ("Analyze" button). */
struct DebleedAnalysis
{
    DebleedModel model;
    int numHits = 0;
    int bestReference = -1;               // index into the candidate list, -1 = none useful
    std::vector<float> referenceScores;   // mean coherence per candidate (0..1)
    float bleedLevelDb = -150.0f;         // bleed level relative to target hit peaks
    float suggestedAmount = 0.6f;
};

DebleedAnalysis analyseDebleed (const float* target, int length,
                                const std::vector<const float*>& referenceCandidates,
                                double sampleRate, Instrument instrument, float sensitivity);

} // namespace thruma
