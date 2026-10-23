#pragma once

#include "Model.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <optional>

// Which library folder a sound file belongs in, for "Import a folder": by
// its name first (kick, snare, hat ...), else by how it sounds. Loops go to
// their own folder, sounds we can't tell to "Unsorted".
namespace drb
{

enum class SortedAs
{
    Instrument,   // see `instrument`
    Unsorted,
    Loop
};

struct SortResult
{
    SortedAs as = SortedAs::Unsorted;
    Instrument instrument = Instrument::Kick;
    juce::String why;   // "name", "sound", "loop" ... for the summary / tests
};

// The instrument a file's name says, looking at the file name and then at
// the folders above it up to (and including) `top`. Nothing when the name
// says nothing, or names two different drums ("Kick_Snare").
std::optional<Instrument> instrumentFromName (const juce::File&, const juce::File& top);
bool nameSaysLoop (const juce::File&, const juce::File& top);
bool nameSaysOneShot (const juce::File&);   // "one shot", "OneShot" ...

// A recording with several hits in it, longer than a one-shot.
bool soundsLikeLoop (const juce::AudioBuffer<float>&, double sampleRate);

// The instrument a one-shot sounds like, or nothing when it is unclear.
std::optional<Instrument> instrumentFromSound (const juce::AudioBuffer<float>&, double sampleRate);

// The measurements instrumentFromSound() decides on, as text (for tests).
juce::String describeSound (const juce::AudioBuffer<float>&, double sampleRate);

// Everything together: a loop by its sound or name wins, then the name, then
// the sound. `audio` = the start of the file (up to ~12 s).
SortResult sortSound (const juce::File&, const juce::File& top, const juce::AudioBuffer<float>& audio, double sampleRate);

} // namespace drb
