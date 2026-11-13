#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Resampler.h"

#include <cstring>
#include <limits>

namespace thruma
{

namespace
{
    constexpr double kCaptureSeconds = 360.0;   // 6 minutes of target (+ reference) history; Analyze / Import grow it

    /** Undoable whole-list trigger edit (lists are small; snapshots keep undo trivially correct). */
    class TriggerEditAction : public juce::UndoableAction
    {
    public:
        TriggerEditAction (ThrumaProcessor& p, std::vector<Trigger> before, std::vector<Trigger> after)
            : proc (p), oldList (std::move (before)), newList (std::move (after)) {}
        bool perform() override { proc.getTriggers().setAll (newList); proc.publishTriggers(); return true; }
        bool undo() override    { proc.getTriggers().setAll (oldList); proc.publishTriggers(); return true; }
        int getSizeInUnits() override { return (int) (oldList.size() + newList.size()) * (int) sizeof (Trigger) + 16; }
    private:
        ThrumaProcessor& proc;
        std::vector<Trigger> oldList, newList;
    };

    juce::String floatsToBase64 (const std::vector<float>& v)
    {
        return juce::Base64::toBase64 (v.data(), v.size() * sizeof (float));
    }

    std::vector<float> base64ToFloats (const juce::String& s)
    {
        juce::MemoryOutputStream mo;
        if (! juce::Base64::convertFromBase64 (mo, s)) return {};
        std::vector<float> v (mo.getDataSize() / sizeof (float));
        std::memcpy (v.data(), mo.getData(), v.size() * sizeof (float));
        return v;
    }

    template <typename T> bool same (const T& a, const T& b) { return std::memcmp (&a, &b, sizeof (T)) == 0; }

    bool sameSettings (const ReplaceSettings& a, const ReplaceSettings& b)
    {
        const auto& d = a.detector; const auto& e = b.detector;
        const auto& p = a.playback; const auto& q = b.playback;
        return same (a.enabled, b.enabled) && same (a.mode, b.mode) && same (a.source, b.source) && same (a.sampleSlot, b.sampleSlot)
            && same (a.blend, b.blend) && same (a.outputDb, b.outputDb) && same (a.alignMs, b.alignMs)
            && same (d.instrument, e.instrument) && same (d.sensitivity, e.sensitivity) && same (d.ghostSensitivity, e.ghostSensitivity)
            && same (d.retriggerMs, e.retriggerMs) && same (d.doubleHits, e.doubleHits) && same (d.frequencyFocus, e.frequencyFocus)
            && same (d.dynamicRangeDb, e.dynamicRangeDb)
            && same (p.velocityResponse, q.velocityResponse) && same (p.dynamicRangeDb, q.dynamicRangeDb) && same (p.gainDb, q.gainDb)
            && same (p.invertPolarity, q.invertPolarity) && same (p.autoAlign, q.autoAlign) && same (p.startOffset, q.startOffset);
    }

    bool sameSettings (const DebleedSettings& a, const DebleedSettings& b)
    {
        return same (a.instrument, b.instrument) && same (a.amount, b.amount) && same (a.sensitivity, b.sensitivity)
            && same (a.attackPreserve, b.attackPreserve) && same (a.releaseMs, b.releaseMs) && same (a.useReference, b.useReference)
            && same (a.maxReductionDb, b.maxReductionDb) && same (a.freqSmoothing, b.freqSmoothing) && same (a.lowProtectHz, b.lowProtectHz)
            && same (a.decayScale, b.decayScale) && same (a.adaptive, b.adaptive);
    }

    juce::String templateToString (const SpectralTemplate& t)
    {
        juce::StringArray a;
        a.add (juce::String (t.count));
        a.add (juce::String (t.learnedAtRefDb));
        for (auto v : t.shape) a.add (juce::String (v, 3));
        return a.joinIntoString (" ");
    }

    SpectralTemplate templateFromString (const juce::String& s)
    {
        SpectralTemplate t;
        auto a = juce::StringArray::fromTokens (s, " ", "");
        if (a.size() != 2 + SpectralTemplate::kBands) return t;
        t.count = a[0].getIntValue();
        t.learnedAtRefDb = a[1].getFloatValue();
        for (int i = 0; i < SpectralTemplate::kBands; ++i) t.shape[(size_t) i] = a[2 + i].getFloatValue();
        return t;
    }
}

//==============================================================================
ThrumaProcessor::ThrumaProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    for (auto* id : { ids::dbOn, ids::dbReference, ids::repLatency, ids::instrument })
        apvts.addParameterListener (id, this);

    kitPublisher.publish (kitManager.buildKit (currentRate));
    triggerPublisher.publish (std::make_unique<TriggerSnapshot>());
    workers = std::make_unique<juce::ThreadPool> (1);
    startTimerHz (20);
}

ThrumaProcessor::~ThrumaProcessor()
{
    stopTimer();
    cancelPendingUpdate();
    workers->removeAllJobs (true, 20000);
    workers.reset();
    for (auto* id : { ids::dbOn, ids::dbReference, ids::repLatency, ids::instrument })
        apvts.removeParameterListener (id, this);
    if (publishId.load() >= 0) LinkHub::getInstance().unregisterSource (publishId.load());
}

//==============================================================================
bool ThrumaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    if (! in.isDisabled() && in != out) return false;   // no audio input: a MIDI-driven drum instrument
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::mono() && sc != juce::AudioChannelSet::stereo()) return false;
    }
    return true;
}

void ThrumaProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const bool rateChanged = std::abs (sampleRate - currentRate) > 0.5;
    currentRate = sampleRate;
    maxBlock = juce::jmax (64, samplesPerBlock);
    // processing follows the output: as an instrument (e.g. Studio One) the main input may be switched off
    mainChannels = juce::jlimit (1, 2, getMainBusNumOutputChannels());
    const auto* scBus = getBusCount (true) > 1 ? getBus (true, 1) : nullptr;
    sidechainChannels.store (scBus != nullptr && scBus->isEnabled() ? juce::jmin (2, scBus->getNumberOfChannels()) : 0);

    refBuf.assign ((size_t) maxBlock, 0.0f);
    monoBuf.assign ((size_t) maxBlock, 0.0f);
    auditionBuf.assign ((size_t) maxBlock, 0.0f);
    previewEngine.prepare (sampleRate);
    numPending = 0;

    configureEngines();

    const auto* kit = kitPublisher.getLatest();
    if (kit == nullptr || rateChanged || std::abs (kit->playRate - sampleRate) > 0.5)
        rebuildKit();

    if (const auto* m = debleedModelPublisher.getLatest())
        if (! m->matches (sampleRate, debleed.getFftSize()))
            debleedModelPublisher.publish (std::make_unique<DebleedModel> (m->remapped (sampleRate, debleed.getFftSize())));

    allocateCapture();   // always listening: the track's audio is recorded while the host plays
    if (publishName.isEmpty())
    {
        static std::atomic<int> instanceCounter { 0 };
        setPublishName (trackName.isNotEmpty() ? trackName : "Thruma " + juce::String (++instanceCounter));
    }
    prepared = true;
}

void ThrumaProcessor::releaseResources()
{
    if (! prepared) return;   // hosts may call this before prepareToPlay
    prepared = false;
    replace.reset();
    previewEngine.reset();
    kitPublisher.markAudioIdle();
    triggerPublisher.markAudioIdle();
    previewPublisher.markAudioIdle();
    midiSeqPublisher.markAudioIdle();
}

void ThrumaProcessor::configureEngines()
{
    const int latencyChoice = (int) apvts.getRawParameterValue (ids::repLatency)->load();
    const auto inst = (Instrument) juce::jlimit (0, kNumInstruments - 1, (int) apvts.getRawParameterValue (ids::instrument)->load());
    configuredLookaheadMs = latencyChoice == 0 ? getProfile (inst).lookaheadMs : 3.0f;
    configuredDebleed = apvts.getRawParameterValue (ids::dbOn)->load() > 0.5f;
    configuredLinked = (ReferenceSource) (int) apvts.getRawParameterValue (ids::dbReference)->load() == ReferenceSource::Linked;

    replace.prepare (currentRate, maxBlock, mainChannels, configuredLookaheadMs);
    debleed.prepare (currentRate, mainChannels);
    linkDelay = configuredLinked ? juce::jmax (1024, 2 * maxBlock) : 0;
    for (auto& d : linkDelayLines) d.prepare (linkDelay + 1);

    ReplaceSettings rs; DebleedSettings ds; ReferenceSource ref; bool mo;
    readParameters (rs, ds, ref, mo);
    replace.setSettings (rs);
    debleed.setSettings (ds);
    replace.reset();
    settingsValid = false;
    lastInstrument = Instrument::Count;
    appliedDebleedModelSeq = 0;
    appliedLiveModelSeq = 0;
    logExpectedNext = -1;
    numHeld = 0;
    allocateCapture();
    updateLatency();
}

