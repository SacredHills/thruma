// Editable sample-kit model (message thread). Loads standard WAV/AIFF files with JUCE, never
// modifies or copies the originals, and builds immutable thruma::Kit snapshots for the audio thread.
#pragma once

#include "Kit.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <map>
#include <mutex>

namespace thruma
{

class KitManager
{
public:
    struct LayerModel
    {
        float velLo = 0.0f, velHi = 1.0f;
        juce::Array<juce::File> files;
    };

    struct SlotModel
    {
        std::vector<LayerModel> layers;
        RoundRobinMode rrMode = RoundRobinMode::Cycle;
        int midiNote = 36;          // pads: -1 = none yet
        juce::String name;          // pads only (empty = "Pad N")
        bool isEmpty() const { for (auto& l : layers) if (! l.files.isEmpty()) return false; return true; }
    };

    KitManager();

    // --- model access (guarded; copy out for UI display) ---
    SlotModel getSlot (int instrument) const;
    void setSlot (int instrument, const SlotModel& s);
    /** "Snare", or a pad's name ("Cowbell", "Pad 3"). */
    juce::String getSlotName (int slot) const;
    void setSlotName (int slot, const juce::String& name);
    /** Pads in use (highest used pad + 1, counted from 0). */
    int getNumUsedPads() const;
    /** A MIDI note no other slot uses (prefers General MIDI percussion notes). */
    int findFreeMidiNote() const;
    juce::String getKitName() const;
    void setKitName (const juce::String& n);

    // --- editing helpers ---
    /** Sort files into velocity layers by measured loudness (round-robin groups of similar level). */
    void assignFiles (int instrument, const juce::Array<juce::File>& files);
    void addFilesToLayer (int instrument, int layer, const juce::Array<juce::File>& files);
    void addLayer (int instrument, const juce::Array<juce::File>& files);
    void removeLayer (int instrument, int layer);
    void removeFile (int instrument, int layer, int fileIndex);
    void autoDistributeVelocities (int instrument);
    void clearSlot (int instrument);
    void clearAll();

    /** Import every WAV in a folder tree, assigning files to instruments by name. Returns files assigned. */
    int importFolder (const juce::File& folder, juce::StringArray* unassigned = nullptr);

    // --- persistence (JSON referencing the original files) ---
    juce::var toVar() const;
    void fromVar (const juce::var& v);
    bool saveKitFile (const juce::File& f) const;
    bool loadKitFile (const juce::File& f);

    // --- audio ---
    /** Load (cached) a file at its native rate. Returns nullptr if unreadable. */
    SamplePtr loadSample (const juce::File& f);
    /** Load (cached) a file converted to the playback rate. */
    SamplePtr playbackSample (const juce::File& f, double rate);
    /** Build an immutable kit at the given playback rate. Missing files are listed, not fatal. */
    std::unique_ptr<Kit> buildKit (double playbackRate, juce::StringArray* missing = nullptr);

    static bool isAudioFile (const juce::File& f);
    juce::AudioFormatManager& getFormatManager() { return formats; }

private:
    mutable std::recursive_mutex mutex;
    juce::AudioFormatManager formats;
    std::array<SlotModel, kNumKitSlots> slots;   // drums, then pads
    void givePadANote (int slot);
    juce::String kitName { "Untitled Kit" };
    std::map<juce::String, std::pair<juce::Time, SamplePtr>> cache;   // path -> (mtime, sample)
    std::map<juce::String, SamplePtr> playCache;                       // path|rate -> resampled sample
};

} // namespace thruma
