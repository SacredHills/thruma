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

void KitManager::setMix (int i, float volumeDb, float pan, bool mute, bool solo)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    if (i < 0 || i >= kNumKitSlots) return;
    auto& s = slots[(size_t) i];
    s.volumeDb = juce::jlimit (-100.0f, 12.0f, volumeDb);
    s.pan = juce::jlimit (-1.0f, 1.0f, pan);
    s.mute = mute;
    s.solo = solo;
}

void KitManager::setOutput (int i, int output)
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    if (i >= 0 && i < kNumKitSlots) slots[(size_t) i].output = juce::jmax (0, output);
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
    const auto key = f.getFullPathName();
    const auto mtime = f.getLastModificationTime();
    {
        std::lock_guard<std::recursive_mutex> l (mutex);
        if (auto it = cache.find (key); it != cache.end() && it->second.first == mtime) { tick(); return it->second.second; }
    }
    // decode without holding the lock (the UI keeps reading the kit while samples load)
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    tick();
    if (reader == nullptr || reader->lengthInSamples <= 0) return nullptr;

    const int len = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 30.0));  // cap 30 s
    const int nch = (int) juce::jmin (2u, reader->numChannels);
    juce::AudioBuffer<float> buf (nch, len);
    reader->read (&buf, 0, len, 0, true, nch > 1);

    std::vector<std::vector<float>> ch ((size_t) nch);
    for (int c = 0; c < nch; ++c)
        ch[(size_t) c].assign (buf.getReadPointer (c), buf.getReadPointer (c) + len);

    auto s = SampleData::create (key.toStdString(), f.getFileNameWithoutExtension().toStdString(), std::move (ch), reader->sampleRate);
    std::lock_guard<std::recursive_mutex> l (mutex);
    cache[key] = { mtime, s };
    return s;
}

void KitManager::assignFiles (int inst, const juce::Array<juce::File>& files)
{
    std::vector<SamplePtr> samples;
    std::map<const SampleData*, juce::File> back;
    for (auto& f : files)   // loading happens outside the lock
        if (auto s = loadSample (f)) { samples.push_back (s); back[s.get()] = f; }
    auto layers = buildLayersByLoudness (samples);

    std::lock_guard<std::recursive_mutex> l (mutex);
    givePadANote (inst);
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
    if (inst < 0 || inst >= kNumKitSlots) return;
    if (! isPadSlot (inst)) { slots[(size_t) inst].layers.clear(); return; }
    for (int i = inst; i + 1 < kNumKitSlots; ++i) slots[(size_t) i] = slots[(size_t) (i + 1)];
    slots[(size_t) (kNumKitSlots - 1)] = {};
    slots[(size_t) (kNumKitSlots - 1)].midiNote = -1;
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
    if (getProgressPhase() != LoadPhase::None) setProgressTotal (count);
    for (int i = 0; i < kNumInstruments; ++i)
        if (! byInst[(size_t) i].isEmpty())
            assignFiles (i, byInst[(size_t) i]);
    // everything else goes onto free pads, named after its folder / file name
    for (auto& [key, files] : others)
    {
        int pad = -1;
        {
            std::lock_guard<std::recursive_mutex> l (mutex);
            for (int i = kNumInstruments; i < kNumKitSlots && pad < 0; ++i)
                if (slots[(size_t) i].isEmpty() && slots[(size_t) i].name.isEmpty()) pad = i;
            if (pad >= 0) slots[(size_t) pad].name = key;
        }
        if (pad < 0)
        {
            if (unassigned != nullptr) for (auto& f : files) unassigned->add (f.getFullPathName());
            count -= files.size();
            continue;
        }
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
        o->setProperty ("volumeDb", s.volumeDb);
        o->setProperty ("pan", s.pan);
        if (s.mute) o->setProperty ("mute", true);
        if (s.solo) o->setProperty ("solo", true);
        if (s.output > 0) o->setProperty ("output", s.output);
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
            s.volumeDb = juce::jlimit (-100.0f, 12.0f, (float) (double) iv.getProperty ("volumeDb", 0.0));
            s.pan = juce::jlimit (-1.0f, 1.0f, (float) (double) iv.getProperty ("pan", 0.0));
            s.mute = (bool) iv.getProperty ("mute", false);
            s.solo = (bool) iv.getProperty ("solo", false);
            s.output = juce::jmax (0, (int) iv.getProperty ("output", 0));
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

namespace
{
    // Lossless FLAC copy of a sample, for TDE files. Returns false (store the original file)
    // when FLAC can't hold the data bit-exactly (float or 32-bit samples) or isn't smaller.
    bool encodeFlac (juce::AudioFormatManager& formats, const juce::File& f, juce::MemoryBlock& out)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
        if (reader == nullptr || reader->usesFloatingPointData || reader->lengthInSamples <= 0) return false;
        const int bits = (int) reader->bitsPerSample;
        if (bits != 16 && bits != 24) return false;
        if (reader->numChannels < 1 || reader->numChannels > 8) return false;

        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (out, false);
        auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (reader->sampleRate)
                                                       .withNumChannels ((int) reader->numChannels)
                                                       .withBitsPerSample (bits)
                                                       .withQualityOptionIndex (5));
        if (writer == nullptr) return false;
        const bool ok = writer->writeFromAudioReader (*reader, 0, -1);
        writer.reset();   // flushes into 'out'
        return ok && out.getSize() > 0 && (juce::int64) out.getSize() < f.getSize();
    }
}

