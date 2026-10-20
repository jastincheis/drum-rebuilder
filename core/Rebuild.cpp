#include "Rebuild.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace drb
{

const char* pickModeName (PickMode m)
{
    switch (m)
    {
        case PickMode::Original:   return "Original";
        case PickMode::Fixed:      return "Fixed";
        case PickMode::Random:     return "Random";
        case PickMode::RoundRobin: return "Round robin";
        case PickMode::Velocity:   return "By velocity";
    }
    return "?";
}

namespace
{
constexpr double fadeInSeconds = 0.002, fadeOutSeconds = 0.010;
constexpr double lengthFadeSeconds = 0.015;   // where a sound is cut short (InstrumentPlan::maxLength)
constexpr double startFadeSeconds = 0.002;    // where a sound starts later than it does (InstrumentPlan::startAt)

// Adds src[from, to) into `out`, starting at `startSeconds`, read `pitch`
// times faster (2 = an octave up, and half as long), with a gain and an
// optional envelope per source sample. Cubic interpolation.
void addVoice (juce::AudioBuffer<float>& out, double outRate, double startSeconds,
               const juce::AudioBuffer<float>& src, double srcRate, int from, int to,
               double pitch, float gain, const std::vector<float>* envelope = nullptr)
{
    if (to - from < 4 || gain <= 0.0f)
        return;

    const double step = pitch * srcRate / outRate;   // source samples per output sample
    const int first = std::max (0, (int) std::ceil (startSeconds * outRate));
    double pos = from + (first / outRate - startSeconds) * srcRate * pitch;
    const int srcChannels = src.getNumChannels();

    for (int i = first; i < out.getNumSamples() && pos < to - 2; ++i, pos += step)
    {
        if (pos < from + 1)
            continue;
        const int k = (int) pos;
        const float f = (float) (pos - k);
        const float env = envelope != nullptr ? (*envelope)[(size_t) (k - from)] : 1.0f;

        for (int ch = 0; ch < out.getNumChannels(); ++ch)
        {
            const auto* s = src.getReadPointer (std::min (ch, srcChannels - 1));
            const float y0 = s[k - 1], y1 = s[k], y2 = s[k + 1], y3 = s[k + 2];
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            out.addSample (ch, i, gain * env * (((c3 * f + c2) * f + c1) * f + y1));
        }
    }
}

// The gate of one cut: 0 outside, short fades at both ends.
float gate (int i, int a, int b, int fadeIn, int fadeOut)
{
    if (i < a || i >= b)
        return 0.0f;
    return std::min ({ 1.0f, (float) (i - a) / (float) std::max (1, fadeIn), (float) (b - i) / (float) std::max (1, fadeOut) });
}
} // namespace

juce::AudioBuffer<float> renderRebuild (const RebuildInput& input, const RebuildSettings& settings, double outputRate,
                                        double tailSeconds)
{
    const auto& src = *input.audio;
    const double sr = input.sampleRate;
    const double f = std::clamp (settings.tempoFactor, 0.25, 4.0);
    const int fadeIn = (int) (fadeInSeconds * sr), fadeOut = (int) (fadeOutSeconds * sr);

    juce::AudioBuffer<float> out (2, (int) std::ceil ((src.getNumSamples() / sr / f + tailSeconds) * outputRate));
    out.clear();
    const auto& hits = input.hits;
    if (hits.empty() || hits.size() != input.slices.size())
        return out;

    auto range = [&] (size_t h)
    {
        const int a = std::clamp ((int) std::floor (input.slices[h].first * sr), 0, src.getNumSamples());
        const int b = std::clamp ((int) std::ceil (input.slices[h].second * sr), a, src.getNumSamples());
        return std::pair { a, b };
    };
    // The same, from the instrument's IN to its OUT (both from the hit).
    auto playedRange = [&] (size_t h)
    {
        auto [a, b] = range (h);
        const auto& plan = settings.instruments[(size_t) hits[h].instrument];
        if (plan.maxLength > 0.0)
            b = std::clamp ((int) std::ceil ((hits[h].sourceTime() + plan.maxLength) * sr), a, b);
        if (plan.startAt > 0.0)
            a = std::clamp ((int) std::floor ((hits[h].sourceTime() + plan.startAt) * sr), a, b);
        return std::pair { a, b };
    };

    // Each instrument's loudest original hit: the level and pitch new sounds
    // are matched to.
    std::array<float, numInstruments> refPeak {}, refPitch {};
    for (auto inst : allInstruments)
    {
        int loudest = -1;
        for (size_t h = 0; h < hits.size(); ++h)
            if (hits[h].instrument == inst && (loudest < 0 || hits[h].recordedVelocity() > hits[(size_t) loudest].recordedVelocity()))
                loudest = (int) h;
        if (loudest < 0)
            continue;
        const auto [a, b] = range ((size_t) loudest);
        if (b - a < 16)
            continue;
        juce::AudioBuffer<float> slice (src.getNumChannels(), b - a);
        for (int ch = 0; ch < src.getNumChannels(); ++ch)
            slice.copyFrom (ch, 0, src, ch, a, b - a);
        refPeak[(size_t) inst] = slice.getMagnitude (0, b - a) / std::max (0.05f, hits[(size_t) loudest].recordedVelocity());
        refPitch[(size_t) inst] = estimatePitch (slice, sr, inst);
    }

    std::mt19937 rng (settings.seed);
    std::array<int, numInstruments> turn {};
    std::vector<bool> done (hits.size(), false);

    for (size_t h = 0; h < hits.size(); ++h)
    {
        if (done[h])
            continue;
        const auto& hit = hits[h];
        const auto& plan = settings.instruments[(size_t) hit.instrument];
        const bool useOriginal = plan.mode == PickMode::Original || plan.pool.empty();

        if (useOriginal)
        {
            // Every original hit at this same moment: their cuts are the same
            // stretch of the recording, so it plays once, opened as far as
            // the widest of them.
            std::vector<size_t> group;
            for (size_t g = h; g < hits.size() && hits[g].time < hit.time + 1.0e-9; ++g)
            {
                const auto& p = settings.instruments[(size_t) hits[g].instrument];
                if (! done[g] && (p.mode == PickMode::Original || p.pool.empty())
                    && std::abs (hits[g].sourceTime() - hit.sourceTime()) < 1.0e-9)
                    group.push_back (g);
            }

            int a = src.getNumSamples(), b = 0;
            double startAt = 1.0e9;   // the IN of the one starting earliest
            for (auto g : group)
            {
                const auto [ga, gb] = playedRange (g);
                a = std::min (a, ga);
                b = std::max (b, gb);
                startAt = std::min (startAt, settings.instruments[(size_t) hits[g].instrument].startAt);
                done[g] = true;
            }
            if (b - a < 4)
                continue;

            std::vector<float> envelope ((size_t) (b - a), 0.0f);
            for (auto g : group)
            {
                const auto [ga, gb] = playedRange (g);
                const bool cut = gb < range (g).second;
                const int out = cut ? (int) (lengthFadeSeconds * sr) : fadeOut;
                for (int i = ga; i < gb; ++i)
                    envelope[(size_t) (i - a)] = std::max (envelope[(size_t) (i - a)],
                                                           gate (i, ga, gb, fadeIn, out) * hits[g].soundGain());
            }
            // Started later (IN): what is left begins at the hit.
            const double start = hit.time / f + (a / sr - hit.sourceTime()) - startAt;
            addVoice (out, outputRate, start, src, sr, a, b, 1.0, 1.0f, &envelope);
            continue;
        }

        // A sound from the pool.
        done[h] = true;
        const auto& pool = plan.pool;
        const int n = (int) pool.size();
        int index = 0;
        switch (plan.mode)
        {
            case PickMode::Fixed:      index = ((plan.fixed % n) + n) % n; break;
            case PickMode::Random:     index = std::uniform_int_distribution<int> (0, n - 1) (rng); break;
            case PickMode::RoundRobin: index = turn[(size_t) hit.instrument]++ % n; break;
            case PickMode::Velocity:
            {
                std::vector<int> order ((size_t) n);
                for (int k = 0; k < n; ++k)
                    order[(size_t) k] = k;
                std::sort (order.begin(), order.end(), [&] (int x, int y) { return pool[(size_t) x]->peak < pool[(size_t) y]->peak; });
                index = order[(size_t) std::clamp ((int) std::lround (hit.velocity * (float) (n - 1)), 0, n - 1)];
                break;
            }
            case PickMode::Original: break;
        }

        const auto& sample = *pool[(size_t) index];
        const float target = refPeak[(size_t) hit.instrument] > 0.0f ? refPeak[(size_t) hit.instrument] * hit.velocity : hit.velocity;
        // Matched to the original's level, but a very quiet sound is not
        // blown up more than 2.5 times (it would bring its noise along).
        const float gain = sample.peak > 0.0f ? std::min (2.5f, target / sample.peak) : 0.0f;

        double pitch = 1.0;
        if (settings.keepPitch && refPitch[(size_t) hit.instrument] > 0.0f && sample.pitch > 0.0f)
            pitch = std::clamp ((double) refPitch[(size_t) hit.instrument] / sample.pitch, 0.5, 2.0);

        // From the instrument's IN to its OUT, with short fades there.
        int end = sample.audio.getNumSamples();
        if (plan.maxLength > 0.0 && plan.maxLength * sample.sampleRate < end)
            end = std::max (1, (int) std::ceil (plan.maxLength * sample.sampleRate));
        const int begin = std::clamp ((int) std::floor (plan.startAt * sample.sampleRate), 0, std::max (0, end - 4));
        std::vector<float> fade;
        if (end < sample.audio.getNumSamples() || begin > 0)
        {
            fade.assign ((size_t) (end - begin), 1.0f);
            if (end < sample.audio.getNumSamples())
                for (int i = 0, fadeLen = std::min (end - begin, (int) (lengthFadeSeconds * sample.sampleRate)); i < fadeLen; ++i)
                    fade[(size_t) (end - begin - 1 - i)] = (float) i / (float) fadeLen;
            if (begin > 0)
                for (int i = 0, fadeLen = std::min (end - begin, (int) (startFadeSeconds * sample.sampleRate)); i < fadeLen; ++i)
                    fade[(size_t) i] *= (float) i / (float) fadeLen;
        }
        addVoice (out, outputRate, hit.time / f, sample.audio, sample.sampleRate, begin, end, pitch, gain, fade.empty() ? nullptr : &fade);
    }

    // Never clips: louder sounds than the original's add up, so the whole
    // beat comes down if it would go over.
    const float peak = out.getMagnitude (0, out.getNumSamples());
    if (peak > 0.98f)
        out.applyGain (0.98f / peak);
    return out;
}

} // namespace drb
