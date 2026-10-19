#include "Samples.h"
#include "AudioFile.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace drb
{

float estimatePitch (const juce::AudioBuffer<float>& audio, double sampleRate, Instrument instrument)
{
    // The first 150 ms, mono, zero-padded for fine frequency steps.
    constexpr int order = 14, size = 1 << order;
    const int n = std::min (audio.getNumSamples(), std::min (size, (int) (0.15 * sampleRate)));
    if (n < 64)
        return 0.0f;

    std::vector<float> data ((size_t) size * 2, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        float v = 0.0f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            v += audio.getSample (ch, i);
        const float window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (n - 1));
        data[(size_t) i] = v * window;
    }
    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data(), true);
    const double binHz = sampleRate / size;

    const bool cymbal = instrument == Instrument::HiHatClosed || instrument == Instrument::HiHatOpen
                     || instrument == Instrument::Crash || instrument == Instrument::Ride
                     || instrument == Instrument::Clap;
    if (cymbal)
    {
        // Brightness: spectral centroid from 1 to 16 kHz.
        double sum = 0.0, weighted = 0.0;
        for (int k = (int) (1000.0 / binHz); k < std::min (size / 2, (int) (16000.0 / binHz)); ++k)
        {
            sum += data[(size_t) k];
            weighted += data[(size_t) k] * k * binHz;
        }
        return sum > 0.0 ? (float) (weighted / sum) : 0.0f;
    }

    // The strongest partial in the drum's range, refined between bins.
    const double lo = instrument == Instrument::Kick ? 30.0 : instrument == Instrument::Snare ? 120.0 : 60.0;
    const double hi = instrument == Instrument::Kick ? 200.0 : instrument == Instrument::Snare ? 500.0 : 400.0;
    int best = -1;
    for (int k = (int) (lo / binHz); k <= (int) (hi / binHz) && k + 1 < size / 2; ++k)
        if (best < 0 || data[(size_t) k] > data[(size_t) best])
            best = k;
    if (best < 1)
        return 0.0f;

    const double a = data[(size_t) best - 1], b = data[(size_t) best], c = data[(size_t) best + 1];
    const double denom = a - 2.0 * b + c;
    const double offset = std::abs (denom) > 1.0e-12 ? 0.5 * (a - c) / denom : 0.0;
    return (float) ((best + std::clamp (offset, -0.5, 0.5)) * binHz);
}

SampleLibrary::SampleLibrary (const juce::File& r) : root (r) {}

juce::File SampleLibrary::defaultRoot()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Drum Rebuilder").getChildFile ("Samples");
}

void SampleLibrary::setRoot (const juce::File& r)
{
    root = r;
    rescan();
}

juce::File SampleLibrary::folderFor (Instrument i) const
{
    return root.getChildFile (instrumentName (i));
}

void SampleLibrary::rescan()
{
    for (auto inst : allInstruments)
    {
        auto& list = lists[(size_t) inst];
        list.clear();
        const auto folder = folderFor (inst);
        if (! folder.isDirectory())
            continue;

        auto files = folder.findChildFiles (juce::File::findFiles, false, supportedWildcard());
        files.sort();

        // The user's order first, then the rest by name.
        const auto order = juce::StringArray::fromLines (folder.getChildFile (orderFileName()).loadFileAsString());
        std::stable_sort (files.begin(), files.end(), [&] (const juce::File& a, const juce::File& b)
        {
            auto rank = [&] (const juce::File& f)
            {
                const int i = order.indexOf (f.getFileName());
                return i < 0 ? order.size() : i;
            };
            return rank (a) < rank (b);
        });
        for (const auto& f : files)
        {
            juce::String error;
            auto loaded = loadAudioFile (f, error, 10.0);   // drum hits: 10 s is plenty
            if (! loaded)
                continue;

            auto s = std::make_shared<Sample>();
            s->file = f;
            s->audio = std::move (loaded->audio);
            s->sampleRate = loaded->sampleRate;
            const auto name = f.getFileNameWithoutExtension();
            s->original = name.contains (originalsTag());
            if (s->original)
                s->fromBeat = name.upToFirstOccurrenceOf (originalsTag(), false, false);
            s->peak = s->audio.getMagnitude (0, s->audio.getNumSamples());
            s->pitch = estimatePitch (s->audio, s->sampleRate, inst);
            list.push_back (std::move (s));
        }
    }
}

int SampleLibrary::saveOriginals (const juce::String& beatName, const juce::AudioBuffer<float>& audio, double sampleRate,
                                  const std::vector<Cut>& cuts)
{
    const auto prefix = juce::File::createLegalFileName (beatName) + originalsTag();

    // This beat's earlier originals go first, in every folder.
    for (auto inst : allInstruments)
        for (const auto& f : folderFor (inst).findChildFiles (juce::File::findFiles, false, "*.wav"))
            if (f.getFileName().startsWith (prefix))
                f.deleteFile();

    std::array<int, numInstruments> numbers {};
    int written = 0;
    const int fadeIn = (int) (0.002 * sampleRate), fadeOut = (int) (0.010 * sampleRate);
    for (const auto& c : cuts)
    {
        const int a = std::max (0, (int) std::floor (c.in * sampleRate));
        const int b = std::min (audio.getNumSamples(), (int) std::ceil (c.out * sampleRate));
        if (b - a < fadeIn + fadeOut)
            continue;

        juce::AudioBuffer<float> slice (audio.getNumChannels(), b - a);
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            slice.copyFrom (ch, 0, audio, ch, a, b - a);
        slice.applyGainRamp (0, fadeIn, 0.0f, 1.0f);
        slice.applyGainRamp (b - a - fadeOut, fadeOut, 1.0f, 0.0f);

        const int number = ++numbers[(size_t) c.instrument];
        const auto file = folderFor (c.instrument).getChildFile (prefix + juce::String (number).paddedLeft ('0', 2) + ".wav");
        if (writeWav (file, slice, sampleRate))
            ++written;
    }

    rescan();
    return written;
}

void SampleLibrary::moveSample (Instrument inst, int from, int to)
{
    auto& list = lists[(size_t) inst];
    if (! juce::isPositiveAndBelow (from, (int) list.size()))
        return;
    to = juce::jlimit (0, (int) list.size() - 1, to);
    if (from == to)
        return;
    auto s = list[(size_t) from];
    list.erase (list.begin() + from);
    list.insert (list.begin() + to, std::move (s));
    saveOrder (inst);
}

void SampleLibrary::saveOrder (Instrument inst) const
{
    juce::StringArray names;
    for (const auto& s : lists[(size_t) inst])
        names.add (s->file.getFileName());
    folderFor (inst).getChildFile (orderFileName()).replaceWithText (names.joinIntoString ("\n") + "\n");
}

int SampleLibrary::addFiles (Instrument instrument, const juce::Array<juce::File>& files)
{
    const auto folder = folderFor (instrument);
    if (! folder.createDirectory())
        return 0;

    int copied = 0;
    for (const auto& f : files)
        if (f.existsAsFile() && isSupported (f) && f.copyFileTo (folder.getNonexistentChildFile (f.getFileNameWithoutExtension(),
                                                                                                f.getFileExtension(), false)))
            ++copied;
    rescan();
    return copied;
}

} // namespace drb