void ThrumaProcessor::updateLatency()
{
    setLatencySamples (replace.getLatency() + (configuredDebleed ? debleed.getLatency() : 0) + linkDelay);
}

void ThrumaProcessor::parameterChanged (const juce::String&, float)
{
    triggerAsyncUpdate();   // latency-affecting parameters: reconfigure on the message thread
}

void ThrumaProcessor::handleAsyncUpdate()
{
    if (! prepared) return;
    suspendProcessing (true);
    configureEngines();
    suspendProcessing (false);
}

void ThrumaProcessor::timerCallback()
{
    kitPublisher.collectGarbage();
    triggerPublisher.collectGarbage();
    debleedModelPublisher.collectGarbage();
    liveModelPublisher.collectGarbage();
    previewPublisher.collectGarbage();
    midiSeqPublisher.collectGarbage();
    if (previewPlaying.load()) hostBridge.keepAwake();   // hosts that pause idle tracks: keep this one running

    // offline analysis: un-finalizing goes back to live detection; changed detection settings re-analyse
    if (offlineSource != OfflineSource::None && ! isFinalized() && ! analysing.load())
        dropOfflineAudio();
    if (offlineSource != OfflineSource::None && ! analysing.load() && detectionSettingsKey() != analysedKey)
    {
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (reanalyseAt <= 0.0) reanalyseAt = now + 0.3;   // wait until a slider drag settles
        else if (now >= reanalyseAt)
        {
            reanalyseAt = 0.0;
            juce::WeakReference<ThrumaProcessor> weak (this);
            runOfflineAnalysis (false, [weak] (const juce::String& m)
            {
                if (auto* self = weak.get()) { self->offlineMessage = m; ++self->offlineMessageCount; }
            });
        }
    }
    linkedId.store (linkedSourceName.isNotEmpty() ? LinkHub::getInstance().findSource (linkedSourceName.toStdString()) : -1);
    drainHitLog();
    maybeAutoLearn();
}

void ThrumaProcessor::drainHitLog()
{
    const int ready = logFifo.getNumReady();
    if (ready == 0) return;
    int s1, n1, s2, n2;
    logFifo.prepareToRead (ready, s1, n1, s2, n2);
    const auto dedupe = (int64_t) (0.005 * currentRate);
    auto apply = [&] (int start, int count)
    {
        for (int i = start; i < start + count; ++i)
        {
            const auto& e = logEvents[(size_t) i];
            if (! e.isHit)
            {
                triggers.removeUnlockedInRange (e.a, e.b);   // this span was played again: replace old detections
                continue;
            }
            if (const auto* near = triggers.findNearest (e.a, dedupe); near != nullptr && near->locked) continue;
            triggers.add (e.a, e.velocity, e.ghost);
        }
    };
    apply (s1, n1);
    apply (s2, n2);
    logFifo.finishedRead (n1 + n2);
    publishTriggers();
}

void ThrumaProcessor::maybeAutoLearn()
{
    // Learn continuously from what the track has played: take-level dynamics and the drum's fingerprint for
    // live detection, and (when debleed is on) decay times, bleed spectrum and reference transfer.
    if (! prepared || analysing.load() || isFinalized()) return;
    const int inst = (int) apvts.getRawParameterValue (ids::instrument)->load();
    if (inst != lastLearnInstrument) { lastLearnInstrument = inst; lastLearnLength = 0; }

    const auto len = capture.length.load();
    const auto interval = (int64_t) ((lastLearnLength == 0 ? 6.0 : 20.0) * currentRate);
    const auto first = std::clamp<int64_t> (capture.firstIndex.load(), 0, len);
    if (len - std::max (lastLearnLength, first) < interval) return;   // count only audio actually recorded
    lastLearnLength = len;
    analysing.store (true);

    const auto window = std::min<int64_t> (len - first, (int64_t) (90.0 * currentRate));
    const auto from = len - window;
    auto target = std::make_shared<std::vector<float>> (capture.data[0].begin() + from, capture.data[0].begin() + len);
    auto ref = std::make_shared<std::vector<float>>();
    const auto refSrc = (ReferenceSource) (int) apvts.getRawParameterValue (ids::dbReference)->load();
    if (capture.hasRef && refSrc != ReferenceSource::None)
    {
        const auto& r1 = capture.data[1];
        const auto& r2 = capture.data[2];
        ref->resize ((size_t) window);
        for (int64_t i = 0; i < window; ++i)
        {
            const float l = r1[(size_t) (from + i)], r = r2.empty() ? l : r2[(size_t) (from + i)];
            (*ref)[(size_t) i] = refSrc == ReferenceSource::SidechainR ? r : refSrc == ReferenceSource::SidechainSum ? 0.5f * (l + r) : l;
        }
    }
    ReplaceSettings rs; DebleedSettings ds; ReferenceSource rsrc; bool mo;
    readParameters (rs, ds, rsrc, mo);
    const bool learnDebleed = configuredDebleed;
    const double sr = currentRate;
    const double seconds = (double) window / sr;
    juce::WeakReference<ThrumaProcessor> weak (this);

    workers->addJob ([weak, target, ref, rs, ds, learnDebleed, sr, seconds]
    {
        float refDb = -150.0f;
        SpectralTemplate tmpl;
        const auto hits = analyseTake (target->data(), (int64_t) target->size(), sr, rs.detector, &refDb, &tmpl);
        std::unique_ptr<DebleedAnalysis> deb;
        if (learnDebleed && hits.size() >= 2)
        {
            std::vector<const float*> cands;
            if (! ref->empty()) cands.push_back (ref->data());
            deb = std::make_unique<DebleedAnalysis> (thruma::analyseDebleed (target->data(), (int) target->size(), cands, sr,
                                                                             ds.instrument, ds.sensitivity));
        }
        juce::MessageManager::callAsync ([weak, nHits = (int) hits.size(), refDb, tmpl, deb = std::shared_ptr<DebleedAnalysis> (std::move (deb)), seconds]
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            self->analysing.store (false);
            if (nHits >= 4)
            {
                auto lm = std::make_unique<LiveDetectionModel>();
                lm->refDb = refDb;
                lm->tmpl = tmpl;
                self->liveModelPublisher.publish (std::move (lm));
            }
            if (deb != nullptr)
                self->debleedModelPublisher.publish (std::make_unique<DebleedModel> (deb->model));
            self->learnStatus = "Learned from the last " + juce::String ((int) seconds) + " s: " + juce::String (nHits) + " hits"
                                + (deb != nullptr ? ", bleed " + juce::String (deb->bleedLevelDb, 1) + " dB" : juce::String());
        });
    });
}

//==============================================================================
void ThrumaProcessor::readParameters (ReplaceSettings& rs, DebleedSettings& ds, ReferenceSource& ref, bool& midiOutOn) const
{
    auto v = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    const auto inst = (Instrument) juce::jlimit (0, kNumInstruments - 1, (int) v (ids::instrument));

    rs.enabled = v (ids::repOn) > 0.5f;
    rs.mode = (ReplaceMode) (int) v (ids::repMode);
    rs.source = (TriggerSource) juce::jlimit (0, 2, (int) v (ids::repSource));
    rs.sampleSlot = (int) v (ids::repSample) - 1;
    rs.detector.instrument = inst;
    rs.detector.sensitivity = v (ids::repSens) * 0.01f;
    rs.detector.ghostSensitivity = v (ids::repGhost) * 0.01f;
    rs.detector.retriggerMs = v (ids::repRetrig);
    rs.detector.doubleHits = v (ids::repDouble) > 0.5f;
    rs.detector.frequencyFocus = v (ids::repFocus) > 0.5f;
    rs.detector.dynamicRangeDb = v (ids::repDynRange);
    rs.playback.velocityResponse = v (ids::repVelResp) * 0.01f;
    rs.playback.dynamicRangeDb = v (ids::repDynRange);
    rs.playback.gainDb = v (ids::repGain);
    rs.playback.invertPolarity = v (ids::repPolarity) > 0.5f;
    rs.playback.autoAlign = v (ids::repAutoAlign) > 0.5f;
    rs.playback.startOffset = msToSamples (v (ids::repOffset), currentRate);
    rs.blend = v (ids::repBlend) * 0.01f;
    rs.outputDb = v (ids::repOutput);
    rs.alignMs = v (ids::repAlign);

    ds.instrument = inst;
    ds.amount = v (ids::dbAmount) * 0.01f;
    ds.sensitivity = v (ids::dbSens) * 0.01f;
    ds.attackPreserve = v (ids::dbAttack) * 0.01f;
    ds.releaseMs = v (ids::dbRelease);
    ds.maxReductionDb = v (ids::dbMaxRed);
    ds.freqSmoothing = v (ids::dbSmooth) * 0.01f;
    ds.lowProtectHz = v (ids::dbLowProtect);
    ds.decayScale = v (ids::dbDecay) * 0.01f;
    ds.adaptive = v (ids::dbAdaptive) > 0.5f;
    ref = (ReferenceSource) juce::jlimit (0, 4, (int) v (ids::dbReference));
    ds.useReference = ref != ReferenceSource::None;

    midiOutOn = v (ids::midiOut) > 0.5f;
}

void ThrumaProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int total = buffer.getNumSamples();
    if (refBuf.empty()) { midi.clear(); return; }   // not prepared yet: pass audio through untouched

    // clear output channels that have no input
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, total);

    // No main audio input (instrument tracks, e.g. Studio One): the sidechain, which then occupies the first
    // channels, is the drum to track. A mono source feeds both output channels.
    const int mainIn = getMainBusNumInputChannels();
    const auto* scBusInfo = getBusCount (true) > 1 ? getBus (true, 1) : nullptr;
    const int effectiveIn = mainIn > 0 ? mainIn : (scBusInfo != nullptr && scBusInfo->isEnabled() ? scBusInfo->getNumberOfChannels() : 0);
    if (effectiveIn == 1 && getMainBusNumOutputChannels() >= 2 && buffer.getNumChannels() >= 2)
        buffer.copyFrom (1, 0, buffer, 0, 0, total);

    juce::Optional<juce::AudioPlayHead::PositionInfo> pos;
    if (auto* ph = getPlayHead()) pos = ph->getPosition();

    // Work out whether the song is playing and where we are. Not every host reports the play flag
    // reliably, so a host position that moves forward block by block also counts as playing; with no
    // usable host position at all, Thruma runs on its own clock while audio is coming in.
    const bool hasTime = pos.hasValue() && pos->getTimeInSamples().hasValue();
    const int64_t hostTime = hasTime ? *pos->getTimeInSamples() : 0;
    const bool hostPlaying = pos.hasValue() && pos->getIsPlaying();
    if (hostPlaying) hostReportsPlayFlag = true;
    // a moving position counts as playing only in hosts that never report the play flag (some hosts keep the
    // position running while stopped)
    const bool advancing = ! hostReportsPlayFlag && hasTime && lastHostTime != std::numeric_limits<int64_t>::min()
                           && hostTime == lastHostTime + lastHostBlock && hostTime != lastHostTime;
    lastHostTime = hasTime ? hostTime : std::numeric_limits<int64_t>::min();
    lastHostBlock = total;

    float inPk = 0.0f;
    for (int c = 0; c < juce::jmin (buffer.getNumChannels(), juce::jmin (2, effectiveIn)); ++c)
        inPk = std::max (inPk, buffer.getMagnitude (c, 0, total));
    if (inPk > 1.0e-4f) lastSignalBlockTime = internalTimeline;
    const bool signalPresent = internalTimeline - lastSignalBlockTime < (int64_t) (2.0 * currentRate);

    hostBridge.noteCurrentTrack();

    int newMode = 0;
    if (hostPlaying || advancing) { newMode = 1; hostTimelineSeen = true; }   // follow the host transport
    else if (! hostTimelineSeen && signalPresent) newMode = 2;              // host gives no moving position: own clock
    if (newMode == 1 && lastActiveMode == 2)
    {
        // the host started reporting its transport: restart the history on the host timeline
        capture.origin.store (-1);
        capture.length.store (0);
        logExpectedNext = -1;
    }
    if (newMode != 0) lastActiveMode = newMode;

    bool playing = newMode != 0;
    int64_t timeline = newMode == 1 ? hostTime : internalTimeline;
    bool previewing = false;
    if (previewPlaying.load (std::memory_order_relaxed) && ! (hostPlaying || advancing))
    {
        // in-plugin playback of imported audio: it replaces the input, on its own timeline
        const int64_t pp = previewPos.load (std::memory_order_relaxed);
        const auto origin = capture.origin.load (std::memory_order_relaxed);
        const auto len = capture.length.load (std::memory_order_relaxed);
        const int outCh = juce::jmin (buffer.getNumChannels(), juce::jmax (1, getMainBusNumOutputChannels()));
        for (int c = 0; c < outCh; ++c)
        {
            auto* d = buffer.getWritePointer (c);
            for (int i = 0; i < total; ++i)
            {
                const int64_t k = pp + i - origin;
                d[i] = (origin >= 0 && k >= 0 && k < len) ? capture.data[0][(size_t) k] : 0.0f;
            }
        }
        previewing = true;
        playing = true;
        timeline = pp;
        previewPos.store (pp + total, std::memory_order_relaxed);
        if (origin < 0 || pp + total >= origin + len) previewPlaying.store (false);
    }
    else if (previewPlaying.load (std::memory_order_relaxed))
        previewPlaying.store (false);   // the host started playing: it takes over
    blockHasBeat = ! previewing && newMode == 1 && pos.hasValue() && pos->getPpqPosition().hasValue() && pos->getBpm().hasValue() && *pos->getBpm() > 1.0;
    if (blockHasBeat)
    {
        blockBeat = *pos->getPpqPosition();
        blockBeatsPerSample = *pos->getBpm() / 60.0 / currentRate;
        blockTimeline = timeline;
    }
    blockPlaying = playing;
    if (pos.hasValue() && pos->getBpm().hasValue()) lastBpm.store (*pos->getBpm());
    transportPlaying.store (playing);
    transportMode.store (newMode);
    // stopped: show where the next playback starts (Thruma's own playback position, else the host's cursor)
    if (playing) playhead.store (timeline);
    else if (previewMode.load (std::memory_order_relaxed)) playhead.store (previewPos.load (std::memory_order_relaxed));
    else if (hasTime) playhead.store (hostTime);

    juce::MidiBuffer midiOut;
    for (int start = 0; start < total; start += maxBlock)
        processChunk (buffer, start, juce::jmin (maxBlock, total - start), midi, midiOut, pos, timeline + start);
    internalTimeline += total;
    midi.swapWith (midiOut);
}

