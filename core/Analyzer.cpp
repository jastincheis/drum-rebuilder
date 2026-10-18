#include "Analyzer.h"

namespace drb
{

AnalysisResult analyse (const juce::AudioBuffer<float>&, double)
{
    // Stage 1 adds onset and tempo detection, stage 2 the instruments.
    return {};
}

} // namespace drb
