#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstring>

namespace thruma
{

namespace
{
    constexpr double kCaptureSeconds = 240.0;   // 4 minutes of target + reference capture

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
    for (auto* id : { ids::dbOn, ids::dbReference, ids::repLatency })
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
    for (auto* id : { ids::dbOn, ids::dbReference, ids::repLatency })
        apvts.removeParameterListener (id, this);
    if (publishId.load() >= 0) LinkHub::getInstance().unregisterSource (publishId.load());
}

//==============================================================================
bool ThrumaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()) return false;
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
    mainChannels = juce::jlimit (1, 2, getMainBusNumInputChannels());
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
}

void ThrumaProcessor::configureEngines()
{
    const int latencyChoice = (int) apvts.getRawParameterValue (ids::repLatency)->load();
    configuredLookaheadMs = latencyChoice == 0 ? 10.0f : 3.0f;
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
    if (! prepared || analysing.load()) return;
    const int inst = (int) apvts.getRawParameterValue (ids::instrument)->load();
    if (inst != lastLearnInstrument) { lastLearnInstrument = inst; lastLearnLength = 0; }

    const auto len = capture.length.load();
    const auto interval = (int64_t) ((lastLearnLength == 0 ? 6.0 : 20.0) * currentRate);
    if (len - lastLearnLength < interval) return;
    lastLearnLength = len;
    analysing.store (true);

    const auto window = std::min<int64_t> (len, (int64_t) (90.0 * currentRate));
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

    juce::Optional<juce::AudioPlayHead::PositionInfo> pos;
    if (auto* ph = getPlayHead()) pos = ph->getPosition();
    const bool playing = pos.hasValue() && pos->getIsPlaying();
    const int64_t timeline = pos.hasValue() && pos->getTimeInSamples().hasValue() ? *pos->getTimeInSamples() : internalTimeline;
    if (pos.hasValue() && pos->getBpm().hasValue()) lastBpm.store (*pos->getBpm());
    transportPlaying.store (playing);
    playhead.store (timeline);

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
    const bool playing = pos.hasValue() && pos->getIsPlaying();

    auto mainBus = getBusBuffer (buffer, true, 0);
    const int nch = juce::jlimit (1, 2, juce::jmin (mainBus.getNumChannels(), mainChannels));
    if (mainBus.getNumChannels() == 0) return;
    float* ch[2] = { mainBus.getWritePointer (0, start), nch > 1 ? mainBus.getWritePointer (1, start) : nullptr };

    int scCh = 0;
    const float* sc[2] = { nullptr, nullptr };
    if (getBusCount (true) > 1 && getBus (true, 1)->isEnabled())
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
        replace.getDetector().setVelocityReference (lm->object->refDb);
        replace.getDetector().setSpectralTemplate (lm->object->tmpl);
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

    if (playing && capture.capacity > 0)
    {
        auto origin = capture.origin.load (std::memory_order_relaxed);
        if (origin < 0)
        {
            // start a little before where playback first started, so an earlier restart is still covered
            origin = std::max<int64_t> (0, chunkTimeline - (int64_t) (10.0 * currentRate));
            if (origin > chunkTimeline) origin = chunkTimeline;
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

    // --- auditioning / previews (always audible at unity) ---
    if (const int req = auditionRequestIdx.exchange (-1); req >= 0)
    {
        auditionPos = req;
        auditionLen = auditionLeft = (int) std::min<int64_t> ((int64_t) (0.35 * currentRate), capture.length.load() - req);
    }
    if (auditionLeft > 0 && auditionPos >= 0 && ! playing)
    {
        const int fade = juce::jmin (64, auditionLen / 4);
        const int m = juce::jmin (n, auditionLeft);
        for (int i = 0; i < m; ++i)
        {
            const int done = auditionLen - auditionLeft;
            float g = 1.0f;
            if (done < fade) g = (float) done / (float) fade;
            if (auditionLeft < fade) g = std::min (g, (float) auditionLeft / (float) fade);
            const float v = capture.data[0][(size_t) auditionPos] * g;
            for (int c = 0; c < nch; ++c) ch[c][i] += v;
            ++auditionPos;
            --auditionLeft;
        }
    }
    if (const int inst = auditionInstrument.exchange (-1); inst >= 0 && kit != nullptr)
        previewEngine.trigger (*kit, kitSeq, (Instrument) inst, auditionVelocity.load(), 0, rs.playback);
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
        if (auto* src = apvts.getParameter (ids::repSource);
            src != nullptr && (int) apvts.getRawParameterValue (ids::repSource)->load() == (int) TriggerSource::Live)
            src->setValueNotifyingHost (src->convertTo0to1 ((float) TriggerSource::Edited));
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
    // called from prepareToPlay / configureEngines, i.e. never concurrently with processBlock
    const bool needRef = isSidechainConnected() || configuredLinked;
    const auto cap = (int64_t) (kCaptureSeconds * currentRate);
    if (capture.capacity == cap && capture.hasRef == needRef && std::abs (capture.sampleRate - currentRate) < 0.5) return;

    capture.capacity = cap;
    capture.sampleRate = currentRate;
    capture.hasRef = needRef;
    capture.data[0].assign ((size_t) cap, 0.0f);
    for (int c = 1; c < CaptureBuffer::kChannels; ++c)
    {
        if (needRef) capture.data[(size_t) c].assign ((size_t) cap, 0.0f);
        else { capture.data[(size_t) c].clear(); capture.data[(size_t) c].shrink_to_fit(); }
    }
    capture.origin.store (-1);
    capture.length.store (0);
    lastLearnLength = 0;
}

void ThrumaProcessor::clearCapture()
{
    suspendProcessing (true);
    capture.origin.store (-1);
    capture.length.store (0);
    for (auto& d : capture.data) std::fill (d.begin(), d.end(), 0.0f);
    suspendProcessing (false);
    storedOverview.clear();
    lastLearnLength = 0;
}

void ThrumaProcessor::clearAll()
{
    commitTriggerEdit ({}, "Clear", false);
    clearCapture();
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

void ThrumaProcessor::analyseCapture (std::function<void (int)> onDone)
{
    const auto len = capture.length.load();
    if (len <= 0 || analysing.exchange (true)) { if (onDone) onDone (-1); return; }

    auto audio = std::make_shared<std::vector<float>> (capture.data[0].begin(), capture.data[0].begin() + len);
    const auto origin = capture.origin.load();
    ReplaceSettings rs; DebleedSettings ds; ReferenceSource ref; bool mo;
    readParameters (rs, ds, ref, mo);
    const auto settings = rs.detector;
    const double sr = currentRate;
    juce::WeakReference<ThrumaProcessor> weak (this);

    workers->addJob ([weak, audio, origin, settings, sr, onDone]
    {
        float refDb = -150.0f;
        SpectralTemplate tmpl;
        auto found = analyseTake (audio->data(), (int64_t) audio->size(), sr, settings, &refDb, &tmpl);
        for (auto& t : found) t.position += origin;

        juce::MessageManager::callAsync ([weak, found = std::move (found), refDb, tmpl, onDone]() mutable
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            self->commitTriggerEdit (found, "Re-detect", false);
            auto lm = std::make_unique<LiveDetectionModel>();
            lm->refDb = refDb;
            lm->tmpl = tmpl;
            self->liveModelPublisher.publish (std::move (lm));
            self->analysing.store (false);
            if (onDone) onDone ((int) found.size());
        });
    });
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

    std::vector<MidiNoteEvent> notes;
    for (auto& t : triggers.getAll())
        notes.push_back ({ t.position, note, velocityToMidi (t.velocity) });
    const auto bytes = writeMidiFile (notes, currentRate, lastBpm.load(), 960, 0.05,
                                      std::string ("Thruma ") + getInstrumentName ((Instrument) slot));
    return file.replaceWithData (bytes.data(), bytes.size());
}

void ThrumaProcessor::auditionCapture (int64_t timelinePos, double)
{
    const auto origin = capture.origin.load();
    const auto idx = timelinePos - origin;
    if (origin < 0 || idx < 0 || idx >= capture.length.load()) return;
    auditionRequestIdx.store ((int) idx);
}

void ThrumaProcessor::auditionReplacement (float velocity)
{
    const int s = (int) apvts.getRawParameterValue (ids::repSample)->load() - 1;
    auditionVelocity.store (velocity);
    auditionInstrument.store (s >= 0 ? s : (int) apvts.getRawParameterValue (ids::instrument)->load());
}

void ThrumaProcessor::previewSample (const juce::File& file)
{
    auto item = std::make_unique<PreviewItem>();
    item->sample = kitManager.playbackSample (file, currentRate);
    item->gain = 0.8f;
    if (item->sample != nullptr) previewPublisher.publish (std::move (item));
}

//==============================================================================
void ThrumaProcessor::analyseDebleed (std::function<void (const juce::String&)> onDone)
{
    const auto len = capture.length.load();
    if (len <= 0 || analysing.exchange (true)) { if (onDone) onDone ("Capture some playback first (arm Capture on the Replace page)."); return; }

    auto target = std::make_shared<std::vector<float>> (capture.data[0].begin(), capture.data[0].begin() + len);
    auto refs = std::make_shared<std::vector<std::vector<float>>>();
    juce::StringArray names;
    if (capture.hasRef)
    {
        const bool linked = configuredLinked && ! isSidechainConnected();
        refs->emplace_back (capture.data[1].begin(), capture.data[1].begin() + len);
        names.add (linked ? "Linked instance" : "Sidechain L");
        if (! linked && sidechainChannels.load() > 1)
        {
            refs->emplace_back (capture.data[2].begin(), capture.data[2].begin() + len);
            names.add ("Sidechain R");
            std::vector<float> sum ((size_t) len);
            for (size_t i = 0; i < (size_t) len; ++i) sum[i] = 0.5f * ((*refs)[0][i] + (*refs)[1][i]);
            refs->push_back (std::move (sum));
            names.add ("Sidechain L+R");
        }
    }
    const auto inst = (Instrument) (int) apvts.getRawParameterValue (ids::instrument)->load();
    const float sens = apvts.getRawParameterValue (ids::dbSens)->load() * 0.01f;
    const double sr = currentRate;
    juce::WeakReference<ThrumaProcessor> weak (this);

    workers->addJob ([weak, target, refs, names, inst, sens, sr, onDone]
    {
        std::vector<const float*> cands;
        for (auto& r : *refs) cands.push_back (r.data());
        auto result = thruma::analyseDebleed (target->data(), (int) target->size(), cands, sr, inst, sens);

        juce::MessageManager::callAsync ([weak, result, names, onDone]
        {
            auto* self = weak.get();
            if (self == nullptr) return;
            self->analysing.store (false);

            juce::String report;
            report << "Analyzed " << result.numHits << " " << getInstrumentName ((Instrument) (int) self->apvts.getRawParameterValue (ids::instrument)->load())
                   << " hits. Bleed level " << juce::String (result.bleedLevelDb, 1) << " dB.";
            int refChoice = 0;
            if (result.bestReference >= 0)
            {
                const auto& nm = names[result.bestReference];
                report << " Best reference: " << nm << " (coherence " << juce::String (result.referenceScores[(size_t) result.bestReference], 2) << ").";
                refChoice = nm == "Sidechain L" ? 1 : nm == "Sidechain R" ? 2 : nm == "Sidechain L+R" ? 3 : 4;
            }
            else if (! names.isEmpty())
            {
                report << " No reference carries enough of the bleed - using blind mode.";
            }

            self->debleedModelPublisher.publish (std::make_unique<DebleedModel> (result.model));
            auto setParam = [self] (const char* id, float plainValue)
            {
                if (auto* p = self->apvts.getParameter (id))
                    p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
            };
            setParam (ids::dbAmount, result.suggestedAmount * 100.0f);
            if (! names.isEmpty()) setParam (ids::dbReference, (float) refChoice);
            self->debleedReport = report;
            if (onDone) onDone (report);
        });
    });
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
        dm.setProperty ("report", debleedReport, nullptr);
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
        debleedReport = dm.getProperty ("report").toString();
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