void ThrumaProcessor::processChunk (juce::AudioBuffer<float>& buffer, int start, int n, juce::MidiBuffer& midiIn,
                                    juce::MidiBuffer& midiOutBuf, const juce::Optional<juce::AudioPlayHead::PositionInfo>& pos,
                                    int64_t chunkTimeline)
{
    const bool playing = blockPlaying;
    juce::ignoreUnused (pos);

    // the main output bus shares its channels with the main input (and still works without an audio input,
    // e.g. on a MIDI/instrument track)
    auto mainBus = getBusBuffer (buffer, false, 0);
    const int nch = juce::jlimit (1, 2, juce::jmin (mainBus.getNumChannels(), mainChannels));
    if (mainBus.getNumChannels() == 0) return;
    float* ch[2] = { mainBus.getWritePointer (0, start), nch > 1 ? mainBus.getWritePointer (1, start) : nullptr };

    int scCh = 0;
    const float* sc[2] = { nullptr, nullptr };
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled() && getMainBusNumInputChannels() > 0)   // else it is the main input
    {
        auto scBus = getBusBuffer (buffer, true, 1);
        scCh = juce::jmin (2, scBus.getNumChannels());
        for (int c = 0; c < scCh; ++c) sc[c] = scBus.getReadPointer (c, start);
    }
    sidechainChannels.store (scCh, std::memory_order_relaxed);

    // --- parameters ---
    ReplaceSettings rs; DebleedSettings ds; ReferenceSource refSrc; bool midiOutOn;
    readParameters (rs, ds, refSrc, midiOutOn);
    if (rs.detector.instrument != lastInstrument)
    {
        // a learned detection model belongs to one instrument
        replace.getDetector().setVelocityReference (-1000.0f);
        replace.getDetector().setSpectralTemplate ({});
        appliedLiveModelSeq = 0;
        lastInstrument = rs.detector.instrument;
    }
    if (! settingsValid || ! sameSettings (rs, lastReplaceSettings))
    {
        replace.setSettings (rs);
        lastReplaceSettings = rs;
    }

    // --- published data ---
    const auto* kitH = kitPublisher.acquire();
    const Kit* kit = kitH != nullptr ? kitH->object.get() : nullptr;
    const uint64_t kitSeq = kitH != nullptr ? kitH->seq : 0;
    const auto* trigH = triggerPublisher.acquire();
    if (const auto* m = debleedModelPublisher.acquire(); m != nullptr && m->seq != appliedDebleedModelSeq)
    {
        debleed.adoptModel (*m->object);
        appliedDebleedModelSeq = m->seq;
    }
    if (const auto* lm = liveModelPublisher.acquire(); lm != nullptr && lm->seq != appliedLiveModelSeq)
    {
        // learned values only seed the live detector: it keeps adapting, so it can never get "stuck"
        replace.getDetector().seedVelocityReference (lm->object->refDb);
        replace.getDetector().seedSpectralTemplate (lm->object->tmpl);
        appliedLiveModelSeq = lm->seq;
    }

    // --- MIDI input -> sample triggers (not echoed to the output) ---
    int numMidi = 0;
    for (const auto meta : midiIn)
    {
        if (meta.samplePosition < start || meta.samplePosition >= start + n) continue;
        const auto msg = meta.getMessage();
        if (msg.isNoteOn() && kit != nullptr && numMidi < (int) midiTriggers.size())
        {
            const int slot = kit->findInstrumentForNote (msg.getNoteNumber());
            if (slot >= 0)
                midiTriggers[(size_t) numMidi++] = { meta.samplePosition - start, slot, msg.getFloatVelocity() };
        }
        else if (! msg.isNoteOnOrOff())
        {
            midiOutBuf.addEvent (msg, meta.samplePosition);   // pass through controllers etc.
        }
    }

    // --- input mono, link publishing, capture, meters ---
    for (int i = 0; i < n; ++i)
        monoBuf[(size_t) i] = nch > 1 ? 0.5f * (ch[0][i] + ch[1][i]) : ch[0][i];
    {
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) pk = std::max (pk, std::abs (monoBuf[(size_t) i]));
        inputPeak.store (std::max (pk, inputPeak.load (std::memory_order_relaxed) * 0.9f), std::memory_order_relaxed);
    }
    if (playing && publishId.load (std::memory_order_relaxed) >= 0)
        LinkHub::getInstance().write (publishId.load (std::memory_order_relaxed), chunkTimeline, monoBuf.data(), n);

    // --- reference signal ---
    const float* ref = nullptr;
    if (scCh > 0 && (refSrc == ReferenceSource::SidechainL || refSrc == ReferenceSource::SidechainR || refSrc == ReferenceSource::SidechainSum))
    {
        const float* l = sc[0];
        const float* r = scCh > 1 ? sc[1] : sc[0];
        for (int i = 0; i < n; ++i)
            refBuf[(size_t) i] = refSrc == ReferenceSource::SidechainL ? l[i] : refSrc == ReferenceSource::SidechainR ? r[i] : 0.5f * (l[i] + r[i]);
        ref = refBuf.data();
    }
    else if (refSrc == ReferenceSource::Linked && configuredLinked)
    {
        const int id = linkedId.load (std::memory_order_relaxed);
        const bool ok = playing && id >= 0 && LinkHub::getInstance().read (id, chunkTimeline - linkDelay, refBuf.data(), n);
        if (! ok) std::fill (refBuf.begin(), refBuf.begin() + n, 0.0f);
        linkSynced.store (ok, std::memory_order_relaxed);
        ref = ok ? refBuf.data() : nullptr;
    }

    const bool recordHistory = playing && capture.capacity > 0 && rs.source == TriggerSource::Live;
    if (recordHistory)
    {
        auto origin = capture.origin.load (std::memory_order_relaxed);
        if (origin < 0)
        {
            // start a little before where playback first started, so an earlier restart is still covered
            origin = std::max<int64_t> (0, chunkTimeline - (int64_t) (10.0 * currentRate));
            if (origin > chunkTimeline) origin = chunkTimeline;
            capture.firstIndex.store (chunkTimeline - origin, std::memory_order_relaxed);
            capture.origin.store (origin, std::memory_order_release);
        }
        const int64_t idx = chunkTimeline - origin;
        if (idx >= 0 && idx + n <= capture.capacity)
        {
            std::copy_n (monoBuf.data(), n, capture.data[0].data() + idx);
            if (capture.hasRef)
            {
                const float* r1 = scCh > 0 ? sc[0] : (refSrc == ReferenceSource::Linked ? refBuf.data() : nullptr);
                const float* r2 = scCh > 1 ? sc[1] : nullptr;
                if (r1 != nullptr) std::copy_n (r1, n, capture.data[1].data() + idx);
                if (r2 != nullptr) std::copy_n (r2, n, capture.data[2].data() + idx);
            }
            if (idx + n > capture.length.load (std::memory_order_relaxed))
                capture.length.store (idx + n, std::memory_order_release);
        }
    }

    // the DAW's beat grid, so exported MIDI lands exactly where the DAW expects it (tempo changes included);
    // recorded whenever the song plays, also for analysed / finalized notes
    if (playing && blockHasBeat)
        if (const auto origin = capture.origin.load (std::memory_order_relaxed); origin >= 0)
        {
            const int64_t idx = chunkTimeline - origin;
            for (int64_t b = std::max<int64_t> (0, (idx + kBeatBin - 1) / kBeatBin); b * kBeatBin < idx + n && b < (int64_t) beatMap.size(); ++b)
                beatMap[(size_t) b] = blockBeat + (double) (b * kBeatBin - idx + (chunkTimeline - blockTimeline)) * blockBeatsPerSample;
        }

    // --- linked-reference alignment delay ---
    if (configuredLinked && linkDelay > 0)
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < n; ++i)
                ch[c][i] = linkDelayLines[(size_t) c].process (ch[c][i], linkDelay);

    // --- debleed ---
    if (configuredDebleed)
    {
        ds.useReference = ds.useReference && ref != nullptr;
        if (! settingsValid || ! sameSettings (ds, lastDebleedSettings))
        {
            debleed.setSettings (ds);
            lastDebleedSettings = ds;
        }
        debleed.process (ch, nch, ref, n);
    }

    // --- replacement ---
    const int64_t engineTimeline = playing ? chunkTimeline - (configuredDebleed ? debleed.getLatency() : 0) - linkDelay : -1;
    const TriggerSnapshot* snap = trigH != nullptr ? trigH->object.get() : nullptr;
    const int noteLen = msToSamples (50.0, currentRate);

    // --- imported MIDI file: notes on the host's beat grid play the kit (scheduled for the output, which the
    // host delays by our latency, so they sound exactly on their beat) ---
    if (const auto* sqH = midiSeqPublisher.acquire())
    {
        const auto& notes = sqH->object->notes;
        if (playing && blockHasBeat && engineTimeline >= 0 && kit != nullptr && ! notes.empty() && blockBeatsPerSample > 0.0)
        {
            const int L = replace.getLatency();
            const int64_t outStart = engineTimeline - L;   // song position of this chunk's output
            const double b0 = blockBeat + (double) (outStart - blockTimeline) * blockBeatsPerSample;
            const double b1 = b0 + (double) n * blockBeatsPerSample;
            auto it = std::lower_bound (notes.begin(), notes.end(), b0, [] (const MidiBeatNote& a, double b) { return a.beat < b; });
            for (; it != notes.end() && it->beat < b1 && numMidi < (int) midiTriggers.size(); ++it)
            {
                const int slot = kit->findInstrumentForNote (it->note);
                if (slot < 0) continue;
                const int i = juce::jlimit (0, n - 1, (int) ((it->beat - b0) / blockBeatsPerSample));
                midiTriggers[(size_t) numMidi++] = { i - L, slot, (float) it->velocity / 127.0f };
            }
        }
        midiSeqPublisher.reportOldestInUse (sqH->seq);
    }
    replace.process (ch, nch, n, engineTimeline, kit, kitSeq, snap, midiTriggers.data(), numMidi,
                     [&] (const ReplaceHitInfo& h)
                     {
                         if (! h.fromMidi && rs.source == TriggerSource::Live && engineTimeline >= 0 && numHeld < (int) heldHits.size())
                             heldHits[(size_t) numHeld++] = { engineTimeline + h.onsetOffset, h.velocity, h.ghost };
                         hitCounter.fetch_add (1, std::memory_order_relaxed);
                         lastHitVelocity.store (h.velocity, std::memory_order_relaxed);
                         if (! midiOutOn || h.fromMidi || kit == nullptr || numPending + 2 > (int) pendingNotes.size()) return;
                         const int note = kit->slots[(size_t) juce::jlimit (0, kNumInstruments - 1, h.instrument)].midiNote;
                         const int vel = velocityToMidi (h.velocity);
                         pendingNotes[(size_t) numPending++] = { h.outputOffset, note, vel, true };
                         pendingNotes[(size_t) numPending++] = { h.outputOffset + noteLen, note, 0, false };
                     });

    // --- live hit log for the editor ---
    // A timeline span is reported once all of its hits are known (detection delay D has passed), together with
    // those hits; the editor replaces earlier detections in that span. Spans are coalesced (~100 ms) and nothing
    // is dropped while the editor is busy (e.g. offline bounces running faster than real time).
    {
        const bool logging = rs.enabled && rs.source == TriggerSource::Live && engineTimeline >= 0;
        auto flush = [this] (int64_t upTo, bool allHeld)
        {
            if (upTo <= logFinal && ! (allHeld && numHeld > 0)) return true;
            if (logFifo.getFreeSpace() < 1 + numHeld) return false;
            auto put = [this] (const LogEvent& e)
            {
                int s1, n1, s2, n2;
                logFifo.prepareToWrite (1, s1, n1, s2, n2);
                logEvents[(size_t) (n1 > 0 ? s1 : s2)] = e;
                logFifo.finishedWrite (n1 + n2);
            };
            if (upTo > logFinal) put ({ logFinal, upTo, 0.0f, false, false });
            int keep = 0;
            for (int i = 0; i < numHeld; ++i)
            {
                const auto& hh = heldHits[(size_t) i];
                if (allHeld || hh.pos < upTo) put ({ hh.pos, 0, hh.velocity, true, hh.ghost });
                else heldHits[(size_t) keep++] = hh;
            }
            numHeld = keep;
            logFinal = std::max (logFinal, upTo);
            return true;
        };

        const bool continuous = logging && engineTimeline == logExpectedNext;
        if (! continuous && logExpectedNext >= 0)
        {
            flush (logPendingEnd, true);   // transport stopped / jumped / looped: report what we have
            logExpectedNext = -1;
        }
        if (logging)
        {
            const int D = replace.getDetector().getMaxReportDelay();
            if (logExpectedNext < 0) { logFinal = engineTimeline - D; numHeld = numHeld > 0 ? numHeld : 0; }
            logPendingEnd = engineTimeline + n - D;
            if (logPendingEnd - logFinal >= (int64_t) (0.1 * currentRate) || numHeld >= (int) heldHits.size() / 2)
                flush (logPendingEnd, false);
            logExpectedNext = engineTimeline + n;
        }
        else
        {
            numHeld = 0;
        }
    }

    // --- detection-level history for the track lane (engine timeline = host timeline of the detector input) ---
    {
        const auto& sc = replace.getScope();
        const auto written = sc.pointsWritten.load (std::memory_order_relaxed);
        const auto origin = capture.origin.load (std::memory_order_relaxed);
        if (rs.source == TriggerSource::Live && engineTimeline >= 0 && origin >= 0 && ! envLevel.empty())
        {
            const int64_t engEnd = replace.getInputPosition();
            scopeOffset.store (engineTimeline + n - engEnd, std::memory_order_relaxed);
            for (int64_t i = std::max (lastScopePoint, written - DetectionScope::kPoints + 1); i < written; ++i)
            {
                const auto k = (size_t) (i % DetectionScope::kPoints);
                const int64_t tl = engineTimeline + n - (engEnd - i * DetectionScope::kDecim);
                const int64_t idx = (tl - origin) / kEnvBin;
                if (idx >= 0 && idx < (int64_t) envLevel.size())
                {
                    envLevel[(size_t) idx] = sc.level[k].load (std::memory_order_relaxed);
                    envThreshold[(size_t) idx] = sc.threshold[k].load (std::memory_order_relaxed);
                }
            }
        }
        lastScopePoint = written;
    }

    // --- sample preview (Samples page) ---
    if (const auto* p = previewPublisher.acquire(); p != nullptr && p->seq != playedPreviewSeq)
    {
        playedPreviewSeq = p->seq;
        if (p->object->sample != nullptr)
            previewEngine.triggerSample (p->object->sample.get(), 1, p->seq, p->object->gain, 0);
    }
    previewEngine.render (ch, nch, n);

    {
        float pk = 0.0f;
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < n; ++i) pk = std::max (pk, std::abs (ch[c][i]));
        outputPeak.store (std::max (pk, outputPeak.load (std::memory_order_relaxed) * 0.9f), std::memory_order_relaxed);
    }

    // --- MIDI out ---
    for (int i = 0; i < numPending;)
    {
        auto& pn = pendingNotes[(size_t) i];
        if (pn.samplesUntil < n)
        {
            const auto msg = pn.on ? juce::MidiMessage::noteOn (10, pn.note, (juce::uint8) pn.velocity)
                                   : juce::MidiMessage::noteOff (10, pn.note);
            midiOutBuf.addEvent (msg, start + juce::jmax (0, pn.samplesUntil));
            pendingNotes[(size_t) i] = pendingNotes[(size_t) --numPending];
        }
        else
        {
            pn.samplesUntil -= n;
            ++i;
        }
    }

    settingsValid = true;

    // --- tell publishers what is still referenced ---
    kitPublisher.reportOldestInUse (std::min (replace.getSampleEngine().getOldestSeqInUse (0, kitSeq),
                                              previewEngine.getOldestSeqInUse (0, kitSeq)));
    if (trigH != nullptr) triggerPublisher.reportOldestInUse (trigH->seq);
    if (const auto* m = debleedModelPublisher.acquire()) debleedModelPublisher.reportOldestInUse (m->seq);
    if (const auto* lm = liveModelPublisher.acquire()) liveModelPublisher.reportOldestInUse (lm->seq);
    if (const auto* p = previewPublisher.acquire()) previewPublisher.reportOldestInUse (previewEngine.getOldestSeqInUse (1, p->seq));
}

