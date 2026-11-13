#include "KitManager.h"

namespace thruma
{

KitManager::KitManager()
{
    formats.registerBasicFormats();   // WAV, AIFF (+ FLAC/OGG where JUCE provides them)
    for (int i = 0; i < kNumInstruments; ++i)
        slots[(size_t) i].midiNote = getProfile ((Instrument) i).defaultMidiNote;
    for (int i = kNumInstruments; i < kNumKitSlots; ++i)
        slots[(size_t) i].midiNote = -1;
}

juce::String KitManager::getSlotName (int i) const
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    if (i >= 0 && i < kNumInstruments) return getInstrumentName ((Instrument) i);
    if (! isPadSlot (i)) return {};
    const auto& n = slots[(size_t) i].name;
    return n.isNotEmpty() ? n : "Pad " + juce::String (i - kNumInstruments + 1);
}

void KitManager::setSlotName (int i, const juce::String& name)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    if (isPadSlot (i)) slots[(size_t) i].name = name.trim();
}

int KitManager::getNumUsedPads() const
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    int used = 0;
    for (int p = 0; p < kNumPads; ++p)
        if (! slots[(size_t) (kNumInstruments + p)].isEmpty() || slots[(size_t) (kNumInstruments + p)].name.isNotEmpty()) used = p + 1;
    return used;
}

int KitManager::findFreeMidiNote() const
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    auto used = [this] (int n) { for (auto& s : slots) if (s.midiNote == n) return true; return false; };
    // General MIDI percussion first (cowbell, tambourine, 2nd crash, splash, chinese, ...), then the rest
    for (int n : { 56, 54, 57, 55, 52, 53, 59, 39, 37, 40, 34, 33, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 75, 76, 77, 81 })
        if (! used (n)) return n;
    for (int n = 27; n < 128; ++n) if (! used (n)) return n;
    return -1;
}

void KitManager::givePadANote (int i)
{
    if (isPadSlot (i) && slots[(size_t) i].midiNote < 0) slots[(size_t) i].midiNote = findFreeMidiNote();
}

bool KitManager::isAudioFile (const juce::File& f)
{
    return f.existsAsFile() && f.hasFileExtension ("wav;wave;aif;aiff;flac");
}

KitManager::SlotModel KitManager::getSlot (int i) const
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    return slots[(size_t) juce::jlimit (0, kNumKitSlots - 1, i)];
}

void KitManager::setSlot (int i, const SlotModel& s)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    slots[(size_t) juce::jlimit (0, kNumKitSlots - 1, i)] = s;
}

juce::String KitManager::getKitName() const { std::lock_guard<std::recursive_mutex> l (mutex); return kitName; }
void KitManager::setKitName (const juce::String& n) { std::lock_guard<std::recursive_mutex> l (mutex); kitName = n; }

SamplePtr KitManager::loadSample (const juce::File& f)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    const auto key = f.getFullPathName();
    const auto mtime = f.getLastModificationTime();
    if (auto it = cache.find (key); it != cache.end() && it->second.first == mtime)
        return it->second.second;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr || reader->lengthInSamples <= 0) return nullptr;

    const int len = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 30.0));  // cap 30 s
    const int nch = (int) juce::jmin (2u, reader->numChannels);
    juce::AudioBuffer<float> buf (nch, len);
    reader->read (&buf, 0, len, 0, true, nch > 1);

    std::vector<std::vector<float>> ch ((size_t) nch);
    for (int c = 0; c < nch; ++c)
        ch[(size_t) c].assign (buf.getReadPointer (c), buf.getReadPointer (c) + len);

    auto s = SampleData::create (key.toStdString(), f.getFileNameWithoutExtension().toStdString(), std::move (ch), reader->sampleRate);
    cache[key] = { mtime, s };
    return s;
}

void KitManager::assignFiles (int inst, const juce::Array<juce::File>& files)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    givePadANote (inst);
    std::vector<SamplePtr> samples;
    std::map<const SampleData*, juce::File> back;
    for (auto& f : files)
        if (auto s = loadSample (f)) { samples.push_back (s); back[s.get()] = f; }

    auto layers = buildLayersByLoudness (samples);
    auto& slot = slots[(size_t) inst];
    slot.layers.clear();
    for (auto& L : layers)
    {
        LayerModel m;
        m.velLo = L.velLo;
        m.velHi = L.velHi;
        for (auto& s : L.samples) m.files.add (back[s.get()]);
        slot.layers.push_back (m);
    }
}

void KitManager::addFilesToLayer (int inst, int layer, const juce::Array<juce::File>& files)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    givePadANote (inst);
    auto& slot = slots[(size_t) inst];
    if (layer < 0 || layer >= (int) slot.layers.size()) { addLayer (inst, files); return; }
    for (auto& f : files) slot.layers[(size_t) layer].files.addIfNotAlreadyThere (f);
}