juce::File KitManager::getTdeLibraryFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Thruma").getChildFile ("TDE");
}

bool KitManager::saveSlotTde (int slot, const juce::File& dest, juce::String& error)
{
    const auto m = getSlot (slot);
    if (m.isEmpty()) { error = "This drum has no samples to save."; return false; }
    juce::ZipFile::Builder zip;
    auto* root = new juce::DynamicObject();
    root->setProperty ("format", "thruma-tde");
    root->setProperty ("version", 2);   // 2: samples may be FLAC (lossless); 1 = WAV only
    root->setProperty ("name", isPadSlot (slot) ? getSlotName (slot) : dest.getFileNameWithoutExtension());
    root->setProperty ("drum", slot < kNumInstruments ? juce::String (getInstrumentName ((Instrument) slot)) : juce::String ("Pad"));
    root->setProperty ("roundRobin", m.rrMode == RoundRobinMode::Random ? "random" : "cycle");
    juce::Array<juce::var> layers;
    int n = 0;
    for (size_t l = 0; l < m.layers.size(); ++l)
    {
        auto* lo = new juce::DynamicObject();
        lo->setProperty ("velLo", m.layers[l].velLo);
        lo->setProperty ("velHi", m.layers[l].velHi);
        juce::Array<juce::var> files;
        for (auto& f : m.layers[l].files)
        {
            if (! f.existsAsFile()) { error = "Missing sample: " + f.getFullPathName(); return false; }
            const auto prefix = "samples/" + juce::String (++n).paddedLeft ('0', 3) + "_";
            juce::MemoryBlock flacData;
            if (encodeFlac (formats, f, flacData))
            {
                // Lossless: decodes to exactly the same samples as the original, about half the size.
                const auto stored = prefix + f.getFileNameWithoutExtension() + ".flac";
                zip.addEntry (new juce::MemoryInputStream (std::move (flacData)), 0, stored, f.getLastModificationTime());
                files.add (stored);
            }
            else
            {
                const auto stored = prefix + f.getFileName();
                zip.addFile (f, 6, stored);   // the original file, unchanged
                files.add (stored);
            }
        }
        lo->setProperty ("files", files);
        layers.add (juce::var (lo));
    }
    root->setProperty ("layers", layers);
    const auto json = juce::JSON::toString (juce::var (root), false);
    zip.addEntry (new juce::MemoryInputStream (json.toRawUTF8(), json.getNumBytesAsUTF8(), true), 6, "thruma-tde.json", juce::Time::getCurrentTime());

    auto out = dest.withFileExtension ("tde");
    juce::TemporaryFile tmp (out);
    {
        juce::FileOutputStream os (tmp.getFile());
        if (! os.openedOk() || ! zip.writeToStream (os, nullptr)) { error = "Could not write " + out.getFileName(); return false; }
    }
    if (! tmp.overwriteTargetFileWithTemporary()) { error = "Could not write " + out.getFileName(); return false; }
    return true;
}

