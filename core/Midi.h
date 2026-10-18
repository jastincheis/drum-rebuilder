#pragma once

#include "Model.h"
#include "Tempo.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <vector>

// Musical time for the hits: the grid they snap to, quantize, and the MIDI
// file they are exported as.
namespace drb
{

struct GridDivision
{
    const char* name;   // "1/16", "1/8T", ...
    double beats;       // length in beats (quarter notes)
};

const std::vector<GridDivision>& gridDivisions();

// The nearest grid line (bar 1 is on the grid).
double snapToGrid (double seconds, const BeatGrid&, double divisionBeats);

// Moves every hit towards its nearest grid line; strength 1 = onto it,
// 0.5 = halfway. Hits keep their order.
std::vector<Hit> quantize (std::vector<Hit>, const BeatGrid&, double divisionBeats, double strength);

// A type 1 MIDI file: tempo and time signature, then one track per
// instrument (named, General MIDI drum notes on channel 10). It starts at
// the bar that holds the first hit, so a pickup before bar 1 stays in.
juce::MidiFile makeMidiFile (const std::vector<Hit>&, const BeatGrid&, int ticksPerQuarter = 960);
bool writeMidiFile (const juce::File&, const std::vector<Hit>&, const BeatGrid&);

} // namespace drb
