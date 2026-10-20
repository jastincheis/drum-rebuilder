#pragma once

#include <array>
#include <vector>

// The data every module shares: one detected (or hand-placed) drum hit.
namespace drb
{

enum class Instrument
{
    Kick,
    Snare,
    HiHatClosed,
    HiHatOpen,
    Clap,
    Rimshot,
    TomLow,
    TomMid,
    TomHigh,
    Crash,
    Ride
};

inline constexpr int numInstruments = 11;

inline constexpr std::array<Instrument, numInstruments> allInstruments {
    Instrument::Kick, Instrument::Snare, Instrument::HiHatClosed, Instrument::HiHatOpen,
    Instrument::Clap, Instrument::Rimshot, Instrument::TomLow, Instrument::TomMid,
    Instrument::TomHigh, Instrument::Crash, Instrument::Ride
};

const char* instrumentName (Instrument);

// General MIDI percussion note for the instrument (kick 36, snare 38, ...).
int gmNote (Instrument);

struct Hit
{
    double time = 0.0;           // seconds from the start of the recording
    Instrument instrument = Instrument::Kick;
    float velocity = 1.0f;       // 0..1, relative to the loudest hit of this instrument
    float confidence = 1.0f;     // 0..1, how sure the analysis is
    bool manual = false;         // placed or corrected by hand

    // Where this instrument's sound starts and ends (IN / OUT), in seconds
    // from `time`, when set by hand; sliceOut <= sliceIn = automatic.
    float sliceIn = 0.0f, sliceOut = 0.0f;

    // Where this hit's own sound is in the recording, when that is not at
    // `time`: after it was moved in the MIDI editor or quantized (the sound
    // goes along), or added by hand (it borrows its instrument's loudest
    // hit). < 0 = at `time`.
    double source = -1.0;
    bool added = false;   // added by hand, not found by the analysis

    // The velocity its own sound was played at in the recording, once the
    // velocity was changed by hand (< 0 = unchanged): its sound is then
    // played that much louder or softer.
    float soundVelocity = -1.0f;
    float soundGain() const { return soundVelocity > 0.0f ? velocity / soundVelocity : 1.0f; }
    // How hard its sound was really played (its velocity before any change).
    float recordedVelocity() const { return soundVelocity > 0.0f ? soundVelocity : velocity; }

    double sourceTime() const { return source >= 0.0 ? source : time; }

    // How much the sound at this time fits each instrument, 0..1, from the
    // analysis (all 0 when unknown). Used to suggest what else it could be.
    std::array<float, numInstruments> likeness {};
};

struct TimeSignature
{
    int beatsPerBar = 4;
    int beatUnit = 4;
};

} // namespace drb
