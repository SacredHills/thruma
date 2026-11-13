#pragma once

#include "Debleed.h"
#include "KitManager.h"
#include "LinkHub.h"
#include "Parameters.h"
#include "Rcu.h"
#include "ReplaceEngine.h"
#include "Triggers.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

namespace thruma
{

/** Detection knowledge learned by Analyze, applied to live detection. */
struct LiveDetectionModel
{
    float refDb = -150.0f;
    SpectralTemplate tmpl;
};

struct PreviewItem
{
    SamplePtr sample;
    float gain = 1.0f;
};

/** Pre-allocated capture of the incoming audio on the host timeline (for the waveform editor/analysis). */
struct CaptureBuffer
{
    static constexpr int kChannels = 3;   // 0 = target (mono), 1/2 = reference candidates
    std::array<std::vector<float>, kChannels> data;
    std::atomic<int64_t> origin { -1 };   // host timeline position of index 0
    std::atomic<int64_t> length { 0 };    // highest written index + 1
    int64_t capacity = 0;
    double sampleRate = 0.0;
    bool hasRef = false;
};

class ThrumaProcessor : public juce::AudioProcessor,
                        private juce::AudioProcessorValueTreeState::Listener,
                        private juce::AsyncUpdater,
                        private juce::Timer
{
public:
    ThrumaProcessor();
    ~ThrumaProcessor() override;

    //== AudioProcessor ===========================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Thruma"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //== UI-facing API (message thread) ============================================
    juce::AudioProcessorValueTreeState apvts;
    juce::UndoManager& getUndoManager() { return undoManager; }

    KitManager& getKitManager() { return kitManager; }
    void rebuildKit();
    juce::StringArray getMissingSampleFiles() const { return missingFiles; }
    double getCurrentSampleRate() const { return currentRate; }

    // listening (always on while the host plays) & editing
    void clearCapture();
    void clearAll();   // triggers + recorded audio
    const CaptureBuffer& getCapture() const { return capture; }
    const std::vector<float>& getStoredOverview() const { return storedOverview; }   // min/max pairs per kOverviewBin
    int64_t getStoredOverviewOrigin() const { return storedOverviewOrigin; }
    static constexpr int kOverviewBin = 256;

    TriggerList& getTriggers() { return triggers; }
    void publishTriggers();
    /** Replace the trigger list as one undoable step. A user edit marks changed triggers as locked
        (live re-detection keeps them) and switches playback to the edited list. */
    void commitTriggerEdit (std::vector<Trigger> newList, const juce::String& actionName, bool userEdit = true);
    juce::String getLearnStatus() const { return learnStatus; }
    bool isAnalysing() const { return analysing.load(); }
    bool exportMidi (const juce::File& file) const;
    double getLastBpm() const { return lastBpm.load(); }
    int64_t getPlayheadPosition() const { return playhead.load(); }
    bool isTransportPlaying() const { return transportPlaying.load(); }

    void previewSample (const juce::File& file);

    /** Detection level / threshold history on the host timeline (one value per kEnvBin samples). */
    static constexpr int kEnvBin = DetectionScope::kDecim;
    const std::vector<float>& getEnvelopeLevel() const { return envLevel; }
    const std::vector<float>& getEnvelopeThreshold() const { return envThreshold; }
    float getCurrentThresholdDb() const;
    /** Host-timeline position = scope (engine) position + this offset. */
    int64_t getScopeToTimelineOffset() const { return scopeOffset.load (std::memory_order_relaxed); }
    float getCurrentReferenceDb() const { return replace.getScope().referenceDb.load(); }

    void updateTrackProperties (const TrackProperties& properties) override;
    juce::String getTrackName() const { return trackName; }

    // debleed
    float getDebleedMeterDb (int band) const { return debleed.getMeterDb (band); }
    bool isDebleedActive() const { return configuredDebleed; }
    float getDebleedMeterHz (int band) const { return debleed.getMeterBandHz (band); }
    bool hasDebleedModel() const { return debleedModelPublisher.getLatest() != nullptr; }

    // linked instances (every instance shares its track under the DAW track name)
    void setPublishName (const juce::String& name);
    juce::String getPublishName() const { return publishName; }
    juce::StringArray getOtherTrackNames() const;
    void setLinkedSourceName (const juce::String& name);
    juce::String getLinkedSourceName() const { return linkedSourceName; }
    bool isLinkSynced() const { return linkSynced.load(); }
    bool isSidechainConnected() const { return sidechainChannels.load() > 0; }

    const DetectionScope& getScope() const { return replace.getScope(); }

    // meters / activity
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };
    std::atomic<int> hitCounter { 0 };
    std::atomic<float> lastHitVelocity { 0.0f };

