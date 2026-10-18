#include "SyntheticDrums.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace drb::test
{
namespace
{
constexpr double twoPi = 6.283185307179586;

// RBJ biquad, enough to shape noise into snare and hi-hat bands.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    static Biquad make (double sr, double freq, double q, bool highPass)
    {
        const double w = twoPi * freq / sr, c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        Biquad f;
        if (highPass)
        {
            f.b0 = (1.0 + c) / 2.0 / a0;
            f.b1 = -(1.0 + c) / a0;
        }
        else // band-pass, 0 dB peak
        {
            f.b0 = alpha / a0;
            f.b1 = 0.0;
        }
        f.b2 = highPass ? f.b0 : -alpha / a0;
        f.a1 = -2.0 * c / a0;
        f.a2 = (1.0 - alpha) / a0;
        return f;
    }

    double process (double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

struct Event
{
    int bar = 0, step = 0;     // step = sixteenth inside the bar
    Instrument instrument;
    float velocity;
};

// One voice into a mono buffer, starting at sample `start`, at most `length` samples.
void renderVoice (std::vector<float>& out, int start, int length, Instrument inst,
                  float velocity, double sr, std::mt19937& rng)
{
    std::uniform_real_distribution<double> noise (-1.0, 1.0);
    const int end = std::min ((int) out.size(), start + length);
    const int fade = (int) (0.005 * sr);   // for choked open hats

    auto bp = Biquad::make (sr, 3000.0, 0.7, false);
    auto hp = Biquad::make (sr, 7000.0, 0.7, true);
    double phase = 0.0;

    for (int i = start; i < end; ++i)
    {
        const double t = (i - start) / sr;
        const int left = end - i;
        const double choke = left < fade ? (double) left / fade : 1.0;
        double s = 0.0;

        switch (inst)
        {
            case Instrument::Kick:
            {
                const double freq = 50.0 + 100.0 * std::exp (-t / 0.03);
                phase += twoPi * freq / sr;
                s = std::sin (phase) * std::exp (-t / 0.25)
                  + 0.3 * noise (rng) * std::exp (-t / 0.003);   // beater click
                break;
            }
            case Instrument::Snare:
                phase += twoPi * 185.0 / sr;
                s = 0.5 * std::sin (phase) * std::exp (-t / 0.08)
                  + 1.2 * bp.process (noise (rng)) * std::exp (-t / 0.12);
                break;
            case Instrument::HiHatClosed:
                s = 0.6 * hp.process (noise (rng)) * std::exp (-t / 0.02);
                break;
            case Instrument::HiHatOpen:
                s = 0.5 * hp.process (noise (rng)) * std::exp (-t / 0.25);
                break;
            case Instrument::Clap:
            case Instrument::Rimshot:
            case Instrument::TomLow:
            case Instrument::TomMid:
            case Instrument::TomHigh:
            case Instrument::Crash:
            case Instrument::Ride:
                break;   // these come with stage 4
        }

        out[(size_t) i] += (float) (s * velocity * choke);
    }
}

std::vector<Event> pattern (Style style, int bars)
{
    std::vector<Event> ev;
    auto add = [&] (int bar, int step, Instrument i, float v) { ev.push_back ({ bar, step, i, v }); };

    for (int b = 0; b < bars; ++b)
    {
        const bool lastOfPair = (b % 2) == 1;

        switch (style)
        {
            case Style::Rock:
                add (b, 0, Instrument::Kick, 1.0f);
                add (b, 8, Instrument::Kick, 0.85f);
                if (lastOfPair) add (b, 10, Instrument::Kick, 0.7f);
                add (b, 4, Instrument::Snare, 0.95f);
                add (b, 12, Instrument::Snare, 0.95f);
                for (int s = 0; s < 16; s += 2)
                {
                    if (lastOfPair && s == 14)
                        add (b, s, Instrument::HiHatOpen, 0.75f);
                    else
                        add (b, s, Instrument::HiHatClosed, s % 4 == 0 ? 0.9f : 0.6f);
                }
                break;

            case Style::Funk:
                for (int s : { 0, 3, 10 }) add (b, s, Instrument::Kick, s == 0 ? 1.0f : 0.8f);
                if (lastOfPair) add (b, 13, Instrument::Kick, 0.7f);
                add (b, 4, Instrument::Snare, 1.0f);
                add (b, 12, Instrument::Snare, 1.0f);
                for (int s : { 6, 9, 15 }) add (b, s, Instrument::Snare, 0.25f);   // ghost notes
                for (int s = 0; s < 16; ++s)
                {
                    if (s == 7 && lastOfPair)
                        add (b, s, Instrument::HiHatOpen, 0.7f);
                    else
                        add (b, s, Instrument::HiHatClosed, s % 2 == 0 ? 0.8f : 0.45f);
                }
                break;

            case Style::Sparse:
                add (b, 0, Instrument::Kick, 1.0f);
                add (b, 11, Instrument::Kick, 0.75f);
                add (b, 8, Instrument::Snare, 1.0f);
                for (int s = 0; s < 16; s += 4)
                    add (b, s, Instrument::HiHatClosed, 0.7f);
                break;
        }
    }
    return ev;
}

// Position of a sixteenth step in beats, with swing on pairs of `unit` steps.
double swungBeats (int step, int unit, double swing)
{
    const int pairLen = 2 * unit;
    const int pairStart = step - step % pairLen;
    const double q = step % pairLen;
    const double inPair = q < unit ? q / unit * swing * pairLen
                                   : swing * pairLen + (q - unit) / unit * (1.0 - swing) * pairLen;
    return (pairStart + inPair) / 4.0;
}
} // namespace

Beat makeBeat (const BeatSpec& spec)
{
    Beat beat;
    beat.spec = spec;

    std::mt19937 rng (spec.seed);
    std::normal_distribution<double> jitter (0.0, spec.jitterMs / 1000.0);
    std::uniform_real_distribution<float> velJitter (-0.05f, 0.05f);

    const double sr = spec.sampleRate;
    const double beatSec = 60.0 / spec.bpm;
    const double leadIn = 0.1;
    const int swingUnit = spec.style == Style::Funk ? 1 : 2;   // sixteenth or eighth swing
    beat.firstDownbeat = leadIn;

    for (const auto& e : pattern (spec.style, spec.bars))
    {
        double t = leadIn + (e.bar * 4 + swungBeats (e.step, swingUnit, spec.swing)) * beatSec;
        if (spec.jitterMs > 0.0)
            t = std::max (0.0, t + jitter (rng));
        const float v = juce::jlimit (0.05f, 1.0f, e.velocity + velJitter (rng));
        beat.truth.push_back ({ t, e.instrument, v, 1.0f, false });
    }

    std::sort (beat.truth.begin(), beat.truth.end(),
               [] (const Hit& a, const Hit& b) { return a.time < b.time; });

    const double total = leadIn + spec.bars * 4 * beatSec + 1.0;   // room for the last tail
    const int n = (int) std::ceil (total * sr);

    // Each instrument on its own mono track, then panned into stereo.
    std::array<std::vector<float>, numInstruments> tracks;
    for (auto& t : tracks)
        t.assign ((size_t) n, 0.0f);

    for (size_t i = 0; i < beat.truth.size(); ++i)
    {
        const auto& h = beat.truth[i];
        int length = (int) (1.0 * sr);

        // A hi-hat stops the open one still ringing.
        if (h.instrument == Instrument::HiHatOpen)
            for (size_t j = i + 1; j < beat.truth.size(); ++j)
                if (beat.truth[j].instrument == Instrument::HiHatClosed
                    || beat.truth[j].instrument == Instrument::HiHatOpen)
                {
                    length = std::max ((int) (0.03 * sr), (int) ((beat.truth[j].time - h.time) * sr));
                    break;
                }

        renderVoice (tracks[(size_t) h.instrument], (int) std::lround (h.time * sr), length,
                     h.instrument, h.velocity, sr, rng);
    }

    auto pan = [] (Instrument i)
    {
        if (i == Instrument::Snare)
            return 0.1f;
        if (i == Instrument::HiHatClosed || i == Instrument::HiHatOpen)
            return 0.3f;
        return 0.0f;
    };

    beat.audio.setSize (2, n);
    beat.audio.clear();
    for (auto inst : allInstruments)
    {
        const float p = pan (inst);
        const auto& t = tracks[(size_t) inst];
        const float gl = std::cos ((p + 1.0f) * juce::MathConstants<float>::pi / 4.0f);
        const float gr = std::sin ((p + 1.0f) * juce::MathConstants<float>::pi / 4.0f);
        for (int i = 0; i < n; ++i)
        {
            beat.audio.addSample (0, i, t[(size_t) i] * gl);
            beat.audio.addSample (1, i, t[(size_t) i] * gr);
        }
    }

    const float peak = beat.audio.getMagnitude (0, n);
    if (peak > 0.0f)
        beat.audio.applyGain (0.9f / peak);

    return beat;
}

std::vector<BeatSpec> standardSet()
{
    return {
        { "rock_90_straight",  Style::Rock,   90.0, 4, 0.50, 0.0, 44100.0, 11 },
        { "rock_120_jitter",   Style::Rock,  120.0, 4, 0.50, 6.0, 44100.0, 12 },
        { "rock_174_fast",     Style::Rock,  174.0, 8, 0.50, 2.0, 48000.0, 13 },
        { "funk_100_swing",    Style::Funk,  100.0, 4, 0.60, 3.0, 44100.0, 14 },
        { "funk_128_straight", Style::Funk,  128.0, 4, 0.50, 0.0, 48000.0, 15 },
        { "sparse_75",         Style::Sparse, 75.0, 4, 0.50, 4.0, 44100.0, 16 },
    };
}

} // namespace drb::test
