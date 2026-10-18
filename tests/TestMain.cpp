// drb_tests: checks the building blocks, then runs the analyzer on the
// synthetic beats and prints how well it found the hits.
//
//   drb_tests                 run everything
//   drb_tests --write <dir>   also save each beat as WAV + its hits as CSV, to listen to

#include "Analyzer.h"
#include "Metronome.h"
#include "Player.h"
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

test::Truth truthOf (const test::Beat& beat)
{
    return { beat.truth, beat.spec.bpm, beat.spec.bars, beat.firstDownbeat };
}

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
        const auto truth = truthOf (beat);
        AnalysisResult perfect;
        perfect.grid.bpm = spec.bpm;
        perfect.grid.bars = spec.bars;
        perfect.grid.firstDownbeat = beat.firstDownbeat;
        perfect.hits = beat.truth;
        const auto r = test::evaluate (truth, perfect);
        check (r.onsets.f() > 0.9999 && r.bpmCorrect && r.barsCorrect, "scorer gives 1.0 to the truth");

        AnalysisResult doubled = perfect;
        doubled.grid.bpm = spec.bpm * 2.0;
        check (test::evaluate (truth, doubled).bpmOctaveError, "scorer spots double tempo");
    }
}

// The player, driven like the audio thread would: blocks of 512 at 48 kHz.
// The metronome clicks exactly on the beats the player passes.
void checkMetronome()
{
    constexpr double rate = 48000.0;
    constexpr int block = 480;
    drb::Player p;
    drb::Metronome m;
    p.prepare (rate);
    m.prepare (rate);
    m.setGrid (120.0, 0.1, 4);   // beats at 0.1, 0.6, 1.1 ...
    m.setEnabled (true);
    auto silence = std::make_unique<juce::AudioBuffer<float>> (1, (int) rate * 3);
    silence->clear();
    p.setSource (std::move (silence));
    p.play();

    // Record 2.2 s, then find where each click starts.
    const int total = (int) (2.2 * rate / block) * block;
    juce::AudioBuffer<float> out (2, total);
    out.clear();
    std::vector<juce::int64> positions (block);
    for (int start = 0; start < total; start += block)
    {
        p.render (out, start, block, positions.data());
        m.render (out, start, block, positions.data());
    }

    std::vector<int> clicks;
    for (int i = 0; i < total; ++i)
        if (std::abs (out.getSample (0, i)) > 0.0f && (clicks.empty() || i - clicks.back() > (int) (0.2 * rate)))
            clicks.push_back (i);

    bool onTime = clicks.size() == 5;
    for (size_t k = 0; onTime && k < clicks.size(); ++k)
        onTime = std::abs (clicks[k] / rate - (0.1 + 0.5 * (double) k)) < 0.0005;
    check (onTime, "metronome clicks on every beat, on time");
    if (! onTime)
        for (auto c : clicks)
            std::printf ("   click at %.4f s\n", c / rate);

    const auto peak = [&] (int k) { return out.getMagnitude (0, clicks[(size_t) k], (int) (0.03 * rate)); };
    check (clicks.size() == 5 && peak (0) > peak (1) && peak (4) > peak (3), "metronome: bar start louder");

    out.setSize (2, block);
    out.clear();
    m.setEnabled (false);
    p.render (out, 0, block, positions.data());
    m.render (out, 0, block, positions.data());
    check (out.getMagnitude (0, block) <= 1.0e-3f, "metronome off = silent");
}

