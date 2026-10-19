#include "HitGains.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace drb
{

namespace
{
constexpr double lowMid = 150.0, midHigh = 5000.0;   // Hz
constexpr double togetherSeconds = 0.03;             // hits closer than this are played together
constexpr double leadSeconds = 0.001;                // a change starts this long before its hit
constexpr double rampSeconds = 0.003;

enum Band { lowBand, midBand, highBand };

// How much of an instrument's sound is in each band (lows, mids, highs).
std::array<float, 3> weightsOf (Instrument i)
{
    switch (i)
    {
        case Instrument::Kick:    return { 1.0f, 0.5f, 0.2f };   // body, click
        case Instrument::TomLow:
        case Instrument::TomMid:
        case Instrument::TomHigh: return { 1.0f, 0.9f, 0.2f };
        case Instrument::Snare:
        case Instrument::Clap:
        case Instrument::Rimshot: return { 0.3f, 1.0f, 0.7f };   // body, wires
        case Instrument::HiHatClosed:
        case Instrument::HiHatOpen: return { 0.0f, 0.1f, 1.0f };
        case Instrument::Crash:
        case Instrument::Ride:    return { 0.0f, 0.3f, 1.0f };
    }
    return { 1.0f, 1.0f, 1.0f };
}

// The bands an instrument's sound is mostly in.
std::array<bool, 3> bandsOf (Instrument i)
{
    const auto w = weightsOf (i);
    return { w[0] >= 0.5f, w[1] >= 0.5f, w[2] >= 0.5f };
}

// Linkwitz-Riley (two Butterworths): low + high add up flat.
void filter (juce::AudioBuffer<float>& b, double rate, double hz, bool high)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int pass = 0; pass < 2; ++pass)
        {
            juce::IIRFilter f;
            f.setCoefficients (high ? juce::IIRCoefficients::makeHighPass (rate, hz, 0.7071)
                                    : juce::IIRCoefficients::makeLowPass (rate, hz, 0.7071));
            f.processSamples (b.getWritePointer (ch), b.getNumSamples());
        }
}

// One sound of the recording at its place: a hit, or the empty place a
// hit was moved away from (gain 0: its sound is silenced there).
struct Entry
{
    double time = 0.0;
    Instrument instrument = Instrument::Kick;
    float gain = 1.0f;
};

struct Moment
{
    double time = 0.0;
    std::array<float, 3> gain { 1.0f, 1.0f, 1.0f };
    std::vector<Entry> entries;
};

// The hits grouped into moments played together, each with its band gains:
// a band follows the hit whose sound is most in it (kick + snare: the lows
// follow the kick, the mids and highs the snare). The lows are the
// exception: with no kick or tom there they are the last one still
// ringing, so they keep its gain. Hits added by hand have no sound of their
// own in the recording and are left out; a moved hit's sound is taken out
// where it was.
std::vector<Moment> moments (const std::vector<Hit>& hits, const InstrumentGains* instrumentGains)
{
    std::vector<Entry> entries;
    for (const auto& h : hits)
    {
        if (h.added)
            continue;
        const float scale = instrumentGains != nullptr ? (*instrumentGains)[(size_t) h.instrument] : 1.0f;
        const bool moved = std::abs (h.sourceTime() - h.time) > 1.0e-9;
        entries.push_back ({ h.sourceTime(), h.instrument, moved ? 0.0f : h.soundGain() * scale });
    }
    std::stable_sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b) { return a.time < b.time; });

    std::vector<Moment> out;
    for (const auto& e : entries)
    {
        if (out.empty() || e.time > out.back().time + togetherSeconds)
            out.push_back ({ e.time, {}, {} });
        out.back().entries.push_back (e);
    }

    float lowCarry = -1.0f;
    for (auto& m : out)
    {
        for (int b = 0; b < 3; ++b)
        {
            // The strongest in this band; equally strong: the louder change.
            float weight = -1.0f, gain = 1.0f;
            for (const auto& e : m.entries)
            {
                const float w = weightsOf (e.instrument)[(size_t) b];
                if (w > weight + 1.0e-6f || (std::abs (w - weight) <= 1.0e-6f && e.gain > gain))
                {
                    weight = w;
                    gain = e.gain;
                }
            }
            m.gain[(size_t) b] = gain;
            if (b == lowBand && weight < 0.5f && lowCarry >= 0.0f)
                m.gain[(size_t) b] = lowCarry;
        }
        lowCarry = m.gain[lowBand];
    }
    return out;
}
} // namespace