//==============================================================================
void ThrumaProcessor::rebuildKit()
{
    missingFiles.clear();
    kitPublisher.publish (kitManager.buildKit (currentRate, &missingFiles));
}

void ThrumaProcessor::commitTriggerEdit (std::vector<Trigger> newList, const juce::String& actionName, bool userEdit)
{
    if (userEdit)
    {
        // lock everything the user added or changed, and play back the edited list from now on
        for (auto& t : newList)
        {
            const auto* old = triggers.find (t.id);
            if (old == nullptr || old->position != t.position || std::abs (old->velocity - t.velocity) > 1.0e-6f)
                t.locked = true;
        }
    }
    undoManager.beginNewTransaction (actionName);
    undoManager.perform (new TriggerEditAction (*this, triggers.getAll(), std::move (newList)));
}

void ThrumaProcessor::publishTriggers()
{
    auto snap = std::make_unique<TriggerSnapshot>();
    snap->triggers = triggers.getAll();
    triggerPublisher.publish (std::move (snap));
}

void ThrumaProcessor::allocateCapture()
{
    // called from prepareToPlay / configureEngines, i.e. never concurrently with processBlock. Toggling Debleed
    // or the reference re-runs this: what was recorded or analysed must survive, so it only grows.
    const bool needRef = isSidechainConnected() || configuredLinked;
    const bool rateChanged = capture.capacity == 0 || std::abs (capture.sampleRate - currentRate) >= 0.5;
    const auto cap = std::max ((int64_t) (kCaptureSeconds * currentRate), rateChanged ? (int64_t) 0 : capture.capacity);
    if (! rateChanged && capture.capacity == cap && (capture.hasRef || ! needRef)) return;

    if (rateChanged)
    {
        capture.origin.store (-1);
        capture.length.store (0);
        for (auto& d : capture.data) { d.clear(); d.shrink_to_fit(); }
        envLevel.clear(); envThreshold.clear(); beatMap.clear();
        capture.hasRef = false;
        lastLearnLength = 0;
    }
    capture.capacity = cap;
    capture.sampleRate = currentRate;
    capture.hasRef = capture.hasRef || needRef;
    capture.data[0].resize ((size_t) cap, 0.0f);
    for (int c = 1; c < CaptureBuffer::kChannels; ++c)
        if (capture.hasRef) capture.data[(size_t) c].resize ((size_t) cap, 0.0f);
    envLevel.resize ((size_t) (cap / kEnvBin + 1), -180.0f);
    envThreshold.resize ((size_t) (cap / kEnvBin + 1), -180.0f);
    beatMap.resize ((size_t) (cap / kBeatBin + 2), std::numeric_limits<double>::quiet_NaN());
}

