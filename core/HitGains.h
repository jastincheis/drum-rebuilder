#pragma once

#include "Model.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

// Making single hits of a recording louder or softer. Hits played together
// share the same stretch of audio, so the recording is split into three
// bands (lows, mids, highs) and each hit changes mostly its own: the kick
// the lows, the snare the mids, the hi-hats and cymbals the highs.
namespace drb
{

struct Bands
{
    juce::AudioBuffer<float> low, mid, high;   // they add up to the recording (phase aside)
    double sampleRate = 44100.0;
};

Bands splitBands (const juce::AudioBuffer<float>&, double sampleRate);

// True when some hit's loudness was changed by hand, or a hit was moved or
// added by hand.
bool hasHitGains (const std::vector<Hit>&);

// Per instrument: 1 = as it is, 0 = silent (solo: 1 for the solo ones).
using InstrumentGains = std::array<float, numInstruments>;

// The recording with every hit's loudness change (Hit::soundGain), times
// its instrument's gain, from the bands. A moved hit is taken out where it
// was; moved and added hits themselves are not in it (they are mixed in
// from their sounds, see Rebuild). `sounds` (one per hit, optional): where
// each hit's sound is in the recording (IN to OUT). A change begins at its
// IN, a little before its time, so no bit of the attack slips through; on
// solo nothing plays after the OUT until the next hit.
juce::AudioBuffer<float> applyHitGains (const Bands&, const std::vector<Hit>&, const InstrumentGains* = nullptr,
                                        const std::vector<juce::Range<double>>* sounds = nullptr);

// How loud one hit sounds with its change: the peak of its own band(s) from
// it to the next hit, in dBFS (-100 = silent).
float hitLevelDb (const Bands&, const std::vector<Hit>&, int hitIndex);

} // namespace drb
