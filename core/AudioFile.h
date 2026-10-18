#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>

namespace drb
{

struct LoadedAudio
{
    juce::File file;
    juce::AudioBuffer<float> audio;   // 1 or 2 channels
    double sampleRate = 0.0;
};

// File types the Import section accepts.
juce::String supportedWildcard();   // "*.wav;*.aif;*.aiff;*.flac;*.mp3"
bool isSupported (const juce::File&);

// Whole file into memory, at most `maxSeconds` (longer files are cut, so a
// stray album doesn't eat all the RAM). Error text in `error` on failure.
std::optional<LoadedAudio> loadAudioFile (const juce::File&, juce::String& error, double maxSeconds = 600.0);

// Writes a WAV file (24-bit by default), through a temporary file so a
// failed write never leaves half a file behind.
bool writeWav (const juce::File&, const juce::AudioBuffer<float>&, double sampleRate, int bitsPerSample = 24);

// High-quality copy at another sample rate, for playback on a device that
// runs at a different rate than the file.
juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>&, double fromRate, double toRate);

} // namespace drb
