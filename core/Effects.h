#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>

namespace drb
{

// The Master: volume, reverb and delay on the whole rebuilt beat. Every
// effect has just two settings, and switching it off keeps them.
struct ReverbSettings
{
    bool on = false;
    float amount = 0.25f;   // 0..1, how much of it is heard
    float length = 0.5f;    // 0..1, a small room .. a long hall
};

// Delay times, in quarter notes, and their names: synced to the tempo.
inline constexpr std::array<double, 6> delayBeats { 2.0, 1.5, 1.0, 0.75, 0.5, 0.25 };
inline constexpr std::array<const char*, 6> delayNames { "1/2", "1/4 dotted", "1/4", "1/8 dotted", "1/8", "1/16" };

struct DelaySettings
{
    bool on = false;
    float amount = 0.25f;   // 0..1, how loud the echoes are
    int length = 4;         // index into delayBeats (1/8)
};

struct MasterSettings
{
    float volumeDb = 0.0f;   // -24..+6
    ReverbSettings reverb;
    DelaySettings delay;

    bool anyEffect() const { return reverb.on || delay.on; }
    // How long the effects still ring after the last hit.
    double tailSeconds (double bpm) const;
    // Saved with the beat as one line of text.
    juce::String toString() const;
    static MasterSettings fromString (const juce::String&);
};

// Applies the Master to a rebuilt beat (stereo) in place. Never clips: if
// it would go over, the whole beat comes down.
void applyMaster (juce::AudioBuffer<float>& audio, double sampleRate, double bpm, const MasterSettings&);

} // namespace drb
