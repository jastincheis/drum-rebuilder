#pragma once

#include "Model.h"
#include "Onsets.h"

// Tempo, beat grid and bar starts of a drum recording (constant tempo).
namespace drb
{

struct BeatGrid
{
    double bpm = 0.0;             // 0 = no steady beat found
    double firstBeat = 0.0;       // seconds; may be before the first hit
    double firstDownbeat = 0.0;   // seconds, the "1" of the first bar
    TimeSignature timeSignature;
    int bars = 0;
    float confidence = 0.0f;      // 0..1

    double beatSeconds() const { return bpm > 0.0 ? 60.0 / bpm : 0.0; }
};

BeatGrid estimateBeatGrid (const OnsetFunction&, const std::vector<Onset>&);

// Second pass once the instruments are known: fixes a tempo found twice too
// fast or too slow (the snare normally comes every 2 beats), and puts bar 1
// where kick and snare fit the usual pattern best (kick on 1, snare on 2
// and 4).
void refineGridWithHits (BeatGrid&, const std::vector<Hit>&, const std::vector<Onset>&);

} // namespace drb
