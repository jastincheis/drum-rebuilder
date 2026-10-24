#pragma once

#include "Model.h"
#include "Samples.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <vector>

// The beat played again with other sounds: same hits, same timing (groove,
// microtiming), same accents, optionally at another tempo.
namespace drb
{

// How an instrument's hits pick their sound.
enum class PickMode
{
    Original,     // each hit its own sound, cut from the recording
    Fixed,        // one sound for every hit
    Random,       // any sound of the instrument's pool, each hit anew
    RoundRobin,   // the pool's sounds in turn
    Velocity      // softer hits the quieter sounds of the pool, harder the louder
};

const char* pickModeName (PickMode);
inline constexpr PickMode allPickModes[] { PickMode::Original, PickMode::Fixed, PickMode::Random,
                                           PickMode::RoundRobin, PickMode::Velocity };

struct InstrumentPlan
{
    PickMode mode = PickMode::Original;
    std::vector<SamplePtr> pool;   // the sounds to choose from (empty = originals)
    int fixed = 0;                 // which one for Fixed
    double maxLength = 0.0;        // where a sound stops (OUT), seconds from its start (0 = all of it), then fades out
    double startAt = 0.0;          // where it starts (IN), seconds from its start: that part is skipped, and
                                   // what is left begins at the hit
    bool muted = false;            // not heard (the others' sounds stay the same)
};

struct RebuildSettings
{
    std::array<InstrumentPlan, numInstruments> instruments;
    double tempoFactor = 1.0;   // 1.25 = 25 % faster; sounds keep their length
    bool keepPitch = false;     // tune new sounds to the original instrument
    juce::uint32 seed = 1;      // for Random
};

// The recording and its hits, each with its IN / OUT (seconds).
struct RebuildInput
{
    const juce::AudioBuffer<float>* audio = nullptr;
    double sampleRate = 44100.0;
    std::vector<Hit> hits;                          // sorted by time
    std::vector<std::pair<double, double>> slices;  // one per hit, around its sourceTime()
};

// The whole rebuilt beat at `outputRate`, stereo, with `tailSeconds` after
// the end for the last sounds to ring out.
juce::AudioBuffer<float> renderRebuild (const RebuildInput&, const RebuildSettings&, double outputRate,
                                        double tailSeconds = 1.0);

} // namespace drb
