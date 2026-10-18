#include "Tempo.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>

namespace drb
{
namespace
{
// Linear interpolation into v, 0 outside it.
double at (const std::vector<float>& v, double x)
{
    const auto i = (int) std::floor (x);
    if (i < 0 || i + 1 >= (int) v.size())
        return 0.0;
    const double frac = x - i;
    return v[(size_t) i] * (1.0 - frac) + v[(size_t) i + 1] * frac;
}

// What stands above the local (~0.4 s) average.
std::vector<float> detrend (const std::vector<float>& v, double hopSeconds)
{
    const int n = (int) v.size();
    const int w = std::max (1, (int) std::lround (0.4 / hopSeconds));
    std::vector<double> prefix ((size_t) n + 1, 0.0);
    for (int i = 0; i < n; ++i)
        prefix[(size_t) i + 1] = prefix[(size_t) i] + v[(size_t) i];

    std::vector<float> out ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const int a = std::max (0, i - w), b = std::min (n, i + w + 1);
        out[(size_t) i] = std::max (0.0f, v[(size_t) i] - (float) ((prefix[(size_t) b] - prefix[(size_t) a]) / (b - a)));
    }
    return out;
}
} // namespace

// Same method as Adormis Audio's measureTempo (autocorrelation scored on the
// beat, its multiples and half a beat, then a phase-locked refinement), here
// on a finer onset function and also returning the grid's phase.
BeatGrid estimateBeatGrid (const OnsetFunction& of, const std::vector<Onset>& onsets)
{
    BeatGrid grid;
    const auto onset = detrend (of.all, of.hopSeconds);
    const int frames = (int) onset.size();
    const double hop = of.hopSeconds;
    constexpr double minBpm = 60.0, maxBpm = 200.0;

    // At least two bars at the slowest tempo.
    if (onsets.size() < 4 || frames < (int) (8.0 * 60.0 / maxBpm / hop))
        return grid;

    const int maxLag = std::min (frames - 2, (int) std::ceil (4.0 * 60.0 / minBpm / hop) + 2);
    std::vector<float> ac ((size_t) maxLag + 2, 0.0f);
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double s = 0.0;
        for (int i = 0; i + lag < frames; ++i)
            s += (double) onset[(size_t) i] * onset[(size_t) (i + lag)];
        ac[(size_t) lag] = (float) (s / (frames - lag));
    }

    double best = 0.0, bestScore = -1.0, total = 0.0;
    int count = 0;
    for (double bpm = minBpm; bpm <= maxBpm; bpm += 0.05)
    {
        const double lag = 60.0 / bpm / hop;
        const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 115.0) / 0.85, 2.0));
        const double score = (at (ac, lag) + 0.5 * at (ac, 2.0 * lag) + 0.33 * at (ac, 3.0 * lag)
                              + 0.25 * at (ac, 4.0 * lag) + 0.4 * at (ac, 0.5 * lag)) * prior;
        total += score;
        ++count;
        if (score > bestScore)
        {
            bestScore = score;
            best = bpm;
        }
    }

    if (bestScore <= 0.0 || bestScore < 1.25 * total / count)
        return grid;

    // Refine tempo and phase together: the grid that collects the most onset
    // strength on its beats (and, half as much, on the eighths between).
    double refined = best, refinedScore = -1.0, phase = 0.0;
    for (double bpm = best * 0.985; bpm <= best * 1.015; bpm += 0.01)
    {
        const double period = 60.0 / bpm / hop;
        for (double p = 0.0; p < period; p += 0.5)
        {
            double sum = 0.0;
            for (double x = p; x < frames; x += period)
                sum += at (onset, x) + 0.5 * at (onset, x + period * 0.5);
            if (sum > refinedScore)
            {
                refinedScore = sum;
                refined = bpm;
                phase = p;
            }
        }
    }

    const double rounded = std::round (refined);
    grid.bpm = std::abs (refined - rounded) < 0.15 ? rounded : std::round (refined * 10.0) / 10.0;
    grid.confidence = (float) juce::jlimit (0.0, 1.0, (bestScore / (total / count) - 1.25) / 2.0);

    const double beat = grid.beatSeconds();
    const double firstHit = onsets.front().time, lastHit = onsets.back().time;

    // Beat grid anchored on the hit closest to the refined phase, so it lands
    // on real attacks rather than on frame centres.
    double anchor = of.timeOf (phase);
    for (const auto& o : onsets)
        if (std::abs (o.time - of.timeOf (phase)) < 0.03)
        {
            anchor = o.time;
            break;
        }
    grid.firstBeat = anchor - std::floor ((anchor - firstHit + 0.05) / beat) * beat;

    // Which of the 4 beats is the "1": the kick (low band) lands most on it.
    // Candidates close to the best win by starting nearest the first hit,
    // since recordings almost always start on a downbeat.
    const int beatsPerBar = grid.timeSignature.beatsPerBar;
    std::array<double, 4> kickOn {};
    for (int k = 0; grid.firstBeat + k * beat <= lastHit + 0.05; ++k)
    {
        // The low-band flux peaks up to a frame before the attack.
        const double t = grid.firstBeat + k * beat;
        const int a = (int) std::floor ((t - 0.025 - of.offsetSeconds) / hop);
        const int b = (int) std::ceil ((t + 0.010 - of.offsetSeconds) / hop);
        float m = 0.0f;
        for (int i = std::max (0, a); i <= std::min ((int) of.low.size() - 1, b); ++i)
            m = std::max (m, of.low[(size_t) i]);
        kickOn[(size_t) (k % beatsPerBar)] += m;
    }

    const double bestKick = *std::max_element (kickOn.begin(), kickOn.end());
    int downbeatIndex = 0;
    for (int j = 0; j < beatsPerBar; ++j)
        if (kickOn[(size_t) j] >= 0.7 * bestKick)
        {
            downbeatIndex = j;   // the earliest strong candidate
            break;
        }

    grid.firstDownbeat = grid.firstBeat + downbeatIndex * beat;
    const double bar = beatsPerBar * beat;
    grid.bars = std::max (1, (int) std::ceil ((lastHit - grid.firstDownbeat + 0.05) / bar));
    return grid;
}

} // namespace drb
