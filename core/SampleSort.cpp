#include "SampleSort.h"
#include "Onsets.h"
#include "Samples.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace drb
{
namespace
{
// "FL_TR_Kit03_KickOneShot" -> fl, tr, kit, 03, kick, one, shot.
juce::StringArray words (const juce::String& name)
{
    juce::String spaced;
    juce::juce_wchar previous = 0;
    for (auto c : name)
    {
        const bool boundary = (juce::CharacterFunctions::isUpperCase (c) && juce::CharacterFunctions::isLowerCase (previous))
                           || (juce::CharacterFunctions::isDigit (c) != juce::CharacterFunctions::isDigit (previous)
                               && juce::CharacterFunctions::isLetterOrDigit (c) && juce::CharacterFunctions::isLetterOrDigit (previous));
        if (boundary || ! juce::CharacterFunctions::isLetterOrDigit (c))
            spaced << ' ';
        if (juce::CharacterFunctions::isLetterOrDigit (c))
            spaced << juce::CharacterFunctions::toLowerCase (c);
        previous = c;
    }
    juce::StringArray w;
    w.addTokens (spaced, " ", "");
    w.removeEmptyStrings();
    return w;
}

bool any (const juce::StringArray& w, std::initializer_list<const char*> list)
{
    for (auto* s : list)
        if (w.contains (s))
            return true;
    return false;
}

// The drums one name talks about (several when it names more than one).
std::set<Instrument> drumsIn (const juce::String& name)
{
    const auto w = words (name);
    const auto joined = w.joinIntoString ("");
    std::set<Instrument> found;

    const bool hat = any (w, { "hat", "hats", "hh", "hihat", "hihats", "hhat", "chh", "ohh", "oh", "ch" })
                  || joined.contains ("hihat");
    const bool open = any (w, { "open", "opened", "ohh", "oh", "openhat" }) || joined.contains ("openhat");
    if (hat)
        found.insert (open ? Instrument::HiHatOpen : Instrument::HiHatClosed);

    if (any (w, { "kick", "kicks", "kck", "kik", "bd", "bassdrum" }) || joined.contains ("bassdrum") || joined.contains ("kick"))
        found.insert (Instrument::Kick);
    if (any (w, { "snare", "snares", "snr", "sd" }) || joined.contains ("snare"))
        found.insert (Instrument::Snare);
    if (any (w, { "clap", "claps", "clp", "handclap" }) || joined.contains ("clap"))
        found.insert (Instrument::Clap);
    if (any (w, { "rim", "rims", "rimshot", "rimshots", "sidestick", "stick" }) || joined.contains ("rimshot"))
        found.insert (Instrument::Rimshot);
    if (any (w, { "crash", "crashes", "splash", "china", "cymbal", "cymbals", "cym" }))
        found.insert (Instrument::Crash);
    if (any (w, { "ride", "rides" }))
        found.insert (Instrument::Ride);
    if (any (w, { "tom", "toms", "floortom" }) || joined.contains ("floortom"))
        found.insert (any (w, { "floor", "low", "lo" }) || joined.contains ("floortom") ? Instrument::TomLow
                      : any (w, { "high", "hi" })                                       ? Instrument::TomHigh
                                                                                        : Instrument::TomMid);
    return found;
}

// Mono copy from the start of the sound (a little before the first sample
// above 5 % of the peak).
std::vector<float> trimmedMono (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    auto mono = mixToMono (audio);
    float peak = 0.0f;
    for (auto v : mono)
        peak = std::max (peak, std::abs (v));
    size_t start = 0;
    while (start < mono.size() && std::abs (mono[start]) < 0.05f * peak)
        ++start;
    start -= std::min (start, (size_t) (0.002 * sampleRate));
    return { mono.begin() + (long) start, mono.end() };
}

struct Features
{
    double low = 0, lowMid = 0, mid = 0, high = 0;   // energy shares over the first 100 ms, sum 1
    double ring = 0;                                 // seconds until 30 dB below the peak
    double noisiness = 0;                            // spectral flatness 200 Hz..10 kHz, 0 tonal .. 1 noise
    double centroid = 0;                             // Hz
};

Features measure (const std::vector<float>& mono, double sampleRate)
{
    Features f;
    constexpr int order = 13, size = 1 << order;
    const int n = std::min ((int) mono.size(), std::min (size, (int) (0.1 * sampleRate)));
    std::vector<float> data ((size_t) size * 2, 0.0f);
    for (int i = 0; i < n; ++i)
        data[(size_t) i] = mono[(size_t) i] * (i > n - (int) (0.01 * sampleRate) ? (float) (n - i) / (float) (0.01 * sampleRate) : 1.0f);
    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data(), true);

    const double binHz = sampleRate / size;
    double total = 0, weighted = 0, logSum = 0, linSum = 0;
    int flatBins = 0;
    for (int k = 1; k < size / 2; ++k)
    {
        const double hz = k * binHz, p = (double) data[(size_t) k] * data[(size_t) k];
        if (hz < 25.0 || hz > 18000.0)
            continue;
        (hz < 150.0 ? f.low : hz < 1000.0 ? f.lowMid : hz < 5000.0 ? f.mid : f.high) += p;
        total += p;
        weighted += p * hz;
        if (hz > 200.0 && hz < 10000.0)
        {
            logSum += std::log (p + 1.0e-20);
            linSum += p;
            ++flatBins;
        }
    }
    if (total > 0.0)
    {
        f.low /= total;
        f.lowMid /= total;
        f.mid /= total;
        f.high /= total;
        f.centroid = weighted / total;
    }
    if (flatBins > 0 && linSum > 0.0)
        f.noisiness = std::exp (logSum / flatBins) / (linSum / flatBins);

    // How long it rings: 10 ms RMS frames.
    const int frame = std::max (1, (int) (0.01 * sampleRate));
    double peakRms = 0;
    int peakFrame = 0;
    std::vector<double> rms;
    for (int a = 0; a + frame <= (int) mono.size(); a += frame)
    {
        double s = 0;
        for (int i = a; i < a + frame; ++i)
            s += (double) mono[(size_t) i] * mono[(size_t) i];
        rms.push_back (std::sqrt (s / frame));
        if (rms.back() > peakRms)
        {
            peakRms = rms.back();
            peakFrame = (int) rms.size() - 1;
        }
    }
    int end = peakFrame;
    while (end + 1 < (int) rms.size() && rms[(size_t) end + 1] > peakRms * 0.0316)   // -30 dB
        ++end;
    f.ring = (end + 1) * 0.01;
    return f;
}
} // namespace

