// drb_tests: checks the building blocks, then runs the analyzer on the
// synthetic beats and prints how well it found the hits.
//
//   drb_tests                 run everything
//   drb_tests --write <dir>   also save each beat as WAV + its hits as CSV, to listen to

#include "Analyzer.h"
#include "Evaluation.h"
#include "SyntheticDrums.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstdio>
#include <set>

using namespace drb;

namespace
{
int failures = 0;

void check (bool ok, const char* what)
{
    if (! ok)
    {
        std::printf ("FAIL  %s\n", what);
        ++failures;
    }
}

void checkBasics()
{
    std::set<int> notes;
    for (auto i : allInstruments)
        notes.insert (gmNote (i));
    check (notes.size() == (size_t) numInstruments, "every instrument has its own GM note");

    for (const auto& spec : test::standardSet())
    {
        const auto beat = test::makeBeat (spec);
        check (! beat.truth.empty(), "beat has hits");
        check (beat.audio.getMagnitude (0, beat.audio.getNumSamples()) > 0.1f, "beat is not silent");
        check (std::is_sorted (beat.truth.begin(), beat.truth.end(),
                               [] (const Hit& a, const Hit& b) { return a.time < b.time; }),
               "hits are sorted");

        // The scorer itself: the truth scored against itself must be perfect.
        AnalysisResult perfect;
        perfect.bpm = spec.bpm;
        perfect.hits = beat.truth;
        const auto r = test::evaluate (beat.truth, spec.bpm, perfect);
        check (r.onsets.f() > 0.9999 && r.bpmCorrect, "scorer gives 1.0 to the truth");

        AnalysisResult doubled = perfect;
        doubled.bpm = spec.bpm * 2.0;
        check (test::evaluate (beat.truth, spec.bpm, doubled).bpmOctaveError, "scorer spots double tempo");
    }
}

void writeBeat (const test::Beat& beat, const juce::File& dir)
{
    dir.createDirectory();
    const auto wavFile = dir.getChildFile (juce::String (beat.spec.name) + ".wav");
    wavFile.deleteFile();

    juce::WavAudioFormat wav;
    auto stream = std::unique_ptr<juce::OutputStream> (wavFile.createOutputStream());
    auto options = juce::AudioFormatWriterOptions().withSampleRate (beat.spec.sampleRate)
                                                   .withNumChannels (2)
                                                   .withBitsPerSample (24);
    if (auto writer = wav.createWriterFor (stream, options))
        writer->writeFromAudioSampleBuffer (beat.audio, 0, beat.audio.getNumSamples());

    juce::String csv ("time_s,instrument,gm_note,velocity\n");
    for (const auto& h : beat.truth)
        csv << juce::String (h.time, 4) << "," << instrumentName (h.instrument) << ","
            << gmNote (h.instrument) << "," << juce::String (h.velocity, 2) << "\n";
    dir.getChildFile (juce::String (beat.spec.name) + ".csv").replaceWithText (csv);
}

void runAnalysis (const juce::File& writeDir)
{
    std::printf ("\n%-18s %6s %8s %6s  %5s %5s %5s %5s  %s\n",
                 "beat", "BPM", "found", "onsF", "kick", "snare", "hhC", "hhO", "timing");

    double sumF = 0.0;
    int bpmOk = 0;
    const auto specs = test::standardSet();

    for (const auto& spec : specs)
    {
        const auto beat = test::makeBeat (spec);
        if (writeDir != juce::File())
            writeBeat (beat, writeDir);

        const auto result = analyse (beat.audio, spec.sampleRate);
        const auto r = test::evaluate (beat.truth, spec.bpm, result);
        auto f = [&] (Instrument i) { return r.instruments[(size_t) i].f(); };

        std::printf ("%-18s %6.1f %8.2f %6.2f  %5.2f %5.2f %5.2f %5.2f  %.1f ms%s\n",
                     spec.name.c_str(), spec.bpm, r.bpmDetected, r.onsets.f(),
                     f (Instrument::Kick), f (Instrument::Snare),
                     f (Instrument::HiHatClosed), f (Instrument::HiHatOpen),
                     r.meanTimingErrorMs, r.bpmOctaveError ? "  (BPM x2 or /2)" : "");

        sumF += r.onsets.f();
        bpmOk += r.bpmCorrect ? 1 : 0;
    }

    std::printf ("\nmean onset F %.2f, BPM correct %d/%d   (F: 1.00 = every hit found, nothing extra)\n",
                 sumF / (double) specs.size(), bpmOk, (int) specs.size());
}
} // namespace

int main (int argc, char** argv)
{
    juce::File writeDir;
    for (int i = 1; i + 1 < argc; ++i)
        if (juce::String (argv[i]) == "--write")
            writeDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i + 1]);

    checkBasics();
    runAnalysis (writeDir);

    std::printf ("%s (%d failed checks)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
