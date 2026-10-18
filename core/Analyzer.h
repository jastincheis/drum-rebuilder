#pragma once

#include "Model.h"
#include "Onsets.h"
#include "Tempo.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace drb
{

struct AnalysisResult
{
    BeatGrid grid;               // tempo, bars, downbeat
    std::vector<Onset> onsets;   // every attack, sorted by time
    std::vector<Hit> hits;       // onsets with instruments (stage 2), sorted by time
};

// Runs offline on a background thread, never on the audio thread.
AnalysisResult analyse (const juce::AudioBuffer<float>& audio, double sampleRate);

} // namespace drb
