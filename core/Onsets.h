#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

// Where something was hit, before we know what it was.
namespace drb
{

struct Onset
{
    double time = 0.0;       // seconds, refined to about a millisecond
    float strength = 0.0f;   // onset function peak, for velocity and thresholds
};

// Spectral flux of the recording, one value per hop: broadband (any hit) and
// low band (kick), both with the local average taken away.
struct OnsetFunction
{
    std::vector<float> all, low;
    double hopSeconds = 0.0;
    double offsetSeconds = 0.0;   // time of value 0 (centre of the first FFT frame)

    double timeOf (double index) const { return offsetSeconds + index * hopSeconds; }
};

std::vector<float> mixToMono (const juce::AudioBuffer<float>&);

OnsetFunction computeOnsetFunction (const std::vector<float>& mono, double sampleRate);

std::vector<Onset> pickOnsets (const OnsetFunction&, const std::vector<float>& mono, double sampleRate);

} // namespace drb
