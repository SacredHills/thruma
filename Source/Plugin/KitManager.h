// Editable sample-kit model (message thread). Loads standard WAV/AIFF files with JUCE, never
// modifies or copies the originals, and builds immutable thruma::Kit snapshots for the audio thread.
#pragma once

#include "Kit.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
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
        float volumeDb = 0.0f;      // mixer
        float pan = 0.0f;           // -1 .. +1
        bool mute = false, solo = false;
        int output = 0;             // 0 = main, k = extra output k ("Out k+1")
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
    /** Mixer settings only (volume / pan / mute / solo); no kit rebuild needed. */
    void setMix (int slot, float volumeDb, float pan, bool mute, bool solo);
    void setOutput (int slot, int output);
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
    /** Drums: remove the samples. Pads: remove the pad (name, note, mix); later pads move up one place. */
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

    // --- TDE: one drum (all its velocity layers and round-robin samples) in one file ---
    static bool isTdeFile (const juce::File& f) { return f.hasFileExtension ("tde"); }
    /** Folder where loaded .tde files are unpacked (Documents/Thruma/TDE). */
    static juce::File getTdeLibraryFolder();
    /** Save a drum / pad: its sample files (copied as they are), velocity ranges and round-robin mode. */
    bool saveSlotTde (int slot, const juce::File& dest, juce::String& error);
    /** Load a .tde onto a drum / pad (keeps its MIDI note and mixer settings). */
    bool loadSlotTde (int slot, const juce::File& tde, juce::String& error);

    // --- progress of a long load (any thread) ---
    enum class LoadPhase : int { None = 0, Samples = 1, Kit = 2 };
    void beginProgress (LoadPhase phase, int total) noexcept { progressDone.store (0); progressTotal.store (total); progressPhase.store ((int) phase); }
    void setProgressTotal (int total) noexcept { progressTotal.store (total); }
    void endProgress() noexcept { progressPhase.store (0); }
    LoadPhase getProgressPhase() const noexcept { return (LoadPhase) progressPhase.load(); }
    int getProgressDone() const noexcept { return progressDone.load(); }
    int getProgressTotal() const noexcept { return progressTotal.load(); }
    int countFiles() const;
    juce::AudioFormatManager& getFormatManager() { return formats; }

private:
    mutable std::recursive_mutex mutex;
    juce::AudioFormatManager formats;
    std::array<SlotModel, kNumKitSlots> slots;   // drums, then pads
    void givePadANote (int slot);
    juce::String kitName { "Untitled Kit" };
    std::map<juce::String, std::pair<juce::Time, SamplePtr>> cache;   // path -> (mtime, sample)
    std::map<juce::String, SamplePtr> playCache;                       // path|rate -> resampled sample
    std::atomic<int> progressDone { 0 }, progressTotal { 0 }, progressPhase { 0 };
    void tick() noexcept { if (progressPhase.load (std::memory_order_relaxed) != 0) progressDone.fetch_add (1, std::memory_order_relaxed); }
};

} // namespace thruma
