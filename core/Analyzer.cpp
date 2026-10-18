#include "Analyzer.h"

namespace drb
{

AnalysisResult analyse (const juce::AudioBuffer<float>& audio, double sampleRate)
{
    AnalysisResult r;
    const auto mono = mixToMono (audio);
    const auto of = computeOnsetFunction (mono, sampleRate);
    r.onsets = pickOnsets (of, mono, sampleRate);
    r.grid = estimateBeatGrid (of, r.onsets);
    // Stage 2 turns onsets into hits with instruments.
    return r;
}

} // namespace drb
