#include "AudioFile.h"

#include <cmath>

namespace drb
{

juce::String supportedWildcard()
{
    return "*.wav;*.aif;*.aiff;*.flac;*.mp3";
}

bool isSupported (const juce::File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac;mp3");
}

std::optional<LoadedAudio> loadAudioFile (const juce::File& file, juce::String& error, double maxSeconds)
{
    if (! file.existsAsFile())
    {
        error = "File not found: " + file.getFullPathName();
        return {};
    }
    if (! isSupported (file))
    {
        error = "Not a WAV, AIFF, FLAC or MP3 file: " + file.getFileName();
        return {};
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
    {
        error = "Can't read " + file.getFileName() + " (damaged or unsupported encoding)";
        return {};
    }

    const auto length = (int) std::min<juce::int64> (reader->lengthInSamples,
                                                     (juce::int64) (maxSeconds * reader->sampleRate));
    if (length <= 0)
    {
        error = file.getFileName() + " is empty";
        return {};
    }

    // Surround files come in as their first two channels.
    const int channels = (int) std::min<unsigned int> (2, reader->numChannels);

    LoadedAudio out;
    out.file = file;
    out.sampleRate = reader->sampleRate;
    out.audio.setSize (channels, length);
    if (! reader->read (&out.audio, 0, length, 0, true, channels > 1))
    {
        error = "Reading " + file.getFileName() + " failed";
        return {};
    }
    return out;
}

juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& in, double fromRate, double toRate)
{
    if (std::abs (fromRate - toRate) < 0.5 || in.getNumSamples() == 0)
        return in;

    const double ratio = fromRate / toRate;
    const int outLength = (int) std::ceil (in.getNumSamples() / ratio);
    juce::AudioBuffer<float> out (in.getNumChannels(), outLength);

    for (int ch = 0; ch < in.getNumChannels(); ++ch)
    {
        juce::WindowedSincInterpolator interp;
        out.clear (ch, 0, outLength);
        // The interpolator reads ahead; give it a zero-padded copy.
        std::vector<float> padded ((size_t) in.getNumSamples() + 256, 0.0f);
        std::copy (in.getReadPointer (ch), in.getReadPointer (ch) + in.getNumSamples(), padded.begin());
        interp.process (ratio, padded.data(), out.getWritePointer (ch), outLength);
    }
    return out;
}

bool writeWav (const juce::File& target, const juce::AudioBuffer<float>& audio, double sampleRate, int bitsPerSample)
{
    if (! target.getParentDirectory().createDirectory())
        return false;

    juce::TemporaryFile temp (target);
    {
        auto stream = temp.getFile().createOutputStream();
        if (stream == nullptr)
            return false;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (std::move (stream));
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions()
                                                    .withSampleRate (sampleRate)
                                                    .withNumChannels (audio.getNumChannels())
                                                    .withBitsPerSample (bitsPerSample));
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
            return false;
    }
    return temp.overwriteTargetFileWithTemporary();
}

} // namespace drb