void ThrumaProcessor::clearCapture()
{
    suspendProcessing (true);
    capture.origin.store (-1);
    capture.length.store (0);
    for (auto& d : capture.data) std::fill (d.begin(), d.end(), 0.0f);
    std::fill (envLevel.begin(), envLevel.end(), -180.0f);
    std::fill (envThreshold.begin(), envThreshold.end(), -180.0f);
    std::fill (beatMap.begin(), beatMap.end(), std::numeric_limits<double>::quiet_NaN());
    suspendProcessing (false);
    storedOverview.clear();
    lastLearnLength = 0;
}

void ThrumaProcessor::clearAll()
{
    previewPlaying.store (false);
    dropOfflineAudio();
    clearMidiFile();
    commitTriggerEdit ({}, "Clear", false);
    clearCapture();
    if (auto* src = apvts.getParameter (ids::repSource))
        src->setValueNotifyingHost (src->convertTo0to1 ((float) TriggerSource::Live));
}

float ThrumaProcessor::getCurrentReferenceDb() const
{
    return offlineSource != OfflineSource::None ? offlineRefDb : replace.getScope().referenceDb.load();
}

float ThrumaProcessor::getCurrentThresholdDb() const
{
    if (offlineSource != OfflineSource::None)
    {
        // the analysis' level reference with the current settings (same mapping as the detector), so the line
        // follows a drag right away; the notes follow once the re-analysis is done
        const auto& prof = getProfile ((Instrument) (int) apvts.getRawParameterValue (ids::instrument)->load());
        const float s = apvts.getRawParameterValue (ids::repSens)->load() * 0.01f * 0.8f;
        const float ghost = apvts.getRawParameterValue (ids::repGhost)->load() * 0.01f * 18.0f * prof.ghostWeight;
        return offlineRefDb - std::max (6.0f, prof.levelRangeDb + (s - 0.4f) * 50.0f) - ghost;
    }
    const auto& sc = replace.getScope();
    const auto w = sc.pointsWritten.load (std::memory_order_acquire);
    return w > 0 ? sc.threshold[(size_t) ((w - 1) % DetectionScope::kPoints)].load (std::memory_order_relaxed) : -40.0f;
}

void ThrumaProcessor::updateTrackProperties (const TrackProperties& properties)
{
    // share this track with other Thruma instances under its DAW track name
    juce::WeakReference<ThrumaProcessor> weak (this);
    const juce::String name = properties.name.has_value() ? *properties.name : juce::String();
    juce::MessageManager::callAsync ([weak, name]
    {
        if (auto* self = weak.get(); self != nullptr && name.isNotEmpty() && name != self->trackName)
        {
            self->trackName = name;
            self->setPublishName (name);
        }
    });
}

juce::StringArray ThrumaProcessor::getOtherTrackNames() const
{
    juce::StringArray names;
    for (auto& src : LinkHub::getInstance().listSources())
        if (src.id != publishId.load()) names.add (src.name);
    return names;
}

void ThrumaProcessor::buildStoredOverview()
{
    const auto len = capture.length.load();
    if (len <= 0) return;   // keep whatever overview was restored from state
    const auto bins = (size_t) ((len + kOverviewBin - 1) / kOverviewBin);
    storedOverview.assign (bins * 2, 0.0f);
    const auto& d = capture.data[0];
    for (size_t b = 0; b < bins; ++b)
    {
        float lo = 0.0f, hi = 0.0f;
        const auto s = b * kOverviewBin, e = std::min ((size_t) len, s + kOverviewBin);
        for (size_t i = s; i < e; ++i) { lo = std::min (lo, d[i]); hi = std::max (hi, d[i]); }
        storedOverview[b * 2] = lo;
        storedOverview[b * 2 + 1] = hi;
    }
    storedOverviewOrigin = capture.origin.load();
}

double ThrumaProcessor::beatAt (int64_t pos) const
{
    const auto origin = capture.origin.load();
    if (origin >= 0 && ! beatMap.empty())
    {
        const auto rel = pos - origin;
        const auto b = rel / kBeatBin;
        if (rel >= 0 && b + 1 < (int64_t) beatMap.size() && ! std::isnan (beatMap[(size_t) b]))
        {
            const double frac = (double) (rel - b * kBeatBin) / kBeatBin;
            const double next = beatMap[(size_t) b + 1];
            const double perBin = ! std::isnan (next) ? next - beatMap[(size_t) b] : lastBpm.load() / 60.0 / currentRate * kBeatBin;
            return beatMap[(size_t) b] + frac * perBin;
        }
    }
    return (double) pos / currentRate * lastBpm.load() / 60.0;   // no beat info (own clock / imported audio): constant tempo
}

bool ThrumaProcessor::exportMidi (const juce::File& file) const
{
    const int slot = [this]
    {
        const int s = (int) apvts.getRawParameterValue (ids::repSample)->load() - 1;
        return s >= 0 ? s : (int) apvts.getRawParameterValue (ids::instrument)->load();
    }();
    const auto* kit = kitPublisher.getLatest();
    const int note = kit != nullptr ? kit->slots[(size_t) slot].midiNote : getProfile ((Instrument) slot).defaultMidiNote;

    std::vector<MidiBeatNote> notes;
    for (auto& t : triggers.getAll())
        notes.push_back ({ beatAt (t.position), note, velocityToMidi (t.velocity) });
    const auto bytes = writeMidiFileBeats (notes, lastBpm.load(), 960, 0.1, std::string ("Thruma ") + getInstrumentName ((Instrument) slot));
    file.deleteFile();
    return file.replaceWithData (bytes.data(), bytes.size());
}

void ThrumaProcessor::setFinalized (bool f)
{
    if (auto* src = apvts.getParameter (ids::repSource))
        src->setValueNotifyingHost (src->convertTo0to1 ((float) (f ? TriggerSource::Edited : TriggerSource::Live)));
}

void ThrumaProcessor::importAudio (const juce::File& file, std::function<void (const juce::String&)> onDone)
{
    std::unique_ptr<juce::AudioFormatReader> reader (kitManager.getFormatManager().createReaderFor (file));
    if (reader == nullptr) { if (onDone) onDone ("Could not read " + file.getFileName()); return; }
    if (analysing.load()) { if (onDone) onDone ("Busy - try again in a moment."); return; }

    const double fileRate = reader->sampleRate;
    const auto maxLen = (int64_t) (fileRate * 600.0);   // up to 10 minutes
    const int len = (int) std::min<juce::int64> (reader->lengthInSamples, maxLen);
    juce::AudioBuffer<float> buf ((int) juce::jmax (1u, juce::jmin (2u, reader->numChannels)), len);
    reader->read (&buf, 0, len, 0, true, buf.getNumChannels() > 1);
    std::vector<float> mono ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        float v = 0.0f;
        for (int c = 0; c < buf.getNumChannels(); ++c) v += buf.getSample (c, i);
        mono[(size_t) i] = v / (float) buf.getNumChannels();
    }
    auto audio = std::make_shared<const std::vector<float>> (std::abs (fileRate - currentRate) > 0.5 ? resampleHighQuality (mono, fileRate, currentRate)
                                                                                                    : std::move (mono));
    installOfflineAudio (audio, 0, file.getFileName(), OfflineSource::File, std::move (onDone));
}

