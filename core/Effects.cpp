#include "Effects.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace drb
{
namespace
{
constexpr float delayFeedback = 0.4f;     // each echo this much softer than the one before
constexpr float delayDampingHz = 4000.0f; // the echoes get a little darker

double delaySeconds (const DelaySettings& d, double bpm)
{
    const int k = std::clamp (d.length, 0, (int) delayBeats.size() - 1);
    return delayBeats[(size_t) k] * 60.0 / std::max (20.0, bpm);
}

// Echoes with feedback, the same time on both channels.
void applyDelay (juce::AudioBuffer<float>& audio, double rate, double bpm, const DelaySettings& d)
{
    const int lag = std::max (1, (int) std::lround (delaySeconds (d, bpm) * rate));
    const float coeff = (float) std::exp (-2.0 * juce::MathConstants<double>::pi * delayDampingHz / rate);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        auto* x = audio.getWritePointer (ch);
        std::vector<float> line ((size_t) lag, 0.0f);   // what comes back out `lag` samples later
        float low = 0.0f;
        for (int i = 0, pos = 0; i < audio.getNumSamples(); ++i, pos = (pos + 1) % lag)
        {
            const float echo = line[(size_t) pos];
            low = echo + coeff * (low - echo);   // one-pole low-pass
            line[(size_t) pos] = x[i] + low * delayFeedback;
            x[i] += low * d.amount;
        }
    }
}

void applyReverb (juce::AudioBuffer<float>& audio, double rate, const ReverbSettings& r)
{
    juce::Reverb reverb;
    juce::Reverb::Parameters p;
    p.roomSize = 0.3f + 0.68f * std::clamp (r.length, 0.0f, 1.0f);
    p.damping = 0.45f;
    p.wetLevel = 0.6f * std::clamp (r.amount, 0.0f, 1.0f);
    p.dryLevel = 1.0f;
    p.width = 1.0f;
    reverb.setParameters (p);
    reverb.setSampleRate (rate);
    if (audio.getNumChannels() >= 2)
        reverb.processStereo (audio.getWritePointer (0), audio.getWritePointer (1), audio.getNumSamples());
    else if (audio.getNumChannels() == 1)
        reverb.processMono (audio.getWritePointer (0), audio.getNumSamples());
}
} // namespace

double MasterSettings::tailSeconds (double bpm) const
{
    double tail = 0.0;
    if (delay.on)   // until the echoes are 60 dB down
        tail += delaySeconds (delay, bpm) * std::ceil (-60.0 / juce::Decibels::gainToDecibels (delayFeedback));
    if (reverb.on)
        tail += 1.0 + 4.0 * reverb.length;
    return tail;
}

juce::String MasterSettings::toString() const
{
    return juce::String (volumeDb, 2) + " " + (reverb.on ? "1" : "0") + " " + juce::String (reverb.amount, 3) + " "
           + juce::String (reverb.length, 3) + " " + (delay.on ? "1" : "0") + " " + juce::String (delay.amount, 3) + " "
           + juce::String (delay.length);
}

MasterSettings MasterSettings::fromString (const juce::String& text)
{
    MasterSettings m;
    const auto t = juce::StringArray::fromTokens (text, " ", "");
    if (t.size() < 7)
        return m;
    m.volumeDb = juce::jlimit (-24.0f, 6.0f, t[0].getFloatValue());
    m.reverb = { t[1] == "1", juce::jlimit (0.0f, 1.0f, t[2].getFloatValue()), juce::jlimit (0.0f, 1.0f, t[3].getFloatValue()) };
    m.delay = { t[4] == "1", juce::jlimit (0.0f, 1.0f, t[5].getFloatValue()),
                juce::jlimit (0, (int) delayBeats.size() - 1, t[6].getIntValue()) };
    return m;
}

void applyMaster (juce::AudioBuffer<float>& audio, double sampleRate, double bpm, const MasterSettings& m)
{
    if (audio.getNumSamples() == 0)
        return;
    if (m.delay.on && m.delay.amount > 0.0f)
        applyDelay (audio, sampleRate, bpm, m.delay);
    if (m.reverb.on && m.reverb.amount > 0.0f)
        applyReverb (audio, sampleRate, m.reverb);
    audio.applyGain (juce::Decibels::decibelsToGain (m.volumeDb));

    const float peak = audio.getMagnitude (0, audio.getNumSamples());
    if (peak > 0.98f)
        audio.applyGain (0.98f / peak);
}

} // namespace drb
