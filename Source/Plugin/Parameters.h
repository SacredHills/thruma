#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace thruma::ids
{
// Replace
inline constexpr const char* repOn         = "rep_on";
inline constexpr const char* repMode       = "rep_mode";
inline constexpr const char* repSource     = "rep_source";
inline constexpr const char* instrument    = "instrument";
inline constexpr const char* repSample     = "rep_sample";
inline constexpr const char* repSens       = "rep_sens";
inline constexpr const char* repVelResp    = "rep_velresp";
inline constexpr const char* repBlend      = "rep_blend";
inline constexpr const char* repOutput     = "rep_output";
// Replace - advanced
inline constexpr const char* repGhost      = "rep_ghost";
inline constexpr const char* repRetrig     = "rep_retrig";
inline constexpr const char* repDouble     = "rep_double";
inline constexpr const char* repFocus      = "rep_focus";
inline constexpr const char* repDynRange   = "rep_dynrange";
inline constexpr const char* repAmount     = "rep_amount";   // how easily a possible hit is noticed
inline constexpr const char* repMatch      = "rep_match";    // how much it must sound like the drum
inline constexpr const char* repGain       = "rep_gain";
inline constexpr const char* repPolarity   = "rep_polarity";
inline constexpr const char* repOffset     = "rep_offset";
inline constexpr const char* repAlign      = "rep_align";
inline constexpr const char* repAutoAlign  = "rep_autoalign";
inline constexpr const char* repLatency    = "rep_latency";
inline constexpr const char* midiOut       = "midi_out";
// Debleed
inline constexpr const char* dbOn          = "db_on";
inline constexpr const char* dbAmount      = "db_amount";
inline constexpr const char* dbSens        = "db_sens";
inline constexpr const char* dbAttack      = "db_attack";
inline constexpr const char* dbRelease     = "db_release";
inline constexpr const char* dbReference   = "db_reference";
// Debleed - advanced
inline constexpr const char* dbMaxRed      = "db_maxred";
inline constexpr const char* dbSmooth      = "db_smooth";
inline constexpr const char* dbLowProtect  = "db_lowprotect";
inline constexpr const char* dbDecay       = "db_decay";
inline constexpr const char* dbAdaptive    = "db_adaptive";
} // namespace thruma::ids

namespace thruma
{
enum class ReferenceSource : int { None = 0, SidechainL, SidechainR, SidechainSum, Linked };

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
juce::StringArray instrumentChoices();
} // namespace thruma
