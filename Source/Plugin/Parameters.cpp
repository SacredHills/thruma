#include "Parameters.h"

#include "Instruments.h"

namespace thruma
{

juce::StringArray instrumentChoices()
{
    juce::StringArray s;
    for (int i = 0; i < kNumInstruments; ++i)
        s.add (getInstrumentName ((Instrument) i));
    return s;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    using namespace juce;
    using P = AudioParameterFloat;
    using B = AudioParameterBool;
    using C = AudioParameterChoice;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto pct = [] (const char* id, const char* name, float def)
    {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 100.0f, 0.1f), def,
                                                      AudioParameterFloatAttributes().withLabel ("%")
                                                          .withStringFromValueFunction ([] (float v, int) { return String (v, 0) + " %"; }));
    };
    auto db = [] (const char* id, const char* name, float lo, float hi, float def)
    {
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, NormalisableRange<float> (lo, hi, 0.1f), def,
                                                      AudioParameterFloatAttributes().withLabel ("dB")
                                                          .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; }));
    };
    auto ms = [] (const char* id, const char* name, float lo, float hi, float def, float skewCentre = 0.0f)
    {
        NormalisableRange<float> r (lo, hi, 0.1f);
        if (skewCentre > lo) r.setSkewForCentre (skewCentre);
        return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def,
                                                      AudioParameterFloatAttributes().withLabel ("ms")
                                                          .withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " ms"; }));
    };
    const auto nonAuto = AudioParameterChoiceAttributes().withAutomatable (false);

    // --- Replace (main) ---
    layout.add (std::make_unique<B> (ParameterID { ids::repOn, 1 }, "Replace On", true));
    layout.add (std::make_unique<C> (ParameterID { ids::repMode, 1 }, "Mode", StringArray { "Replace", "Layer" }, 0));
    layout.add (std::make_unique<C> (ParameterID { ids::repSource, 1 }, "Trigger Source",
                                     StringArray { "Live detection", "Edited triggers", "MIDI only" }, 0));
    layout.add (std::make_unique<C> (ParameterID { ids::instrument, 1 }, "Instrument", instrumentChoices(), (int) Instrument::Snare));
    auto sampleChoices = StringArray { "Same as instrument" };
    sampleChoices.addArray (instrumentChoices());
    layout.add (std::make_unique<C> (ParameterID { ids::repSample, 1 }, "Sample", sampleChoices, 0));
    layout.add (pct (ids::repSens, "Sensitivity", 50.0f));
    layout.add (pct (ids::repVelResp, "Velocity Response", 70.0f));
    layout.add (pct (ids::repBlend, "Blend", 100.0f));
    layout.add (db (ids::repOutput, "Output", -24.0f, 12.0f, 0.0f));

    // --- Replace (advanced) ---
    layout.add (pct (ids::repGhost, "Ghost Sensitivity", 35.0f));
    layout.add (ms (ids::repRetrig, "Retrigger (0 = auto)", 0.0f, 200.0f, 0.0f, 40.0f));
    layout.add (std::make_unique<B> (ParameterID { ids::repDouble, 1 }, "Double Hits", true));
    layout.add (std::make_unique<B> (ParameterID { ids::repFocus, 1 }, "Frequency Focus", true));
    layout.add (db (ids::repDynRange, "Dynamic Range", 12.0f, 72.0f, 40.0f));
    layout.add (db (ids::repGain, "Sample Gain", -24.0f, 24.0f, 0.0f));
    layout.add (std::make_unique<B> (ParameterID { ids::repPolarity, 1 }, "Invert Polarity", false));
    layout.add (ms (ids::repOffset, "Sample Start", 0.0f, 50.0f, 0.0f, 5.0f));
    layout.add (ms (ids::repAlign, "Align", -5.0f, 20.0f, 0.0f));
    layout.add (std::make_unique<B> (ParameterID { ids::repAutoAlign, 1 }, "Auto Align", true));
    layout.add (std::make_unique<C> (ParameterID { ids::repLatency, 1 }, "Lookahead",
                                     StringArray { "Automatic (per drum, 4.5-8 ms)", "Lowest (3 ms)" }, 0, nonAuto));
    layout.add (std::make_unique<B> (ParameterID { ids::midiOut, 1 }, "MIDI Out", true));

    // --- Debleed (main) ---
    layout.add (std::make_unique<B> (ParameterID { ids::dbOn, 1 }, "Debleed On", false,
                                     AudioParameterBoolAttributes().withAutomatable (false)));
    layout.add (pct (ids::dbAmount, "Debleed Amount", 60.0f));
    layout.add (pct (ids::dbSens, "Debleed Sensitivity", 50.0f));
    layout.add (pct (ids::dbAttack, "Attack Preservation", 60.0f));
    layout.add (ms (ids::dbRelease, "Release", 10.0f, 1000.0f, 120.0f, 150.0f));
    layout.add (std::make_unique<C> (ParameterID { ids::dbReference, 1 }, "Reference",
                                     StringArray { "None (blind)", "Sidechain L", "Sidechain R", "Sidechain L+R", "Linked instance" }, 0, nonAuto));

    // --- Debleed (advanced) ---
    layout.add (db (ids::dbMaxRed, "Max Reduction", 6.0f, 60.0f, 30.0f));
    layout.add (pct (ids::dbSmooth, "Frequency Smoothing", 40.0f));
    layout.add (std::make_unique<P> (ParameterID { ids::dbLowProtect, 1 }, "Low Protect", NormalisableRange<float> (0.0f, 500.0f, 1.0f), 0.0f,
                                     AudioParameterFloatAttributes().withLabel ("Hz")
                                         .withStringFromValueFunction ([] (float v, int) { return v < 1.0f ? String ("Off") : String (v, 0) + " Hz"; })));
    layout.add (std::make_unique<P> (ParameterID { ids::dbDecay, 1 }, "Decay Scale", NormalisableRange<float> (25.0f, 400.0f, 1.0f, 0.5f), 100.0f,
                                     AudioParameterFloatAttributes().withLabel ("%")
                                         .withStringFromValueFunction ([] (float v, int) { return String (v, 0) + " %"; })));
    layout.add (std::make_unique<B> (ParameterID { ids::dbAdaptive, 1 }, "Adaptive Learning", true));
    return layout;
}

} // namespace thruma