void KitManager::addLayer (int inst, const juce::Array<juce::File>& files)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    givePadANote (inst);
    auto& slot = slots[(size_t) inst];
    LayerModel m;
    m.files = files;
    slot.layers.push_back (m);
    autoDistributeVelocities (inst);
}

void KitManager::removeLayer (int inst, int layer)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    auto& slot = slots[(size_t) inst];
    if (layer >= 0 && layer < (int) slot.layers.size())
        slot.layers.erase (slot.layers.begin() + layer);
}

void KitManager::removeFile (int inst, int layer, int fileIndex)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    auto& slot = slots[(size_t) inst];
    if (layer < 0 || layer >= (int) slot.layers.size()) return;
    slot.layers[(size_t) layer].files.remove (fileIndex);
    if (slot.layers[(size_t) layer].files.isEmpty())
        slot.layers.erase (slot.layers.begin() + layer);
}

void KitManager::autoDistributeVelocities (int inst)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    auto& slot = slots[(size_t) inst];
    const auto n = slot.layers.size();
    for (size_t i = 0; i < n; ++i)
    {
        slot.layers[i].velLo = (float) i / (float) n;
        slot.layers[i].velHi = (float) (i + 1) / (float) n;
    }
}

void KitManager::clearSlot (int inst)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    slots[(size_t) inst].layers.clear();
}

void KitManager::clearAll()
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    for (int i = 0; i < kNumKitSlots; ++i)
    {
        slots[(size_t) i].layers.clear();
        if (isPadSlot (i)) { slots[(size_t) i].name = {}; slots[(size_t) i].midiNote = -1; }
    }
}

int KitManager::importFolder (const juce::File& folder, juce::StringArray* unassigned)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    std::array<juce::Array<juce::File>, kNumInstruments> byInst;
    std::map<juce::String, juce::Array<juce::File>> others;   // sounds that are not one of the drums -> pads
    int count = 0;
    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*.wav;*.wave;*.aif;*.aiff;*.flac"))
    {
        const auto f = entry.getFile();
        // try the file name first, then the parent folder names ("Snare/Top/03.wav")
        auto inst = guessInstrumentFromName (f.getFileNameWithoutExtension().toStdString());
        for (auto p = f.getParentDirectory(); inst == Instrument::Count && p != folder.getParentDirectory() && p.exists(); p = p.getParentDirectory())
        {
            inst = guessInstrumentFromName (p.getFileName().toStdString());
            if (p == folder) break;
        }
        if (inst == Instrument::Count)
        {
            // group by its own folder ("Cowbell/03.wav") or, loose in the kit folder, by name without numbers
            juce::String key = f.getParentDirectory() != folder ? f.getParentDirectory().getFileName()
                                                                 : f.getFileNameWithoutExtension();
            if (f.getParentDirectory() == folder)
            {
                auto words = juce::StringArray::fromTokens (key.replaceCharacters ("_-.", "   "), " ", "");
                juce::StringArray keep;
                for (auto w : words)
                {
                    const auto lw = w.toLowerCase();
                    const bool numberish = w.containsOnly ("0123456789") || (lw.startsWith ("v") && lw.substring (1).containsOnly ("0123456789"))
                                           || (lw.startsWith ("rr") && lw.substring (2).containsOnly ("0123456789"));
                    if (! numberish) keep.add (w.trimCharactersAtEnd ("0123456789"));
                }
                keep.removeEmptyStrings();
                key = keep.joinIntoString (" ");
            }
            if (key.isEmpty()) key = "Pad";
            others[key].add (f);
            ++count;
            continue;
        }
        byInst[(size_t) inst].add (f);
        ++count;
    }
    // Tom names are ambiguous ("tom" -> Tom 1); spread several distinct "Tom" files across toms only if
    // the files themselves carry numbers - otherwise keep them on Tom 1.
    for (int i = 0; i < kNumInstruments; ++i)
        if (! byInst[(size_t) i].isEmpty())
            assignFiles (i, byInst[(size_t) i]);
    // everything else goes onto free pads, named after its folder / file name
    for (auto& [key, files] : others)
    {
        int pad = -1;
        for (int i = kNumInstruments; i < kNumKitSlots && pad < 0; ++i)
            if (slots[(size_t) i].isEmpty()) pad = i;
        if (pad < 0)
        {
            if (unassigned != nullptr) for (auto& f : files) unassigned->add (f.getFullPathName());
            count -= files.size();
            continue;
        }
        slots[(size_t) pad].name = key;
        assignFiles (pad, files);
    }
    return count;
}