std::optional<Instrument> instrumentFromName (const juce::File& file, const juce::File& top)
{
    // The file's own name first, then each folder going up.
    for (auto f = file; ; f = f.getParentDirectory())
    {
        const auto found = drumsIn (f == file ? f.getFileNameWithoutExtension() : f.getFileName());
        if (found.size() > 1)
            return std::nullopt;   // "Kick_Snare": the sound decides
        if (found.size() == 1)
            return *found.begin();
        if (f == top || ! f.isAChildOf (top) || f.getParentDirectory() == f)
            break;
    }
    return std::nullopt;
}

bool nameSaysOneShot (const juce::File& file)
{
    const auto w = words (file.getFileNameWithoutExtension());
    return any (w, { "oneshot", "oneshots" }) || w.joinIntoString ("").contains ("oneshot");
}

bool nameSaysLoop (const juce::File& file, const juce::File& top)
{
    for (auto f = file; ; f = f.getParentDirectory())
    {
        const auto w = words (f == file ? f.getFileNameWithoutExtension() : f.getFileName());
        if (any (w, { "loop", "loops", "break", "breaks", "breakbeat", "breakbeats", "groove", "grooves" }))
            return true;
        if (f == top || ! f.isAChildOf (top) || f.getParentDirectory() == f)
            break;
    }
    return false;
}