namespace
{
    bool readTdeHeader (juce::ZipFile& zip, juce::String& json, juce::var& v)
    {
        const int idx = zip.getIndexOfFileName ("thruma-tde.json");
        if (idx < 0) return false;
        if (std::unique_ptr<juce::InputStream> in { zip.createStreamForEntry (idx) }) json = in->readEntireStreamAsString();
        v = juce::JSON::parse (json);
        return v.getProperty ("format", "").toString() == "thruma-tde";
    }

    int countTdeSamples (const juce::var& v)
    {
        int n = 0;
        if (auto* ls = v.getProperty ("layers", {}).getArray())
            for (auto& lv : *ls)
                if (auto* fs = lv.getProperty ("files", {}).getArray()) n += fs->size();
        return n;
    }
}

int KitManager::saveAllTde (const juce::File& folder, juce::StringArray& errors)
{
    if (! folder.createDirectory()) { errors.add ("Could not create " + folder.getFullPathName()); return 0; }
    const auto kit = getKitName().trim();
    juce::StringArray usedNames;
    int saved = 0;
    for (int slot = 0; slot < kNumKitSlots; ++slot)
    {
        if (getSlot (slot).isEmpty()) continue;
        auto base = juce::File::createLegalFileName ((kit.isNotEmpty() ? kit + " - " : juce::String()) + getSlotName (slot));
        auto name = base;
        for (int k = 2; usedNames.contains (name, true); ++k) name = base + " " + juce::String (k);
        usedNames.add (name);
        juce::String err;
        if (saveSlotTde (slot, folder.getChildFile (name + ".tde"), err)) ++saved;
        else errors.add (getSlotName (slot) + ": " + err);
    }
    if (saved == 0 && errors.isEmpty()) errors.add ("The kit has no samples to save.");
    return saved;
}

int KitManager::loadTdeFiles (const juce::Array<juce::File>& files, juce::StringArray& errors)
{
    struct Item { juce::File file; int slot = -1; };
    std::vector<Item> items;
    std::array<bool, kNumKitSlots> taken {};
    int total = 0;
    int nextPad = kNumInstruments + getNumUsedPads();
    for (auto& f : files)
    {
        juce::ZipFile zip (f);
        juce::String json; juce::var v;
        if (! readTdeHeader (zip, json, v)) { errors.add (f.getFileName() + " is not a TDE file."); continue; }
        int slot = -1;
        const auto drum = v.getProperty ("drum", "").toString();
        for (int i = 0; i < kNumInstruments; ++i)
            if (drum.equalsIgnoreCase (getInstrumentName ((Instrument) i)) && ! taken[(size_t) i]) slot = i;
        if (slot < 0)
        {
            if (nextPad >= kNumKitSlots) { errors.add (f.getFileName() + ": no free pads left."); continue; }
            slot = nextPad++;
        }
        taken[(size_t) slot] = true;
        total += countTdeSamples (v);
        items.push_back ({ f, slot });
    }
    if (getProgressPhase() != LoadPhase::None) setProgressTotal (total);
    int loaded = 0;
    for (auto& it : items)
    {
        juce::String err;
        if (loadSlotTde (it.slot, it.file, err, false)) ++loaded;
        else errors.add (err);
    }
    return loaded;
}