Bands splitBands (const juce::AudioBuffer<float>& audio, double rate)
{
    Bands b;
    b.sampleRate = rate;
    b.low = audio;
    filter (b.low, rate, lowMid, false);
    juce::AudioBuffer<float> rest (audio);
    filter (rest, rate, lowMid, true);
    b.mid = rest;
    filter (b.mid, rate, midHigh, false);
    b.high = std::move (rest);
    filter (b.high, rate, midHigh, true);
    return b;
}

bool hasHitGains (const std::vector<Hit>& hits)
{
    return std::any_of (hits.begin(), hits.end(), [] (const Hit& h)
    {
        return std::abs (h.soundGain() - 1.0f) > 1.0e-4f || h.added || std::abs (h.sourceTime() - h.time) > 1.0e-9;
    });
}

juce::AudioBuffer<float> applyHitGains (const Bands& bands, const std::vector<Hit>& hits, const InstrumentGains* instrumentGains)
{
    const int n = bands.low.getNumSamples(), channels = bands.low.getNumChannels();
    juce::AudioBuffer<float> out (channels, n);
    out.clear();

    const auto ms = moments (hits, instrumentGains);
    const double rate = bands.sampleRate;
    const int ramp = std::max (1, (int) (rampSeconds * rate));
    const juce::AudioBuffer<float>* src[3] { &bands.low, &bands.mid, &bands.high };

    for (int b = 0; b < 3; ++b)
    {
        // [from, to) at `gain`, then a short ramp to the next moment's.
        // Before the first hit: as it is, or silent when only some play.
        float gain = instrumentGains != nullptr ? 0.0f : 1.0f;
        int from = 0;
        for (size_t k = 0; k <= ms.size(); ++k)
        {
            const int to = k < ms.size() ? juce::jlimit (from, n, (int) ((ms[k].time - leadSeconds) * rate) - ramp) : n;
            for (int ch = 0; ch < channels; ++ch)
                out.addFrom (ch, from, *src[b], ch, from, to - from, gain);
            if (k == ms.size())
                break;
            const float next = ms[k].gain[(size_t) b];
            const int end = std::min (n, to + ramp);
            for (int ch = 0; ch < channels; ++ch)
                out.addFromWithRamp (ch, to, src[b]->getReadPointer (ch, to), end - to, gain, next);
            gain = next;
            from = end;
        }
    }
    return out;
}

float hitLevelDb (const Bands& bands, const std::vector<Hit>& hits, int hitIndex)
{
    if (! juce::isPositiveAndBelow (hitIndex, (int) hits.size()))
        return -100.0f;
    const auto& h = hits[(size_t) hitIndex];
    const double rate = bands.sampleRate;
    const int n = bands.low.getNumSamples();

    // Its sound in the recording (also for a moved or borrowed one): from
    // there to the next sound after it (at most 0.5 s).
    double end = h.sourceTime() + 0.5;
    for (const auto& o : hits)
        if (! o.added && o.sourceTime() > h.sourceTime() + togetherSeconds)
            end = std::min (end, o.sourceTime() - leadSeconds);
    const int a = juce::jlimit (0, n, (int) ((h.sourceTime() - leadSeconds) * rate));
    const int b = juce::jlimit (a, n, (int) (end * rate));
    if (b <= a)
        return -100.0f;

    // Its own bands, added up sample by sample.
    const auto own = bandsOf (h.instrument);
    float peak = 0.0f;
    for (int ch = 0; ch < bands.low.getNumChannels(); ++ch)
        for (int i = a; i < b; ++i)
        {
            float v = 0.0f;
            if (own[0]) v += bands.low.getSample (ch, i);
            if (own[1]) v += bands.mid.getSample (ch, i);
            if (own[2]) v += bands.high.getSample (ch, i);
            peak = std::max (peak, std::abs (v));
        }
    return juce::Decibels::gainToDecibels (peak * h.soundGain(), -100.0f);
}

} // namespace drb