void checkPlayer()
{
    constexpr double rate = 48000.0;
    constexpr int block = 512;
    drb::Player p;
    p.prepare (rate);

    auto src = std::make_unique<juce::AudioBuffer<float>> (1, (int) rate * 2);   // 2 s ramp 0..1
    for (int i = 0; i < src->getNumSamples(); ++i)
        src->setSample (0, i, (float) i / (float) src->getNumSamples());
    p.setSource (std::move (src));

    juce::AudioBuffer<float> out (2, block);
    auto run = [&] (double seconds)
    {
        float peak = 0.0f;
        for (int n = 0; n < (int) (seconds * rate / block); ++n)
        {
            out.clear();
            p.render (out, 0, block);
            peak = std::max (peak, out.getMagnitude (0, block));
        }
        return peak;
    };

    check (run (0.1) <= 0.0f, "player silent before play");
    p.play();
    check (p.isPlaying() && run (0.5) > 0.0f, "player makes sound after play");
    check (std::abs (p.getPositionSeconds() - 0.5) < 0.02, "player position advances");

    p.pause();
    run (0.1);
    const double paused = p.getPositionSeconds();
    check (! p.isPlaying() && run (0.2) <= 0.0f, "player silent after pause");
    check (std::abs (p.getPositionSeconds() - paused) < 1.0e-9, "pause keeps the position");

    p.play();
    run (0.1);
    p.stop();
    run (0.1);
    check (p.getPositionSeconds() < 1.0e-9, "stop goes back to the start");

    p.setPositionSeconds (1.5);
    p.play();
    run (0.2);
    check (std::abs (p.getPositionSeconds() - 1.7) < 0.02, "jump while playing is kept");

    run (0.5);   // runs past the 2 s end
    check (! p.isPlaying() && p.getPositionSeconds() < 1.0e-9, "end of file stops and rewinds");

    p.setLoop (true, { 0.5, 1.0 });
    p.setPositionSeconds (0.5);
    p.play();
    run (1.2);   // 2.4 loops
    const double pos = p.getPositionSeconds();
    check (p.isPlaying() && pos >= 0.5 && pos <= 1.0, "loop stays inside the region");
    p.stop();
    run (0.1);
    check (std::abs (p.getPositionSeconds() - 0.5) < 1.0e-3, "stop in a loop goes to the loop start");
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
    std::printf ("\n%-18s %6s %8s %5s %8s  %6s %7s  %5s %5s %5s %5s\n",
                 "beat", "BPM", "found", "bars", "bar 1", "onsF", "timing", "kick", "snare", "hhC", "hhO");

    double sumF = 0.0;
    int bpmOk = 0, barsOk = 0;
    const auto specs = test::standardSet();

    for (const auto& spec : specs)
    {
        const auto beat = test::makeBeat (spec);
        if (writeDir != juce::File())
            writeBeat (beat, writeDir);

        const auto result = analyse (beat.audio, spec.sampleRate);
        const auto r = test::evaluate (truthOf (beat), result);
        auto f = [&] (Instrument i) { return r.instruments[(size_t) i].f(); };

        std::printf ("%-18s %6.1f %8.2f %2d%-3s %5.1fms  %6.2f %5.1fms  %5.2f %5.2f %5.2f %5.2f%s\n",
                     spec.name.c_str(), spec.bpm, r.bpmDetected, result.grid.bars, r.barsCorrect ? "" : " X",
                     r.downbeatErrorMs, r.onsets.f(), r.meanTimingErrorMs,
                     f (Instrument::Kick), f (Instrument::Snare),
                     f (Instrument::HiHatClosed), f (Instrument::HiHatOpen),
                     r.bpmOctaveError ? "  (BPM x2 or /2)" : "");

        sumF += r.onsets.f();
        bpmOk += r.bpmCorrect ? 1 : 0;
        barsOk += r.barsCorrect && r.downbeatErrorMs < 30.0 ? 1 : 0;
    }

    std::printf ("\nmean onset F %.2f, BPM correct %d/%d, bars + bar 1 correct %d/%d"
                 "   (F: 1.00 = every hit found, nothing extra)\n",
                 sumF / (double) specs.size(), bpmOk, (int) specs.size(), barsOk, (int) specs.size());
}
} // namespace

int main (int argc, char** argv)
{
    juce::File writeDir;
    for (int i = 1; i + 1 < argc; ++i)
        if (juce::String (argv[i]) == "--write")
            writeDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i + 1]);

    checkBasics();
    checkPlayer();
    checkMetronome();
    runAnalysis (writeDir);

    std::printf ("%s (%d failed checks)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
