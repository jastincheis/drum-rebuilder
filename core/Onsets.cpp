#include "Onsets.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>

namespace drb
{

std::vector<float> mixToMono (const juce::AudioBuffer<float>& audio)
{
    const int n = audio.getNumSamples(), channels = audio.getNumChannels();
    std::vector<float> mono ((size_t) n, 0.0f);
    if (channels == 0)
        return mono;

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* in = audio.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += in[i];
    }

    const float g = 1.0f / (float) channels;
    for (auto& s : mono)
        s *= g;
    return mono;
}

OnsetFunction computeOnsetFunction (const std::vector<float>& mono, double sampleRate)
{
    // ~23 ms frames every ~2.9 ms, whatever the sample rate.
    const int order = sampleRate > 60000.0 ? 11 : 10, size = 1 << order;
    const int hop = std::max (32, juce::roundToInt (sampleRate * 128.0 / 44100.0));
    const int half = size / 2;

    OnsetFunction of;
    of.hopSeconds = hop / sampleRate;
    of.offsetSeconds = 0.0;   // frames are padded so frame f is centred on sample f * hop

    const int n = (int) mono.size();
    if (n == 0)
        return of;

    const int frames = n / hop + 1;
    std::vector<float> window ((size_t) size);
    for (int i = 0; i < size; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));

    // Kick lives below ~150 Hz.
    const int lowBins = std::max (2, (int) std::ceil (150.0 * size / sampleRate));

    juce::dsp::FFT fft (order);
    std::vector<float> work ((size_t) (2 * size)), previous ((size_t) half, 0.0f);
    of.all.assign ((size_t) frames, 0.0f);
    of.low.assign ((size_t) frames, 0.0f);

    for (int f = 0; f < frames; ++f)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        const int start = f * hop - half;
        for (int i = 0; i < size; ++i)
        {
            const int s = start + i;
            if (s >= 0 && s < n)
                work[(size_t) i] = mono[(size_t) s] * window[(size_t) i];
        }

        fft.performFrequencyOnlyForwardTransform (work.data(), true);

        float all = 0.0f, low = 0.0f;
        for (int k = 1; k < half; ++k)
        {
            const float m = std::log (1.0f + 100.0f * work[(size_t) k]);
            const float rise = std::max (0.0f, m - previous[(size_t) k]);
            all += rise;
            if (k <= lowBins)
                low += rise;
            previous[(size_t) k] = m;
        }

        of.all[(size_t) f] = f > 0 ? all : 0.0f;
        of.low[(size_t) f] = f > 0 ? low : 0.0f;
    }

    return of;
}