void ThrumaProcessor::analyseTrack (std::function<void (const juce::String&)> onDone)
{
    if (analysing.load()) { if (onDone) onDone ("Busy - try again in a moment."); return; }
    if (canReadHostTrack())
    {
        const double hostRate = hostBridge.hostSampleRate() > 0.0 ? hostBridge.hostSampleRate() : currentRate;
        auto mono = hostBridge.readWholeTrack ((int64_t) (hostRate * 600.0));
        if (mono.empty()) { if (onDone) onDone ("This track has no audio to analyse yet."); return; }
        auto audio = std::make_shared<const std::vector<float>> (std::abs (hostRate - currentRate) > 0.5 ? resampleHighQuality (mono, hostRate, currentRate)
                                                                                                        : std::move (mono));
        installOfflineAudio (audio, 0, trackName.isNotEmpty() ? trackName : juce::String ("the track"), OfflineSource::HostTrack, std::move (onDone));
        return;
    }
    // hosts that do not hand plugins the track: analyse everything heard so far
    const auto origin = capture.origin.load();
    const auto len = capture.length.load();
    if (origin < 0 || len <= 0)
    {
        if (onDone) onDone ("This DAW does not let plugins read the whole track. Play the song once, then press Analyze - or use Import Audio.");
        return;
    }
    auto audio = std::make_shared<const std::vector<float>> (capture.data[0].begin(), capture.data[0].begin() + (long) len);
    installOfflineAudio (audio, origin, "what was played", OfflineSource::Heard, std::move (onDone));
}

void ThrumaProcessor::installOfflineAudio (std::shared_ptr<const std::vector<float>> audio, int64_t origin, const juce::String& name,
                                           OfflineSource src, std::function<void (const juce::String&)> onDone)
{
    previewPlaying.store (false);
    offlineAudio = std::move (audio);
    offlineOrigin = origin;
    offlineSource = src;
    previewMode.store (src == OfflineSource::File);
    if (src == OfflineSource::File) previewPos.store (std::max<int64_t> (0, origin));
    offlineName = name;
    analysedKey = {};
    reanalyseAt = 0.0;
    runOfflineAnalysis (true, std::move (onDone));
}

void ThrumaProcessor::dropOfflineAudio()
{
    previewPlaying.store (false);
    previewMode.store (false);
    offlineAudio.reset();
    offlineSource = OfflineSource::None;
    offlineName = {};
    analysedKey = {};
    reanalyseAt = 0.0;
}

juce::String ThrumaProcessor::detectionSettingsKey() const
{
    juce::String k;
    for (auto* id : { ids::instrument, ids::repSens, ids::repGhost, ids::repRetrig, ids::repDouble, ids::repFocus, ids::repDynRange })
        if (auto* v = apvts.getRawParameterValue (id)) k << juce::String (v->load(), 4) << ";";
    return k;
}

void ThrumaProcessor::runOfflineAnalysis (bool initial, std::function<void (const juce::String&)> onDone)
{
    if (offlineAudio == nullptr) return;
    if (analysing.exchange (true)) { if (initial && onDone) onDone ("Busy - try again in a moment."); return; }

    ReplaceSettings rs; DebleedSettings ds; ReferenceSource ref; bool mo;
    readParameters (rs, ds, ref, mo);
    const double sr = currentRate;
    const auto key = detectionSettingsKey();
    const auto audio = offlineAudio;
    const auto origin = offlineOrigin;
    const auto name = offlineName;
    juce::WeakReference<ThrumaProcessor> weak (this);

    workers->addJob ([weak, audio, origin, sr, rs, key, name, initial, onDone]
    {
        auto level = std::make_shared<std::vector<float>>();
        auto thr = std::make_shared<std::vector<float>>();
        float refDb = -150.0f;
        SpectralTemplate tmpl;
        auto found = std::make_shared<std::vector<Trigger>> (analyseTake (audio->data(), (int64_t) audio->size(), sr, rs.detector,
                                                                         &refDb, &tmpl, level.get(), thr.get(), kEnvBin));
        for (auto& t : *found) t.position += origin;
        juce::MessageManager::callAsync ([weak, audio, origin, level, thr, found, key, name, initial, onDone, refDb]
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            self->analysing.store (false);
            if (self->offlineAudio != audio) return;   // superseded (cleared or replaced meanwhile)
            self->analysedKey = key;
            if (refDb > -140.0f) self->offlineRefDb = refDb;

            auto& cap = self->capture;
            const auto len = (int64_t) audio->size();
            if (initial)
            {
                self->suspendProcessing (true);
                if (cap.capacity < len)
                {
                    cap.capacity = len;
                    cap.data[0].assign ((size_t) len, 0.0f);
                    for (int c = 1; c < CaptureBuffer::kChannels; ++c)
                        if (cap.hasRef) cap.data[(size_t) c].assign ((size_t) len, 0.0f);
                    self->envLevel.assign ((size_t) (len / kEnvBin + 1), -180.0f);
                    self->envThreshold.assign ((size_t) (len / kEnvBin + 1), -180.0f);
                    self->beatMap.assign ((size_t) (len / kBeatBin + 2), std::numeric_limits<double>::quiet_NaN());
                }
                if (origin != cap.origin.load())
                    std::fill (self->beatMap.begin(), self->beatMap.end(), std::numeric_limits<double>::quiet_NaN());
                std::copy (audio->begin(), audio->end(), cap.data[0].begin());
                cap.origin.store (origin);
                cap.firstIndex.store (0);
                cap.length.store (len);
                self->suspendProcessing (false);
            }
            std::fill (self->envLevel.begin(), self->envLevel.end(), -180.0f);
            std::fill (self->envThreshold.begin(), self->envThreshold.end(), -180.0f);
            std::copy (level->begin(), level->begin() + (long) std::min (level->size(), self->envLevel.size()), self->envLevel.begin());
            std::copy (thr->begin(), thr->begin() + (long) std::min (thr->size(), self->envThreshold.size()), self->envThreshold.begin());

            juce::String msg;
            if (initial)
            {
                self->commitTriggerEdit (*found, "Analyze", false);
                self->setFinalized (true);   // play back exactly these notes; settings changes re-analyse them
                msg = "Analysed " + name + ": " + juce::String ((int) found->size()) + " notes.";
            }
            else
            {
                // keep what the user edited by hand; replace the rest with the new detection
                std::vector<Trigger> next;
                const auto keepOut = (int64_t) (0.004 * self->currentRate);
                for (const auto& t : self->triggers.getAll()) if (t.locked) next.push_back (t);
                const size_t locked = next.size();
                for (const auto& t : *found)
                {
                    bool clash = false;
                    for (size_t i = 0; i < locked && ! clash; ++i) clash = std::abs (next[i].position - t.position) < keepOut;
                    if (! clash) next.push_back (t);
                }
                std::sort (next.begin(), next.end(), [] (const Trigger& a, const Trigger& b) { return a.position < b.position; });
                self->commitTriggerEdit (std::move (next), "Re-analyse", false);
                msg = "Re-analysed with the new settings: " + juce::String ((int) self->triggers.size()) + " notes.";
            }
            if (onDone) onDone (msg);
        });
    });
}

bool ThrumaProcessor::locateTo (int64_t pos)
{
    pos = std::max<int64_t> (0, pos);
    if (offlineSource == OfflineSource::File)
    {
        previewPos.store (pos);
        playhead.store (pos);
        return true;
    }
    if (hostBridge.locate (pos))
    {
        playhead.store (pos);
        return true;
    }
    return false;
}

void ThrumaProcessor::togglePlay()
{
    if (offlineSource == OfflineSource::File)
    {
        if (previewPlaying.load()) { previewPlaying.store (false); return; }
        const auto origin = std::max<int64_t> (0, capture.origin.load());
        const auto end = origin + capture.length.load();
        if (previewPos.load() < origin || previewPos.load() >= end) previewPos.store (origin);
        hostBridge.keepAwake();
        previewPlaying.store (true);
        return;
    }
    if (hostBridge.isHostPlaying()) hostBridge.stop();
    else hostBridge.play();
}

