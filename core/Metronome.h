#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace drb
{

// Clicks on the beats of the detected grid, the first beat of each bar
// higher. It follows the Player: render() gets, for every output sample, the
// recording position the Player played there (or -1), so it stays in time
// through seeks, loops and pauses.
class Metronome
{
public:
    void prepare (double deviceSampleRate);   // builds the clicks, may allocate

    // Message thread.
    void setGrid (double bpm, double firstDownbeatSeconds, int beatsPerBar);   // bpm 0 = silent
    void setEnabled (bool on) { enabled = on; }
    bool isEnabled() const { return enabled.load(); }

    // Audio thread: adds clicks into out[start, start + num).
    void render (juce::AudioBuffer<float>& out, int start, int num, const juce::int64* positions);

private:
    double rate = 44100.0;
    juce::AudioBuffer<float> clicks { 2, 0 };   // channel 0 = bar start, 1 = other beats

    std::atomic<bool> enabled { false };
    std::atomic<double> beatSamples { 0.0 }, downbeatSample { 0.0 };
    std::atomic<int> perBar { 4 };
    std::atomic<double> beatSeconds { 0.0 }, downbeatSeconds { 0.0 };

    int playing = -1, playPos = 0;   // audio thread: the click sounding now
};

} // namespace drb
