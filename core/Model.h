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
};

struct TimeSignature
{
    int beatsPerBar = 4;
    int beatUnit = 4;
};

} // namespace drb
