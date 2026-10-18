#pragma once

#include "Model.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <string>

// Drum beats synthesized from code, with the exact list of hits that went
// into them: the ground truth the analyzer is scored against.
namespace drb::test
{

enum class Style
{
    Rock,       // kick 1 & 3, snare 2 & 4, closed hi-hat eighths, open hat at bar end
    Funk,       // sixteenth hats, syncopated kicks, ghost snares
    Sparse      // half-time: few hits, long gaps
};

struct BeatSpec
{
    std::string name;
    Style style = Style::Rock;
    double bpm = 120.0;
    int bars = 4;
    double swing = 0.5;          // 0.5 = straight, 0.66 = triplet swing (off-beat position in the pair)
    double jitterMs = 0.0;       // random timing offset per hit (standard deviation)
    double sampleRate = 44100.0;
    unsigned seed = 1;
};

struct Beat
{
    BeatSpec spec;
    juce::AudioBuffer<float> audio;   // stereo
    std::vector<Hit> truth;           // sorted by time
};

Beat makeBeat (const BeatSpec&);

// The fixed set every test run uses.
std::vector<BeatSpec> standardSet();

} // namespace drb::test