bool KitManager::loadSlotTde (int slot, const juce::File& tde, juce::String& error, bool setProgress)
{
    juce::ZipFile zip (tde);
    juce::String json; juce::var v;
    if (! readTdeHeader (zip, json, v)) { error = tde.getFileName() + " is not a TDE file."; return false; }

    // unpack once into the library (same file again: reuse it)
    const auto id = juce::String::toHexString ((juce::int64) (json.hashCode64() ^ tde.getSize()));
    const auto folder = getTdeLibraryFolder().getChildFile (juce::File::createLegalFileName (tde.getFileNameWithoutExtension()) + "_" + id);
    if (! folder.getChildFile ("thruma-tde.json").existsAsFile())
    {
        folder.createDirectory();
        if (zip.uncompressTo (folder, true).failed()) { error = "Could not unpack " + tde.getFileName(); return false; }
    }

    std::vector<LayerModel> layers;
    juce::Array<juce::File> all;
    if (auto* ls = v.getProperty ("layers", {}).getArray())
        for (auto& lv : *ls)
        {
            LayerModel lm;
            lm.velLo = (float) (double) lv.getProperty ("velLo", 0.0);
            lm.velHi = (float) (double) lv.getProperty ("velHi", 1.0);
            if (auto* fs = lv.getProperty ("files", {}).getArray())
                for (auto& fv : *fs)
                {
                    const auto f = folder.getChildFile (fv.toString());
                    if (f.existsAsFile()) { lm.files.add (f); all.add (f); }
                }
            if (! lm.files.isEmpty()) layers.push_back (lm);
        }
    if (layers.empty()) { error = tde.getFileName() + " has no samples."; return false; }
    if (setProgress && getProgressPhase() != LoadPhase::None) setProgressTotal (all.size());
    for (auto& f : all) loadSample (f);   // decode now (counts on the loading bar), outside the lock

    std::lock_guard<std::recursive_mutex> l (mutex);
    givePadANote (slot);
    auto& s = slots[(size_t) slot];
    s.layers = layers;   // exactly the saved velocity ranges
    s.rrMode = v.getProperty ("roundRobin", "cycle").toString() == "random" ? RoundRobinMode::Random : RoundRobinMode::Cycle;
    if (isPadSlot (slot) && s.name.isEmpty()) s.name = v.getProperty ("name", tde.getFileNameWithoutExtension()).toString();
    return true;
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
    auto native = loadSample (f);
    if (native == nullptr) return nullptr;
    if (std::abs (native->playRate - rate) < 1e-6) return native;
    const auto key = f.getFullPathName() + "|" + juce::String (rate) + "|" + juce::String (f.getLastModificationTime().toMilliseconds());
    {
        std::lock_guard<std::recursive_mutex> l (mutex);
        if (auto it = playCache.find (key); it != playCache.end()) return it->second;
    }
    SamplePtr s = native->withPlaybackRate (rate);   // outside the lock
    std::lock_guard<std::recursive_mutex> l (mutex);
    playCache[key] = s;
    return s;
}

int KitManager::countFiles() const
{
    std::lock_guard<std::recursive_mutex> l (mutex);
    int n = 0;
    for (auto& s : slots) for (auto& L : s.layers) n += L.files.size();
    return n;
}

std::unique_ptr<Kit> KitManager::buildKit (double rate, juce::StringArray* missing)
{
    std::array<SlotModel, kNumKitSlots> model;
    juce::String name;
    {
        std::lock_guard<std::recursive_mutex> l (mutex);   // copy the model; load the samples without the lock
        model = slots;
        name = kitName;
    }
    auto kit = std::make_unique<Kit>();
    kit->name = name.toStdString();
    kit->playRate = rate;
    auto& native = *kit;
    for (int i = 0; i < kNumKitSlots; ++i)
    {
        const auto& m = model[(size_t) i];
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
