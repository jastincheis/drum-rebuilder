#pragma once

#include "Model.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace drb
{

struct AnalysisResult
{
    double bpm = 0.0;            // 0 = not detected
    TimeSignature timeSignature;
    int bars = 0;
    double firstDownbeat = 0.0;  // seconds
    std::vector<Hit> hits;       // sorted by time
};

// Runs offline on a background thread, never on the audio thread.
AnalysisResult analyse (const juce::AudioBuffer<float>& audio, double sampleRate);

} // namespace drb
