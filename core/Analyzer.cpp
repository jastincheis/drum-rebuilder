#include "Analyzer.h"
#include "Classify.h"

namespace drb
{

AnalysisResult analyse (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    AnalysisResult r;
    const auto mono = mixToMono (audio);
    const auto of = computeOnsetFunction (mono, sampleRate);
    r.onsets = pickOnsets (of, mono, sampleRate);
    r.grid = estimateBeatGrid (of, r.onsets);
    r.hits = classifyHits (r.onsets, mono, sampleRate);

    // Onsets where nothing new sounded got no hit: not a hit after all.
    std::vector<Onset> kept;
    size_t h = 0;
    for (const auto& o : r.onsets)
    {
        while (h < r.hits.size() && r.hits[h].time < o.time - 1.0e-9)
            ++h;
        if (h < r.hits.size() && std::abs (r.hits[h].time - o.time) < 1.0e-9)
            kept.push_back (o);
    }
    r.onsets = std::move (kept);
    refineGridWithHits (r.grid, r.hits, r.onsets);
    return r;
}

} // namespace drb
