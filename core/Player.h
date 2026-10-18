#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <memory>

namespace drb
{

// Plays the original recording: play, pause, stop, loop a region. The
// control calls come from the message thread, render() from the audio
// thread; they share only atomics and a try-lock, so the audio thread never
// waits and never allocates.
class Player
{
public:
    // Audio thread setup (prepareToPlay).
    void prepare (double deviceSampleRate);
    double getDeviceSampleRate() const { return deviceRate.load(); }

    // Message thread. The buffer must already be at the device sample rate.
    void setSource (std::unique_ptr<juce::AudioBuffer<float>> audioAtDeviceRate);
    void clearSource();

    void play();
    void pause();
    void stop();   // pause and go back to the loop start (or the beginning)

    bool isPlaying() const { return wantPlaying.load(); }

    void setPositionSeconds (double);
    double getPositionSeconds() const;

    // Looping on; `region` in seconds, empty = the whole recording.
    void setLoop (bool on, juce::Range<double> region);
    bool isLooping() const { return looping.load(); }

    // Audio thread: adds the recording into out[start, start + num). With
    // `positions`, also writes there, for every output sample, the recording
    // sample played at it, or -1 (paused, fading out, nothing loaded).
    void render (juce::AudioBuffer<float>& out, int start, int num, juce::int64* positions = nullptr);

private:
    juce::SpinLock sourceLock;
    std::unique_ptr<juce::AudioBuffer<float>> source;
    std::atomic<juce::int64> sourceLength { 0 };

    std::atomic<double> deviceRate { 44100.0 };
    std::atomic<juce::int64> position { 0 };
    std::atomic<bool> wantPlaying { false }, looping { false }, rewindWhenSilent { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };   // samples, end 0 = whole file
    std::atomic<juce::int64> seekRequest { -1 };               // picked up by the next render()

    float gain = 0.0f;   // audio thread only: 5 ms fades on play / pause

    juce::int64 toSamples (double seconds) const;
    void moveTo (juce::int64 samplePosition);
};

} // namespace drb