bool ThrumaProcessor::loadMidiFileData (const juce::MemoryBlock& data, const juce::String& name, juce::String& message)
{
    juce::MemoryInputStream in (data, false);
    juce::MidiFile mf;
    if (! mf.readFrom (in)) { message = "Could not read " + name + " as a MIDI file."; return false; }
    const short tf = mf.getTimeFormat();
    if (tf <= 0) mf.convertTimestampTicksToSeconds();   // SMPTE timing: place by time at the current tempo
    const double bpm = lastBpm.load();
    auto seq = std::make_unique<MidiSequence>();
    for (int tr = 0; tr < mf.getNumTracks(); ++tr)
        for (const auto* e : *mf.getTrack (tr))
            if (e->message.isNoteOn() && e->message.getVelocity() > 0)
            {
                const double t = e->message.getTimeStamp();
                seq->notes.push_back ({ tf > 0 ? t / (double) tf : t * bpm / 60.0, e->message.getNoteNumber(), (int) e->message.getVelocity() });
            }
    std::stable_sort (seq->notes.begin(), seq->notes.end(), [] (const MidiBeatNote& a, const MidiBeatNote& b) { return a.beat < b.beat; });
    if (seq->notes.empty()) { message = name + " has no notes."; return false; }
    const auto count = (int) seq->notes.size();
    midiSeqPublisher.publish (std::move (seq));
    midiFileData = data;
    midiFileName = name;
    message = "Loaded " + name + ": " + juce::String (count) + " notes play the kit with the song (from bar 1).";
    return true;
}

bool ThrumaProcessor::importMidiFile (const juce::File& file, juce::String& message)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data)) { message = "Could not open " + file.getFileName(); return false; }
    return loadMidiFileData (data, file.getFileName(), message);
}

void ThrumaProcessor::clearMidiFile()
{
    if (midiFileName.isEmpty() && midiSeqPublisher.getLatest() == nullptr) return;
    midiSeqPublisher.publish (std::make_unique<MidiSequence>());
    midiFileData.reset();
    midiFileName = {};
}

int ThrumaProcessor::getMidiFileNoteCount() const
{
    const auto* sq = midiSeqPublisher.getLatest();
    return sq != nullptr ? (int) sq->notes.size() : 0;
}

void ThrumaProcessor::previewSample (const juce::File& file)
{
    auto item = std::make_unique<PreviewItem>();
    item->sample = kitManager.playbackSample (file, currentRate);
    item->gain = 0.8f;
    if (item->sample != nullptr) previewPublisher.publish (std::move (item));
}

//==============================================================================
void ThrumaProcessor::setPublishName (const juce::String& name)
{
    auto& hub = LinkHub::getInstance();
    const int old = publishId.exchange (-1);
    if (old >= 0) hub.unregisterSource (old);
    publishName = name.trim();
    if (publishName.isNotEmpty()) publishId.store (hub.registerSource (publishName.toStdString()));
}

void ThrumaProcessor::setLinkedSourceName (const juce::String& name)
{
    linkedSourceName = name;
    linkedId.store (name.isNotEmpty() ? LinkHub::getInstance().findSource (name.toStdString()) : -1);
}

//==============================================================================
void ThrumaProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("THRUMA");
    root.setProperty ("version", 1, nullptr);
    root.appendChild (apvts.copyState(), nullptr);

    juce::ValueTree kit ("KIT");
    kit.setProperty ("json", juce::JSON::toString (kitManager.toVar(), true), nullptr);
    root.appendChild (kit, nullptr);

    juce::ValueTree trig ("TRIGGERS");
    const auto bytes = triggers.serialise();
    trig.setProperty ("data", juce::Base64::toBase64 (bytes.data(), bytes.size()), nullptr);
    if (const auto* lm = liveModelPublisher.getLatest())
    {
        trig.setProperty ("refDb", lm->refDb, nullptr);
        trig.setProperty ("template", templateToString (lm->tmpl), nullptr);
    }
    root.appendChild (trig, nullptr);

    if (const auto* m = debleedModelPublisher.getLatest())
    {
        juce::ValueTree dm ("DEBLEEDMODEL");
        dm.setProperty ("rate", m->sampleRate, nullptr);
        dm.setProperty ("fft", m->fftSize, nullptr);
        dm.setProperty ("hasTransfer", m->hasTransfer, nullptr);
        dm.setProperty ("refDb", m->velocityRefDb, nullptr);
        dm.setProperty ("template", templateToString (m->targetTemplate), nullptr);
        dm.setProperty ("transfer", floatsToBase64 (m->transfer), nullptr);
        dm.setProperty ("floor", floatsToBase64 (m->bleedFloor), nullptr);
        dm.setProperty ("tau", floatsToBase64 (m->decayTau), nullptr);
        root.appendChild (dm, nullptr);
    }

    juce::ValueTree link ("LINK");
    link.setProperty ("publish", publishName, nullptr);
    link.setProperty ("source", linkedSourceName, nullptr);
    root.appendChild (link, nullptr);

    buildStoredOverview();
    if (! storedOverview.empty())
    {
        juce::ValueTree ov ("OVERVIEW");
        ov.setProperty ("origin", (juce::int64) storedOverviewOrigin, nullptr);
        ov.setProperty ("data", floatsToBase64 (storedOverview), nullptr);
        root.appendChild (ov, nullptr);
    }

    if (midiFileData.getSize() > 0)
    {
        juce::ValueTree mf ("MIDIFILE");
        mf.setProperty ("name", midiFileName, nullptr);
        mf.setProperty ("data", midiFileData.toBase64Encoding(), nullptr);
        root.appendChild (mf, nullptr);
    }

    juce::ValueTree ui ("UI");
    ui.setProperty ("tab", uiTab, nullptr);
    root.appendChild (ui, nullptr);

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void ThrumaProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr) return;
    const auto root = juce::ValueTree::fromXml (*xml);
    if (! root.hasType ("THRUMA")) return;

    if (auto params = root.getChildWithName (apvts.state.getType()); params.isValid())
        apvts.replaceState (params);

    if (auto kit = root.getChildWithName ("KIT"); kit.isValid())
    {
        kitManager.fromVar (juce::JSON::parse (kit.getProperty ("json").toString()));
        rebuildKit();   // synchronous: samples are ready before the next (possibly offline) render
    }

    if (auto trig = root.getChildWithName ("TRIGGERS"); trig.isValid())
    {
        juce::MemoryOutputStream mo;
        if (juce::Base64::convertFromBase64 (mo, trig.getProperty ("data").toString()))
            triggers.deserialise (static_cast<const uint8_t*> (mo.getData()), mo.getDataSize());
        publishTriggers();
        if (trig.hasProperty ("refDb"))
        {
            auto lm = std::make_unique<LiveDetectionModel>();
            lm->refDb = (float) trig.getProperty ("refDb");
            lm->tmpl = templateFromString (trig.getProperty ("template").toString());
            liveModelPublisher.publish (std::move (lm));
        }
    }

    if (auto dm = root.getChildWithName ("DEBLEEDMODEL"); dm.isValid())
    {
        auto m = std::make_unique<DebleedModel>();
        m->sampleRate = (double) dm.getProperty ("rate");
        m->fftSize = (int) dm.getProperty ("fft");
        m->hasTransfer = (bool) dm.getProperty ("hasTransfer");
        m->velocityRefDb = (float) dm.getProperty ("refDb");
        m->targetTemplate = templateFromString (dm.getProperty ("template").toString());
        m->transfer = base64ToFloats (dm.getProperty ("transfer").toString());
        m->bleedFloor = base64ToFloats (dm.getProperty ("floor").toString());
        m->decayTau = base64ToFloats (dm.getProperty ("tau").toString());
        if ((int) m->decayTau.size() == m->getNumBins())
        {
            if (! m->matches (currentRate, debleedFftSizeFor (currentRate)))
                *m = m->remapped (currentRate, debleedFftSizeFor (currentRate));
            debleedModelPublisher.publish (std::move (m));
        }
    }

    if (auto link = root.getChildWithName ("LINK"); link.isValid())
    {
        setPublishName (link.getProperty ("publish").toString());
        setLinkedSourceName (link.getProperty ("source").toString());
    }

    if (auto ov = root.getChildWithName ("OVERVIEW"); ov.isValid())
    {
        storedOverview = base64ToFloats (ov.getProperty ("data").toString());
        storedOverviewOrigin = (juce::int64) ov.getProperty ("origin");
    }

    if (auto mf = root.getChildWithName ("MIDIFILE"); mf.isValid())
    {
        juce::MemoryBlock d;
        juce::String msg;
        if (d.fromBase64Encoding (mf.getProperty ("data").toString()))
            loadMidiFileData (d, mf.getProperty ("name").toString(), msg);
    }
    else
        clearMidiFile();

    if (auto ui = root.getChildWithName ("UI"); ui.isValid())
        uiTab = (int) ui.getProperty ("tab");

    undoManager.clearUndoHistory();
    if (prepared) triggerAsyncUpdate();
}

} // namespace thruma

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new thruma::ThrumaProcessor();
}