    int getUiTab() const { return uiTab; }
    void setUiTab (int t) { uiTab = t; }

private:
    void parameterChanged (const juce::String& id, float) override;
    void handleAsyncUpdate() override;
    void timerCallback() override;

    void configureEngines();   // apply lookahead / debleed / link configuration (audio suspended)
    void updateLatency();
    void readParameters (ReplaceSettings& rs, DebleedSettings& ds, ReferenceSource& ref, bool& midiOutOn) const;
    void processChunk (juce::AudioBuffer<float>& buffer, int start, int n, juce::MidiBuffer& midiIn,
                       juce::MidiBuffer& midiOutBuf, const juce::Optional<juce::AudioPlayHead::PositionInfo>& pos, int64_t chunkTimeline);
    void allocateCapture();
    void buildStoredOverview();
    void drainHitLog();
    void maybeAutoLearn();

    // --- engines (audio thread) ---
    ReplaceEngine replace;
    DebleedProcessor debleed;
    SampleEngine previewEngine;
    std::array<DelayLine, 2> linkDelayLines;

    // --- published immutable data ---
    RcuPublisher<Kit> kitPublisher;
    RcuPublisher<TriggerSnapshot> triggerPublisher;
    RcuPublisher<DebleedModel> debleedModelPublisher;
    RcuPublisher<LiveDetectionModel> liveModelPublisher;
    RcuPublisher<PreviewItem> previewPublisher;
    uint64_t appliedDebleedModelSeq = 0, appliedLiveModelSeq = 0, playedPreviewSeq = 0;

    // --- configuration (changed with audio suspended) ---
    double currentRate = 48000.0;
    int maxBlock = 512, mainChannels = 2;
    float configuredLookaheadMs = 10.0f;
    bool configuredDebleed = false, configuredLinked = false;
    int linkDelay = 0;
    std::atomic<int> sidechainChannels { 0 };
    bool prepared = false;

    // --- audio thread scratch ---
    std::vector<float> refBuf, monoBuf, auditionBuf;
    std::array<MidiTrigger, 128> midiTriggers {};
    struct PendingNote { int samplesUntil; int note; int velocity; bool on; };
    std::array<PendingNote, 512> pendingNotes {};
    int numPending = 0;
    int64_t internalTimeline = 0;
    std::vector<float> envLevel, envThreshold;
    int64_t lastScopePoint = 0;
    std::atomic<int64_t> scopeOffset { 0 };
    juce::String trackName;
    Instrument lastInstrument = Instrument::Count;
    ReplaceSettings lastReplaceSettings;
    DebleedSettings lastDebleedSettings;
    bool settingsValid = false;

    // --- capture ---
    CaptureBuffer capture;
    // live hit log: audio thread -> editor (spans that became final + hits inside them)
    struct LogEvent { int64_t a = 0, b = 0; float velocity = 0; bool isHit = false, ghost = false; };
    static constexpr int kLogSize = 8192;
    std::array<LogEvent, kLogSize> logEvents {};
    juce::AbstractFifo logFifo { kLogSize };
    struct HeldHit { int64_t pos; float velocity; bool ghost; };
    std::array<HeldHit, 256> heldHits {};
    int numHeld = 0;
    int64_t logFinal = 0, logExpectedNext = -1, logPendingEnd = 0;
    // automatic learning
    int64_t lastLearnLength = 0;
    int lastLearnInstrument = -1;
    juce::String learnStatus;
    std::vector<float> storedOverview;
    int64_t storedOverviewOrigin = 0;

    // --- message-thread state ---
    KitManager kitManager;
    juce::StringArray missingFiles;
    TriggerList triggers;
    juce::UndoManager undoManager { 30000, 100 };
    std::atomic<bool> analysing { false };
    juce::String publishName, linkedSourceName;
    std::atomic<int> publishId { -1 }, linkedId { -1 };
    std::atomic<bool> linkSynced { false };
    std::atomic<double> lastBpm { 120.0 };
    std::atomic<int64_t> playhead { 0 };
    std::atomic<bool> transportPlaying { false };
    int uiTab = 1;   // REPLACE
    std::unique_ptr<juce::ThreadPool> workers;

    JUCE_DECLARE_WEAK_REFERENCEABLE (ThrumaProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThrumaProcessor)
};

} // namespace thruma
