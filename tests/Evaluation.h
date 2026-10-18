#pragma once

#include "Analyzer.h"

#include <array>

// Scores an analysis against known hits, the way drum transcription is
// usually measured: a hit counts when it lands within a time window.
namespace drb::test
{

struct Score
{
    int truePositives = 0, falsePositives = 0, falseNegatives = 0;

    double precision() const;
    double recall() const;
    double f() const;       // 1 = perfect
    int truthCount() const  { return truePositives + falseNegatives; }
};

struct Report
{
    Score onsets;                                  // "something was hit here", any instrument
    std::array<Score, numInstruments> instruments;
    double meanTimingErrorMs = 0.0;                // over matched onsets
    double bpmTruth = 0.0, bpmDetected = 0.0;
    bool bpmCorrect = false;                       // within 2 %
    bool bpmOctaveError = false;                   // half or double, within 2 %
    bool barsCorrect = false;
    double downbeatErrorMs = 0.0;                  // where bar 1 starts
};

struct Truth
{
    std::vector<Hit> hits;
    double bpm = 0.0;
    int bars = 0;
    double firstDownbeat = 0.0;
};

Report evaluate (const Truth&, const AnalysisResult& detected, double toleranceSec = 0.030);

} // namespace drb::test
