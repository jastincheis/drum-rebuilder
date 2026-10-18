#include "Evaluation.h"

#include <cmath>

namespace drb::test
{
namespace
{
// Greedy one-to-one matching of two sorted time lists; adds the matched
// errors (seconds) to `errors`.
Score match (const std::vector<double>& truth, const std::vector<double>& found,
             double tolerance, std::vector<double>* errors = nullptr)
{
    Score s;
    std::vector<bool> used (found.size(), false);

    for (double t : truth)
    {
        int best = -1;
        double bestDist = tolerance;
        for (size_t j = 0; j < found.size(); ++j)
        {
            const double d = std::abs (found[j] - t);
            if (! used[j] && d <= bestDist)
            {
                best = (int) j;
                bestDist = d;
            }
        }

        if (best >= 0)
        {
            used[(size_t) best] = true;
            ++s.truePositives;
            if (errors != nullptr)
                errors->push_back (bestDist);
        }
        else
        {
            ++s.falseNegatives;
        }
    }

    for (bool u : used)
        if (! u)
            ++s.falsePositives;
    return s;
}

// Hits closer than 25 ms are one onset (a kick and a hat played together,
// or a flam): the onset detector doesn't split attacks closer than that.
// Each instrument still gets scored on its own times.
std::vector<double> onsetTimes (const std::vector<Hit>& hits)
{
    std::vector<double> t;
    for (const auto& h : hits)
        if (t.empty() || h.time - t.back() > 0.025)
            t.push_back (h.time);
    return t;
}

std::vector<double> timesOf (const std::vector<Hit>& hits, Instrument i)
{
    std::vector<double> t;
    for (const auto& h : hits)
        if (h.instrument == i)
            t.push_back (h.time);
    return t;
}

bool within (double a, double b, double ratio) { return b > 0.0 && std::abs (a / b - 1.0) <= ratio; }
} // namespace

double Score::precision() const
{
    const int n = truePositives + falsePositives;
    return n == 0 ? 0.0 : (double) truePositives / n;
}

double Score::recall() const
{
    const int n = truePositives + falseNegatives;
    return n == 0 ? 0.0 : (double) truePositives / n;
}

double Score::f() const
{
    const double p = precision(), r = recall();
    return p + r <= 0.0 ? 0.0 : 2.0 * p * r / (p + r);
}

Report evaluate (const Truth& t, const AnalysisResult& detected, double toleranceSec)
{
    const auto& truth = t.hits;
    Report r;
    std::vector<double> errors, found;
    if (detected.onsets.empty())
        found = onsetTimes (detected.hits);
    else
        for (const auto& o : detected.onsets)
            found.push_back (o.time);
    r.onsets = match (onsetTimes (truth), found, toleranceSec, &errors);

    double sum = 0.0;
    for (double e : errors)
        sum += e;
    r.meanTimingErrorMs = errors.empty() ? 0.0 : 1000.0 * sum / (double) errors.size();

    for (auto inst : allInstruments)
        r.instruments[(size_t) inst] = match (timesOf (truth, inst), timesOf (detected.hits, inst), toleranceSec);

    const double bpm = detected.grid.bpm;
    r.bpmTruth = t.bpm;
    r.bpmDetected = bpm;
    r.bpmCorrect = within (bpm, t.bpm, 0.02);
    r.bpmOctaveError = ! r.bpmCorrect
                    && (within (bpm, t.bpm * 2.0, 0.02) || within (bpm, t.bpm / 2.0, 0.02));
    r.barsCorrect = detected.grid.bars == t.bars;
    r.downbeatErrorMs = 1000.0 * std::abs (detected.grid.firstDownbeat - t.firstDownbeat);
    return r;
}

} // namespace drb::test