juce::var KitManager::toVar() const
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "thruma-kit");
    root->setProperty ("version", 1);
    root->setProperty ("name", kitName);
    juce::Array<juce::var> insts;
    for (int i = 0; i < kNumKitSlots; ++i)
    {
        const auto& s = slots[(size_t) i];
        if (isPadSlot (i) && s.isEmpty() && s.name.isEmpty()) continue;
        auto* o = new juce::DynamicObject();
        if (isPadSlot (i))
        {
            o->setProperty ("pad", i - kNumInstruments + 1);
            o->setProperty ("name", s.name);
        }
        else
            o->setProperty ("instrument", getInstrumentName ((Instrument) i));
        o->setProperty ("midiNote", s.midiNote);
        o->setProperty ("roundRobin", s.rrMode == RoundRobinMode::Cycle ? "cycle" : "random");
        juce::Array<juce::var> layers;
        for (auto& L : s.layers)
        {
            auto* lo = new juce::DynamicObject();
            lo->setProperty ("velLo", L.velLo);
            lo->setProperty ("velHi", L.velHi);
            juce::Array<juce::var> files;
            for (auto& f : L.files) files.add (f.getFullPathName());
            lo->setProperty ("files", files);
            layers.add (juce::var (lo));
        }
        o->setProperty ("layers", layers);
        insts.add (juce::var (o));
    }
    root->setProperty ("instruments", insts);
    return juce::var (root);
}

void KitManager::fromVar (const juce::var& v)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    if (! v.isObject()) return;
    for (int i = 0; i < kNumKitSlots; ++i)
    {
        slots[(size_t) i] = {};
        slots[(size_t) i].midiNote = i < kNumInstruments ? getProfile ((Instrument) i).defaultMidiNote : -1;
    }
    kitName = v.getProperty ("name", "Untitled Kit").toString();
    if (auto* insts = v.getProperty ("instruments", {}).getArray())
    {
        for (auto& iv : *insts)
        {
            const auto name = iv.getProperty ("instrument", "").toString();
            int idx = -1;
            for (int i = 0; i < kNumInstruments; ++i)
                if (name == getInstrumentName ((Instrument) i)) idx = i;
            if (const int pad = (int) iv.getProperty ("pad", 0); pad >= 1 && pad <= kNumPads)
            {
                idx = kNumInstruments + pad - 1;
                slots[(size_t) idx].name = iv.getProperty ("name", "").toString();
            }
            if (idx < 0) continue;
            auto& s = slots[(size_t) idx];
            s.midiNote = juce::jlimit (0, 127, (int) iv.getProperty ("midiNote", s.midiNote));
            s.rrMode = iv.getProperty ("roundRobin", "cycle").toString() == "random" ? RoundRobinMode::Random : RoundRobinMode::Cycle;
            if (auto* layers = iv.getProperty ("layers", {}).getArray())
            {
                for (auto& lv : *layers)
                {
                    LayerModel m;
                    m.velLo = (float) (double) lv.getProperty ("velLo", 0.0);
                    m.velHi = (float) (double) lv.getProperty ("velHi", 1.0);
                    if (auto* files = lv.getProperty ("files", {}).getArray())
                        for (auto& fv : *files) m.files.add (juce::File (fv.toString()));
                    s.layers.push_back (m);
                }
            }
        }
    }
}

bool KitManager::saveKitFile (const juce::File& f) const
{
    return f.replaceWithText (juce::JSON::toString (toVar(), false));
}

bool KitManager::loadKitFile (const juce::File& f)
{
    const auto v = juce::JSON::parse (f.loadFileAsString());
    if (! v.isObject() || v.getProperty ("format", "").toString() != "thruma-kit") return false;
    fromVar (v);
    return true;
}

SamplePtr KitManager::playbackSample (const juce::File& f, double rate)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    auto native = loadSample (f);
    if (native == nullptr) return nullptr;
    if (std::abs (native->playRate - rate) < 1e-6) return native;
    const auto key = f.getFullPathName() + "|" + juce::String (rate) + "|" + juce::String (f.getLastModificationTime().toMilliseconds());
    if (auto it = playCache.find (key); it != playCache.end()) return it->second;
    SamplePtr s = native->withPlaybackRate (rate);
    playCache[key] = s;
    return s;
}

std::unique_ptr<Kit> KitManager::buildKit (double rate, juce::StringArray* missing)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    auto kit = std::make_unique<Kit>();
    kit->name = kitName.toStdString();
    kit->playRate = rate;
    auto& native = *kit;
    for (int i = 0; i < kNumKitSlots; ++i)
    {
        const auto& m = slots[(size_t) i];
        auto& slot = native.slots[(size_t) i];
        slot.midiNote = m.midiNote;
        slot.rrMode = m.rrMode;
        for (auto& L : m.layers)
        {
            VelocityLayer layer;
            layer.velLo = L.velLo;
            layer.velHi = L.velHi;
            for (auto& f : L.files)
            {
                if (auto s = playbackSample (f, rate)) layer.samples.push_back (s);
                else if (missing != nullptr) missing->add (f.getFullPathName());
            }
            if (! layer.samples.empty()) slot.layers.push_back (std::move (layer));
        }
        slot.finalise();
    }
    return kit;
}

} // namespace thruma