namespace
{
// Moving average of v over +-w values, through a prefix sum.
std::vector<float> localMean (const std::vector<float>& v, int w)
{
    const int n = (int) v.size();
    std::vector<double> prefix ((size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + v[(size_t) i];

    std::vector<float> mean ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const int a = std::max (0, i - w), b = std::min (n, i + w + 1);
        mean[(size_t) i] = (float) ((prefix[(size_t) b] - prefix[(size_t) a]) / (b - a));
    }
    return mean;
}

// The attack inside [from, to): where the short-time energy rises fastest.
double refineAttack (const std::vector<float>& mono, int from, int to, double sampleRate)
{
    const int block = std::max (8, juce::roundToInt (sampleRate * 16.0 / 44100.0));   // ~0.36 ms
    from = std::max (0, from);
    to = std::min ((int) mono.size(), to);
    const int blocks = (to - from) / block;
    if (blocks < 6)
        return (from + to) * 0.5 / sampleRate;

    std::vector<float> logE ((size_t) blocks);
    for (int b = 0; b < blocks; ++b)
    {
        double e = 1.0e-7 * block;   // floor at about -70 dBFS, so noise doesn't count as a rise
        for (int i = 0; i < block; ++i)
        {
            // First difference = high-frequency emphasis: a ringing kick's
            // energy wobbles at 2x its pitch in such short blocks, the
            // click of a new hit doesn't.
            const int s = from + b * block + i;
            const float x = s > 0 ? mono[(size_t) s] - mono[(size_t) s - 1] : 0.0f;
            e += x * x;
        }
        logE[(size_t) b] = (float) std::log (e);
    }

    // Rise over ~1 ms; the attack starts where the steepest rise begins.
    constexpr int span = 3;
    int best = span;
    float bestRise = -1.0e9f;
    for (int b = span; b < blocks; ++b)
    {
        const float rise = logE[(size_t) b] - logE[(size_t) (b - span)];
        if (rise > bestRise)
        {
            bestRise = rise;
            best = b;
        }
    }

    return (from + (best - span + 1) * block) / sampleRate;
}
} // namespace

std::vector<Onset> pickOnsets (const OnsetFunction& of, const std::vector<float>& mono, double sampleRate)
{
    std::vector<Onset> onsets;
    const auto& f = of.all;
    const int n = (int) f.size();
    if (n < 3)
        return onsets;

    const float peak = *std::max_element (f.begin(), f.end());
    if (peak <= 0.0f)
        return onsets;

    const int peakRadius = std::max (1, (int) std::lround (0.025 / of.hopSeconds));   // one hit per 25 ms
    const auto mean = localMean (f, (int) std::lround (0.10 / of.hopSeconds));
    const float floorLevel = 0.04f * peak;

    for (int i = 1; i < n - 1; ++i)
    {
        const float v = f[(size_t) i];
        if (v < mean[(size_t) i] + floorLevel)
            continue;

        bool isMax = true;
        for (int j = std::max (0, i - peakRadius); j <= std::min (n - 1, i + peakRadius) && isMax; ++j)
            if (f[(size_t) j] > v || (j < i && f[(size_t) j] >= v))
                isMax = false;
        if (! isMax)
            continue;

        // On a log scale the flux jumps as soon as the attack enters the
        // frame's right edge, so the attack is mostly after the centre.
        const double centre = of.timeOf (i) * sampleRate;
        const double frame = std::exp2 (sampleRate > 60000.0 ? 11.0 : 10.0);
        int from = (int) (centre - 0.5 * frame), to = (int) (centre + 0.75 * frame);
        if (! onsets.empty())
            from = std::max (from, (int) ((onsets.back().time + 0.010) * sampleRate));

        onsets.push_back ({ refineAttack (mono, from, to, sampleRate), v });
    }

    return onsets;
}

double findAttack (const juce::AudioBuffer<float>& audio, double sampleRate, double around, double window, float minRise)
{
    const int n = audio.getNumSamples();
    const int frame = std::max (1, (int) (sampleRate / 1000.0));   // 1 ms
    const float peak = audio.getMagnitude (0, n);
    if (peak <= 0.0f)
        return -1.0;

    // Peak level per millisecond, from a little before the window on.
    const int first = std::max (0, (int) ((around - window) * sampleRate) / frame - 3);
    const int last = std::min ((n - 1) / frame, (int) ((around + window) * sampleRate) / frame + 1);
    if (last <= first)
        return -1.0;
    std::vector<float> level ((size_t) (last - first + 1));
    for (int k = first; k <= last; ++k)
        level[(size_t) (k - first)] = audio.getMagnitude (k * frame, std::min (frame, n - k * frame));

    auto at = [&] (int k) { return k < first ? 0.0f : level[(size_t) (k - first)]; };
    int best = -1;
    float bestRise = minRise * peak;
    for (int k = std::max (first, (int) ((around - window) * sampleRate) / frame); k < last; ++k)
    {
        // Before the file starts there is silence, so a sound at 0 is found too.
        const float before = k == 0 ? 0.0f : std::max ({ at (k - 1), at (k - 2), at (k - 3) });
        const float rise = std::max (at (k), at (k + 1)) - before;
        if (rise > bestRise)
        {
            bestRise = rise;
            best = k;
        }
    }
    if (best < 0)
        return -1.0;
    return std::max (0.0, best * frame / sampleRate - 0.001);
}

double findSoundStart (const juce::AudioBuffer<float>& audio, double sampleRate, double hit, double earliest)
{
    const int n = audio.getNumSamples(), channels = audio.getNumChannels();
    const int t = std::clamp ((int) std::lround (hit * sampleRate), 0, n);
    const int limit = std::clamp ((int) std::ceil (std::max (earliest, hit - 0.025) * sampleRate), 0, t);
    if (t - limit < 2)
        return hit;

    auto level = [&] (int i)
    {
        float m = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            m = std::max (m, std::abs (audio.getSample (ch, i)));
        return m;
    };

    // How loud the hit gets, and the quietest millisecond before it.
    const float peak = audio.getMagnitude (t, std::min (n - t, (int) (0.015 * sampleRate)));
    const int ms = std::max (1, (int) (0.001 * sampleRate));
    float quiet = peak;
    for (int a = limit; a + ms <= t; a += ms / 2)
        quiet = std::min (quiet, audio.getMagnitude (a, ms));
    if (peak <= 0.0f || quiet > 0.5f * peak)
        return hit;   // no clear start (the sound before rings as loud)
    const float threshold = std::max (0.03f * peak, 2.0f * quiet);

    // Back from the hit until a whole millisecond stays under the threshold:
    // the sound starts right after that gap.
    int start = t, below = 0;
    for (int i = t - 1; i >= limit; --i)
    {
        if (level (i) >= threshold)
        {
            start = i;
            below = 0;
        }
        else if (++below >= ms)
            break;
    }
    return std::min (hit, std::max (earliest, (start - (int) (0.0003 * sampleRate)) / sampleRate));
}

} // namespace drb
