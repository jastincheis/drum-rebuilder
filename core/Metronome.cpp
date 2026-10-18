#include "Metronome.h"

#include <cmath>

namespace drb
{

void Metronome::prepare (double deviceSampleRate)
{
    rate = deviceSampleRate;
    const int length = (int) (0.04 * rate);
    clicks.setSize (2, length);
    for (int c = 0; c < 2; ++c)
    {
        const double freq = c == 0 ? 1760.0 : 1100.0;
        const float level = c == 0 ? 0.5f : 0.32f;
        for (int i = 0; i < length; ++i)
        {
            const double t = i / rate;
            clicks.setSample (c, i, level * (float) (std::exp (-t / 0.008) * std::sin (juce::MathConstants<double>::twoPi * freq * t)));
        }
    }
    beatSamples = beatSeconds.load() * rate;
    downbeatSample = downbeatSeconds.load() * rate;
    playing = -1;
}

void Metronome::setGrid (double bpm, double firstDownbeatSeconds, int beatsPerBar)
{
    beatSeconds = bpm > 0.0 ? 60.0 / bpm : 0.0;
    downbeatSeconds = firstDownbeatSeconds;
    perBar = std::max (1, beatsPerBar);
    beatSamples = beatSeconds.load() * rate;
    downbeatSample = firstDownbeatSeconds * rate;
}

void Metronome::render (juce::AudioBuffer<float>& out, int start, int num, const juce::int64* positions)
{
    const double beat = beatSamples.load(), first = downbeatSample.load();
    const bool on = enabled.load() && beat > 1.0;
    const int bar = perBar.load(), length = clicks.getNumSamples();

    for (int i = 0; i < num; ++i)
    {
        // A beat falls between the previous recording sample and this one.
        if (const auto p = positions[i]; on && p > 0)
        {
            const double k = std::floor (((double) p - first) / beat);
            if (k > std::floor (((double) p - 1.0 - first) / beat))
            {
                const auto kb = (juce::int64) k;
                playing = ((kb % bar) + bar) % bar == 0 ? 0 : 1;
                playPos = 0;
            }
        }

        if (playing < 0)
            continue;
        const float v = clicks.getSample (playing, playPos);
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            out.addSample (ch, start + i, v);
        if (++playPos >= length)
            playing = -1;
    }
}

} // namespace drb