// New hits after the first: the level jumping up again (x2.5 within 50 ms)
// while still loud (within 26 dB of the peak). A cymbal that rings out only
// goes down, with small wobbles.
int reattacks (const std::vector<float>& mono, double sampleRate, double& span)
{
    const int frame = std::max (1, (int) (0.005 * sampleRate));
    std::vector<double> rms;
    double peak = 0.0;
    for (int a = 0; a + frame <= (int) mono.size(); a += frame)
    {
        double sum = 0.0;
        for (int i = a; i < a + frame; ++i)
            sum += (double) mono[(size_t) i] * mono[(size_t) i];
        rms.push_back (std::sqrt (sum / frame));
        peak = std::max (peak, rms.back());
    }
    int count = 0;
    double first = -1.0, last = -1.0;
    for (size_t i = 10; i < rms.size(); ++i)
    {
        const double before = *std::min_element (rms.begin() + (long) i - 10, rms.begin() + (long) i);
        if (rms[i] > 2.5 * before && rms[i] > 0.05 * peak)
        {
            ++count;
            const double t = (double) i * 0.005;
            first = first < 0.0 ? t : first;
            last = t;
            i += 10;   // one hit, not each frame of its attack
        }
    }
    span = last - first;
    return count;
}

bool soundsLikeLoop (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const auto mono = trimmedMono (audio, sampleRate);
    if ((double) mono.size() / sampleRate < 1.0)
        return false;
    double span = 0.0;
    return reattacks (mono, sampleRate, span) >= 3 && span > 0.75;
}

std::optional<Instrument> instrumentFromSound (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const auto mono = trimmedMono (audio, sampleRate);
    if (mono.size() < (size_t) (0.01 * sampleRate))
        return std::nullopt;
    const auto f = measure (mono, sampleRate);

    // Measured on a few hundred real one-shots (kick / snare / hats /
    // crash / tom folders); claps and rims sound like snares here.
    if (f.low > 0.6 && f.high < 0.05)
        return Instrument::Kick;
    if (f.ring > 1.0 && f.low < 0.05 && f.mid + f.high > 0.55)
        return Instrument::Crash;
    if (f.high > 0.55 && f.low < 0.1)
        return f.ring > 0.25 ? Instrument::HiHatOpen : Instrument::HiHatClosed;
    if (f.centroid < 450.0 && f.low + f.lowMid > 0.85)
    {
        const auto pitch = estimatePitch (audio, sampleRate, Instrument::TomMid);
        return pitch < 110.0f ? Instrument::TomLow : pitch < 200.0f ? Instrument::TomMid : Instrument::TomHigh;
    }
    if (f.low < 0.35 && f.centroid >= 500.0 && (f.noisiness > 0.1 || f.mid + f.high > 0.3))
        return Instrument::Snare;
    return std::nullopt;
}

juce::String describeSound (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const auto mono = trimmedMono (audio, sampleRate);
    const auto f = measure (mono, sampleRate);
    double span = 0.0;
    const int hits = reattacks (mono, sampleRate, span);
    return juce::String::formatted ("low %.2f lowmid %.2f mid %.2f high %.2f ring %.2f noise %.3f centroid %.0f len %.2f hits %d/%.2f",
                                    f.low, f.lowMid, f.mid, f.high, f.ring, f.noisiness, f.centroid,
                                    (double) mono.size() / sampleRate, hits, span);
}

SortResult sortSound (const juce::File& file, const juce::File& top, const juce::AudioBuffer<float>& audio, double sampleRate)
{
    if (nameSaysLoop (file, top))
        return { SortedAs::Loop, Instrument::Kick, "loop (name)" };
    if (! nameSaysOneShot (file) && soundsLikeLoop (audio, sampleRate))
        return { SortedAs::Loop, Instrument::Kick, "loop (sound)" };
    if (const auto i = instrumentFromName (file, top))
        return { SortedAs::Instrument, *i, "name" };
    if (const auto i = instrumentFromSound (audio, sampleRate))
        return { SortedAs::Instrument, *i, "sound" };
    return { SortedAs::Unsorted, Instrument::Kick, "unclear" };
}

} // namespace drb
