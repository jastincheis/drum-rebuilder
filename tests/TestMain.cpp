// drb_tests: checks the building blocks, then runs the analyzer on the
// synthetic beats and prints how well it found the hits.
//
//   drb_tests                 run everything
//   drb_tests --write <dir>   also save each beat as WAV + its hits as CSV, to listen to

#include "Analyzer.h"
#include "AudioFile.h"
#include "Effects.h"
#include "Metronome.h"
#include "Midi.h"
#include "HitGains.h"
#include "Onsets.h"
#include "Rebuild.h"
#include "Samples.h"
#include "Player.h"
#include "Evaluation.h"
#include "SyntheticDrums.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
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
// Quantize and MIDI export.
void checkMidi()
{
    BeatGrid grid;
    grid.bpm = 120.0;          // beat 0.5 s, bar 2 s
    grid.firstDownbeat = 1.0;
    grid.firstBeat = 0.0;

    const std::vector<Hit> hits {
        { 0.51, Instrument::HiHatClosed, 0.5f, 1.0f, false },   // pickup, beat 4 of bar 0 (+10 ms)
        { 1.00, Instrument::Kick, 1.0f, 1.0f, false },
        { 1.02, Instrument::HiHatClosed, 0.5f, 1.0f, false },
        { 1.49, Instrument::Snare, 0.9f, 1.0f, false },         // beat 2, 10 ms early
        { 1.63, Instrument::HiHatClosed, 0.5f, 1.0f, false },   // just after the 1/16 at 1.625
    };

    const auto full = quantize (hits, grid, 0.25, 1.0);
    bool onGrid = true;
    for (const auto& h : full)
    {
        const double steps = (h.time - 1.0) / 0.125;
        onGrid = onGrid && std::abs (steps - std::round (steps)) < 1.0e-9;
    }
    check (onGrid && std::abs (full[3].time - 1.5) < 1.0e-9, "quantize 100 % puts every hit on the 1/16 grid");

    const auto half = quantize (hits, grid, 0.25, 0.5);
    check (std::abs (half[3].time - 1.495) < 1.0e-9, "quantize 50 % moves halfway");

    const auto midi = makeMidiFile (hits, grid, 960);
    // 1 conductor track + kick, snare, hat
    bool ok = midi.getNumTracks() == 4;
    double tempo = 0.0;
    std::map<int, std::vector<double>> notes;
    for (int t = 0; ok && t < midi.getNumTracks(); ++t)
        for (const auto* e : *midi.getTrack (t))
        {
            if (e->message.isTempoMetaEvent())
                tempo = 60.0 / e->message.getTempoSecondsPerQuarterNote();
            if (e->message.isNoteOn())
                notes[e->message.getNoteNumber()].push_back (e->message.getTimeStamp());
        }
    ok = ok && std::abs (tempo - 120.0) < 0.01;
    // The pickup hat makes the file start one bar before bar 1 (at 4 beats = 3840 ticks).
    ok = ok && notes[36].size() == 1 && std::abs (notes[36][0] - 3840.0) < 0.5;
    ok = ok && notes[38].size() == 1 && std::abs (notes[38][0] - (3840.0 + 0.98 * 960.0)) < 0.5;
    ok = ok && notes[42].size() == 3 && std::abs (notes[42][0] - 0.02 * 960.0 - 2880.0) < 0.5;
    check (ok, "MIDI file: tempo, one track per instrument, GM notes, times, pickup bar");
}

// Sample folders and rebuilding a beat.
void checkRebuild()
{
    auto spec = test::standardSet()[0];   // rock, 90 BPM
    const auto beat = test::makeBeat (spec);
    const double sr = spec.sampleRate;

    // Cuts like the plugin's automatic IN / OUT.
    RebuildInput input;
    input.audio = &beat.audio;
    input.sampleRate = sr;
    input.hits = beat.truth;
    for (size_t i = 0; i < input.hits.size(); ++i)
    {
        const double t = input.hits[i].time;
        double next = t + 0.4;
        for (size_t j = i + 1; j < input.hits.size(); ++j)
            if (input.hits[j].time > t + 1.0e-9)
            {
                next = std::min (next, input.hits[j].time - 0.002);
                break;
            }
        input.slices.emplace_back (t - 0.002, next);
    }

    // 1. Only originals, same tempo: the beat itself (where the cuts are).
    RebuildSettings settings;
    const auto same = renderRebuild (input, settings, sr, 0.0);
    double err = 0.0, ref = 0.0;
    for (size_t i = 0; i < input.hits.size(); ++i)
    {
        const int a = (int) (input.slices[i].first * sr) + (int) (0.003 * sr);
        const int b = (int) (input.slices[i].second * sr) - (int) (0.012 * sr);
        for (int k = a; k < b; ++k)
        {
            const float o = beat.audio.getSample (0, k), r = same.getSample (0, k);
            err += (double) (o - r) * (o - r);
            ref += (double) o * o;
        }
    }
    check (ref > 0.0 && err / ref < 1.0e-3, "rebuild from the originals = the beat (no doubled hits)");

    // 1b. A kick moved by hand (MIDI editor) takes its sound along.
    {
        RebuildInput moved;
        moved.audio = &beat.audio;
        moved.sampleRate = sr;
        for (size_t i = 0; i < input.hits.size(); ++i)
            if (input.hits[i].instrument == Instrument::Kick)
            {
                auto h = input.hits[i];
                h.source = h.time;
                h.time += 0.25;
                moved.hits.push_back (h);
                moved.slices.push_back (input.slices[i]);
                break;
            }
        const auto out = renderRebuild (moved, RebuildSettings(), sr, 0.0);
        const int to = (int) (moved.hits[0].time * sr), from = (int) (moved.hits[0].source * sr);
        const float atNew = out.getMagnitude (0, to, (int) (0.05 * sr));
        const float atOld = out.getMagnitude (0, from, (int) (0.05 * sr));
        const float original = beat.audio.getMagnitude (0, from, (int) (0.05 * sr));
        check (std::abs (atNew - original) < 0.02f * original && atOld < 0.01f * original,
               "a moved hit plays its own sound at its new time");
    }

    // 1c. A hit made softer by hand: its own sound plays softer.
    {
        RebuildInput one;
        one.audio = &beat.audio;
        one.sampleRate = sr;
        auto h = input.hits[0];
        h.soundVelocity = h.velocity;
        h.velocity *= 0.5f;
        one.hits.push_back (h);
        one.slices.push_back (input.slices[0]);
        const auto out = renderRebuild (one, RebuildSettings(), sr, 0.0);
        const int at = (int) (h.time * sr) + (int) (0.003 * sr);
        const float got = out.getMagnitude (0, at, (int) (0.03 * sr)), was = beat.audio.getMagnitude (0, at, (int) (0.03 * sr));
        check (std::abs (got / was - 0.5f) < 0.02f, "a hit made half as hard plays its sound half as loud");
    }

    // 2. A click for every instrument, twice the tempo: hits at half the time.
    auto click = std::make_shared<Sample>();
    click->audio.setSize (1, (int) (0.01 * sr));
    click->audio.clear();
    click->audio.setSample (0, 2, 1.0f);
    click->sampleRate = sr;
    click->peak = 1.0f;
    for (auto& plan : settings.instruments)
    {
        plan.mode = PickMode::Fixed;
        plan.pool = { click };
    }
    settings.tempoFactor = 2.0;
    const auto fast = renderRebuild (input, settings, sr, 0.0);
    bool onTime = true;
    for (const auto& h : input.hits)
    {
        const int at = (int) std::lround (h.time / 2.0 * sr);
        onTime = onTime && fast.getMagnitude (0, std::max (0, at - 8), 24) > 0.01f;
    }
    check (onTime && std::abs (fast.getNumSamples() / sr - beat.audio.getNumSamples() / sr / 2.0) < 0.01,
           "rebuild at twice the tempo: every hit at half the time");

    // 3. Keep the pitch: a 100 Hz kick comes out at the beat's kick pitch.
    auto tone = std::make_shared<Sample>();
    tone->audio.setSize (1, (int) (0.3 * sr));
    for (int i = 0; i < tone->audio.getNumSamples(); ++i)
        tone->audio.setSample (0, i, (float) (0.8 * std::sin (2.0 * juce::MathConstants<double>::pi * 100.0 * i / sr)
                                              * std::exp (-i / (0.15 * sr))));
    tone->sampleRate = sr;
    tone->peak = tone->audio.getMagnitude (0, tone->audio.getNumSamples());
    tone->pitch = estimatePitch (tone->audio, sr, Instrument::Kick);

    RebuildInput one = input;
    one.hits.clear();
    one.slices.clear();
    for (size_t i = 0; i < input.hits.size(); ++i)
        if (input.hits[i].instrument == Instrument::Kick && one.hits.empty())
        {
            one.hits.push_back (input.hits[i]);
            one.slices.push_back (input.slices[i]);
        }
    juce::AudioBuffer<float> kickOnly (beat.audio.getNumChannels(), beat.audio.getNumSamples());
    kickOnly.clear();
    const int ka = (int) (one.slices[0].first * sr), kb = (int) (one.slices[0].second * sr);
    for (int ch = 0; ch < kickOnly.getNumChannels(); ++ch)
        kickOnly.copyFrom (ch, ka, beat.audio, ch, ka, kb - ka);
    one.audio = &kickOnly;
    juce::AudioBuffer<float> kickSlice (1, kb - ka);
    kickSlice.copyFrom (0, 0, beat.audio, 0, ka, kb - ka);
    const float kickPitch = estimatePitch (kickSlice, sr, Instrument::Kick);

    RebuildSettings tuned;
    tuned.instruments[(size_t) Instrument::Kick] = { PickMode::Fixed, { tone }, 0 };
    tuned.keepPitch = true;
    const auto out = renderRebuild (one, tuned, sr, 0.5);
    const int at = (int) (one.hits[0].time * sr);
    juce::AudioBuffer<float> played (1, (int) (0.3 * sr));
    played.copyFrom (0, 0, out, 0, at, played.getNumSamples());
    const float got = estimatePitch (played, sr, Instrument::Kick);
    check (kickPitch > 0.0f && std::abs (got / kickPitch - 1.0f) < 0.06f, "keep pitch: the new kick is tuned to the beat's kick");

    // 3b. A length on the instrument (Drum Rack): the sound stops there,
    // for a sound from the folder and for the original alike.
    {
        RebuildSettings shortTone;
        shortTone.instruments[(size_t) Instrument::Kick] = { PickMode::Fixed, { tone }, 0, 0.1 };
        const auto cut = renderRebuild (one, shortTone, sr, 0.5);
        const auto whole = renderRebuild (one, RebuildSettings { [&] { std::array<InstrumentPlan, numInstruments> p {}; p[(size_t) Instrument::Kick] = { PickMode::Fixed, { tone }, 0 }; return p; }() }, sr, 0.5);
        const int t0 = (int) (one.hits[0].time * sr);
        const bool toneCut = cut.getMagnitude (0, t0 + (int) (0.11 * sr), (int) (0.1 * sr)) < 1.0e-4f
                             && whole.getMagnitude (0, t0 + (int) (0.11 * sr), (int) (0.1 * sr)) > 0.05f
                             && cut.getMagnitude (0, t0, (int) (0.05 * sr)) > 0.05f;

        RebuildSettings shortOriginal;
        shortOriginal.instruments[(size_t) Instrument::Kick].maxLength = 0.03;
        const auto cutOriginal = renderRebuild (one, shortOriginal, sr, 0.5);
        const int s0 = (int) (one.hits[0].sourceTime() * sr);
        const bool originalCut = cutOriginal.getMagnitude (0, s0 + (int) (0.031 * sr), kb - s0 - (int) (0.031 * sr)) < 1.0e-4f
                                 && cutOriginal.getMagnitude (0, s0, (int) (0.02 * sr)) > 0.01f;
        check (toneCut && originalCut, "a sound length cuts the sound there (folder sounds and originals)");

        // An IN: the sound from there on starts at the hit (the part before
        // is skipped), and still stops at its OUT.
        RebuildSettings late;
        late.instruments[(size_t) Instrument::Kick] = { PickMode::Fixed, { tone }, 0, 0.1, 0.04 };
        const auto skipped = renderRebuild (one, late, sr, 0.5);
        const bool startsAtHit = skipped.getMagnitude (0, t0 + (int) (0.005 * sr), (int) (0.03 * sr)) > 0.05f
                                 && skipped.getMagnitude (0, t0 + (int) (0.062 * sr), (int) (0.1 * sr)) < 1.0e-4f;
        RebuildSettings lateOriginal;
        lateOriginal.instruments[(size_t) Instrument::Kick].startAt = 0.01;
        const auto skippedOriginal = renderRebuild (one, lateOriginal, sr, 0.5);
        const auto whole0 = renderRebuild (one, RebuildSettings(), sr, 0.5);
        // Its sample 10 ms in now plays at the hit (past the 2 ms fade-in).
        const int k = s0 + (int) (0.01 * sr) + (int) (0.004 * sr);
        const bool originalShifted = std::abs (skippedOriginal.getSample (0, (int) (one.hits[0].time * sr) + (int) (0.004 * sr))
                                               - whole0.getSample (0, k)) < 0.02f;
        check (startsAtHit && originalShifted, "a sound's IN: from there on, at the hit (folder sounds and originals)");
    }

    // 4. Saving the originals into folders, and again (replaces them).
    juce::TemporaryFile tempDir;
    const auto root = tempDir.getFile();
    SampleLibrary library (root);
    std::vector<Cut> cuts;
    int kicks = 0;
    for (size_t i = 0; i < input.hits.size(); ++i)
    {
        cuts.push_back ({ input.hits[i].instrument, input.slices[i].first, input.slices[i].second });
        kicks += input.hits[i].instrument == Instrument::Kick ? 1 : 0;
    }
    library.saveOriginals ("Test beat", beat.audio, sr, cuts);
    const int firstCount = (int) library.samples (Instrument::Kick).size();
    library.saveOriginals ("Test beat", beat.audio, sr, cuts);
    const auto& saved = library.samples (Instrument::Kick);
    const bool ok = firstCount == kicks && (int) saved.size() == kicks && ! saved.empty() && saved[0]->original
                 && saved[0]->fromBeat == "Test beat"
                 && saved[0]->file.getFileName() == "Test beat (original) 01.wav";
    check (ok, "original hits saved into the instrument folders, saving again replaces them");

    // 5. The user's order: the last one put first, kept after reading the
    // folders again; a new sound comes after the ordered ones.
    {
        const auto names = [] (const SampleLibrary& lib)
        {
            juce::StringArray n;
            for (const auto& smp : lib.samples (Instrument::Kick))
                n.add (smp->file.getFileName());
            return n;
        };
        const int count = (int) library.samples (Instrument::Kick).size();
        const auto last = library.samples (Instrument::Kick).back()->file.getFileName();
        library.moveSample (Instrument::Kick, count - 1, 0);
        SampleLibrary again (root);
        again.rescan();
        juce::AudioBuffer<float> click (1, 2000);
        click.clear();
        click.setSample (0, 10, 0.9f);
        writeWav (again.folderFor (Instrument::Kick).getChildFile ("A new kick.wav"), click, sr);
        again.rescan();
        const auto order = names (again);
        check (count >= 2 && order[0] == last && order[order.size() - 1] == "A new kick.wav",
               "the sounds' order is kept, a new sound comes last");
    }
    root.deleteRecursively();
}

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
        auto f = [&] (Instrument i) { const auto& sc = r.instruments[(size_t) i]; return sc.truthCount() == 0 && sc.falsePositives == 0 ? 1.0 : sc.f(); };
        if (std::getenv ("DRB_DETAIL") != nullptr)
            for (auto i : { Instrument::Kick, Instrument::Snare, Instrument::HiHatClosed, Instrument::HiHatOpen })
                std::printf ("    %-14s tp %3d  fp %3d  fn %3d\n", instrumentName (i), r.instruments[(size_t) i].truePositives,
                             r.instruments[(size_t) i].falsePositives, r.instruments[(size_t) i].falseNegatives);

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

// --analyse <file>: what the analysis finds in a real recording.
int analyseFile (const juce::File& file)
{
    juce::String error;
    const auto audio = loadAudioFile (file, error);
    if (! audio)
    {
        std::printf ("%s\n", error.toRawUTF8());
        return 1;
    }

    const auto r = analyse (audio->audio, audio->sampleRate);
    std::printf ("%s\n%.1f BPM, %d bars, bar 1 at %.3f s, %d onsets, %d hits\n", file.getFileName().toRawUTF8(),
                 r.grid.bpm, r.grid.bars, r.grid.firstDownbeat, (int) r.onsets.size(), (int) r.hits.size());
    for (auto i : allInstruments)
    {
        int count = 0;
        for (const auto& h : r.hits)
            count += h.instrument == i ? 1 : 0;
        if (count > 0)
            std::printf ("  %-14s %d\n", instrumentName (i), count);
    }

    const double beat = r.grid.beatSeconds();
    for (const auto& h : r.hits)
        std::printf ("%8.3f  %6.2f  %-14s v %.2f  c %.2f\n", h.time,
                     beat > 0.0 ? (h.time - r.grid.firstDownbeat) / beat : 0.0,
                     instrumentName (h.instrument), h.velocity, h.confidence);
    return 0;
}

// One hit made softer in a recording where a kick (60 Hz) and a hi-hat
// (9 kHz) are struck 1 ms apart: only its own band comes down.
void checkHitGains()
{
    const double sr = 48000.0;
    juce::AudioBuffer<float> audio (1, (int) sr);
    for (int i = 0; i < audio.getNumSamples(); ++i)
    {
        const double t = i / sr;
        const float env = t >= 0.5 ? (float) std::exp (-(t - 0.5) * 8.0) : 0.0f;
        audio.setSample (0, i, env * (0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 60.0 * t)
                                      + 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 9000.0 * t)));
    }
    std::vector<Hit> hits { { 0.5, Instrument::Kick, 0.8f, 1.0f, true }, { 0.501, Instrument::HiHatClosed, 0.8f, 1.0f, true } };
    const auto bands = splitBands (audio, sr);
    check (! hasHitGains (hits), "no loudness changes = none to apply");

    auto band = [&] (const juce::AudioBuffer<float>& b, double hz)
    {
        // The level of one tone, 50 ms after the hits (Goertzel-like).
        double re = 0.0, im = 0.0;
        const int a = (int) (0.55 * sr), n = (int) (0.05 * sr);
        for (int i = a; i < a + n; ++i)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * hz * i / sr;
            re += b.getSample (0, i) * std::cos (w);
            im += b.getSample (0, i) * std::sin (w);
        }
        return std::sqrt (re * re + im * im);
    };
    const double kickBefore = band (audio, 60.0), hatBefore = band (audio, 9000.0);
    const float levelBefore = hitLevelDb (bands, hits, 0);
    hits[0].soundVelocity = hits[0].velocity;
    hits[0].velocity *= 0.5f;
    check (hasHitGains (hits), "a softer hit is a change");
    const auto out = applyHitGains (bands, hits);
    const double kick = band (out, 60.0) / kickBefore, hat = band (out, 9000.0) / hatBefore;
    check (std::abs (kick - 0.5) < 0.05 && std::abs (hat - 1.0) < 0.05,
           "softer kick played with a hi-hat: the kick comes down, the hi-hat stays");
    check (std::abs (hitLevelDb (bands, hits, 0) - (levelBefore - 6.02f)) < 0.2f, "the hit's level shows the change");

    // Solo: a kick + hi-hat at 0.5 s, the kick still ringing at a lone
    // hi-hat at 0.6 s.
    hits = { { 0.5, Instrument::Kick, 0.8f, 1.0f, true }, { 0.501, Instrument::HiHatClosed, 0.8f, 1.0f, true },
             { 0.6, Instrument::HiHatClosed, 0.8f, 1.0f, true } };
    InstrumentGains onlyKick {}, onlyHat {};
    onlyKick[(size_t) Instrument::Kick] = 1.0f;
    onlyHat[(size_t) Instrument::HiHatClosed] = 1.0f;
    const auto kickSolo = applyHitGains (bands, hits, &onlyKick), hatSolo = applyHitGains (bands, hits, &onlyHat);
    auto tone = [&] (const juce::AudioBuffer<float>& b, double hz, double from)
    {
        double re = 0.0, im = 0.0;
        for (int i = (int) (from * sr); i < (int) ((from + 0.04) * sr); ++i)
        {
            const double w = 2.0 * juce::MathConstants<double>::pi * hz * i / sr;
            re += b.getSample (0, i) * std::cos (w);
            im += b.getSample (0, i) * std::sin (w);
        }
        return std::sqrt (re * re + im * im);
    };
    const double kick1 = tone (audio, 60.0, 0.52), hat1 = tone (audio, 9000.0, 0.52);
    const double kick2 = tone (audio, 60.0, 0.65), hat2 = tone (audio, 9000.0, 0.65);
    // (The bands overlap a little at their edges: a few % leak through.)
    check (tone (kickSolo, 60.0, 0.52) > 0.9 * kick1 && tone (kickSolo, 9000.0, 0.52) < 0.1 * hat1
               && tone (kickSolo, 60.0, 0.65) > 0.9 * kick2 && tone (kickSolo, 9000.0, 0.65) < 0.1 * hat2,
           "solo kick: the kick (also ringing on), no hi-hats");
    check (tone (hatSolo, 9000.0, 0.52) > 0.9 * hat1 && tone (hatSolo, 60.0, 0.52) < 0.1 * kick1
               && tone (hatSolo, 9000.0, 0.65) > 0.9 * hat2 && tone (hatSolo, 60.0, 0.65) < 0.1 * kick2,
           "solo hi-hat: the hi-hats, no kick");
    // The same with each hit's IN / OUT (the kick's OUT where the lone
    // hi-hat starts): the kick still rings on under it.
    {
        const std::vector<juce::Range<double>> cuts { { 0.5, 0.598 }, { 0.501, 0.598 }, { 0.6, 0.9 } };
        const auto kickCut = applyHitGains (bands, hits, &onlyKick, &cuts);
        check (tone (kickCut, 60.0, 0.65) > 0.9 * kick2 && tone (kickCut, 9000.0, 0.65) < 0.1 * hat2,
               "solo kick with OUTs: the kick rings on under a hi-hat after it");
    }

    // A kick moved away by hand: its old place no longer sounds.
    hits = { { 0.7, Instrument::Kick, 0.8f, 1.0f, true } };
    hits[0].source = 0.5;
    check (hasHitGains (hits), "a moved hit is a change");
    const auto moved = applyHitGains (bands, hits);
    check (tone (moved, 60.0, 0.52) < 0.05 * kick1, "a moved kick is gone from where it was");

    // A kick (60 Hz) and a snare (1 kHz body, 9 kHz wires) struck together.
    juce::AudioBuffer<float> ks (1, (int) sr);
    for (int i = 0; i < ks.getNumSamples(); ++i)
    {
        const double t = i / sr, w = 2.0 * juce::MathConstants<double>::pi * t;
        const float env = t >= 0.5 ? (float) std::exp (-(t - 0.5) * 8.0) : 0.0f;
        ks.setSample (0, i, env * (0.3f * (float) std::sin (w * 60.0) + 0.3f * (float) std::sin (w * 1000.0)
                                   + 0.3f * (float) std::sin (w * 9000.0)));
    }
    const auto ksBands = splitBands (ks, sr);
    hits = { { 0.5, Instrument::Kick, 0.8f, 1.0f, true }, { 0.5, Instrument::Snare, 0.8f, 1.0f, true } };
    InstrumentGains onlySnare {};
    onlySnare[(size_t) Instrument::Snare] = 1.0f;
    const auto ksKick = applyHitGains (ksBands, hits, &onlyKick), ksSnare = applyHitGains (ksBands, hits, &onlySnare);
    check (tone (ksKick, 60.0, 0.52) > 0.9 * tone (ks, 60.0, 0.52) && tone (ksKick, 1000.0, 0.52) < 0.1 * tone (ks, 1000.0, 0.52)
               && tone (ksKick, 9000.0, 0.52) < 0.1 * tone (ks, 9000.0, 0.52),
           "solo kick on a kick + snare: no snare");
    check (tone (ksSnare, 60.0, 0.52) < 0.1 * tone (ks, 60.0, 0.52) && tone (ksSnare, 1000.0, 0.52) > 0.9 * tone (ks, 1000.0, 0.52)
               && tone (ksSnare, 9000.0, 0.52) > 0.9 * tone (ks, 9000.0, 0.52),
           "solo snare on a kick + snare: no kick");

    // A snare with a low body (100 Hz) long after a kick: the kick's lows
    // no longer ring there, so a kick solo leaves the snare out.
    juce::AudioBuffer<float> late (1, (int) sr);
    for (int i = 0; i < late.getNumSamples(); ++i)
    {
        const double t = i / sr, w = 2.0 * juce::MathConstants<double>::pi * t;
        const float kickEnv = t >= 0.1 ? (float) std::exp (-(t - 0.1) * 20.0) : 0.0f;
        const float snareEnv = t >= 0.7 ? (float) std::exp (-(t - 0.7) * 8.0) : 0.0f;
        late.setSample (0, i, kickEnv * 0.4f * (float) std::sin (w * 60.0)
                              + snareEnv * (0.4f * (float) std::sin (w * 100.0) + 0.3f * (float) std::sin (w * 1000.0)));
    }
    const auto lateBands = splitBands (late, sr);
    hits = { { 0.1, Instrument::Kick, 0.8f, 1.0f, true }, { 0.7, Instrument::Snare, 0.8f, 1.0f, true } };
    const auto lateKick = applyHitGains (lateBands, hits, &onlyKick);
    check (tone (lateKick, 100.0, 0.72) < 0.1 * tone (late, 100.0, 0.72),
           "solo kick: a snare's lows long after the kick are left out");

    // A kick at 0.1 s cut at 0.3 s and a sound without a marker at 0.4 s:
    // on kick solo it is not heard, the kick before its OUT is.
    juce::AudioBuffer<float> unmarked (1, (int) sr);
    for (int i = 0; i < unmarked.getNumSamples(); ++i)
    {
        const double t = i / sr, w = 2.0 * juce::MathConstants<double>::pi * t;
        const float kickEnv = t >= 0.1 ? (float) std::exp (-(t - 0.1) * 5.0) : 0.0f;
        const float otherEnv = t >= 0.4 ? (float) std::exp (-(t - 0.4) * 8.0) : 0.0f;
        unmarked.setSample (0, i, kickEnv * 0.3f * (float) std::sin (w * 60.0) + otherEnv * 0.4f * (float) std::sin (w * 1000.0));
    }
    const auto unmarkedBands = splitBands (unmarked, sr);
    hits = { { 0.1, Instrument::Kick, 0.8f, 1.0f, true }, { 0.9, Instrument::Snare, 0.8f, 1.0f, true } };
    const std::vector<juce::Range<double>> sounds { { 0.1, 0.3 }, { 0.9, 1.0 } };
    const auto cut = applyHitGains (unmarkedBands, hits, &onlyKick, &sounds);
    check (tone (cut, 60.0, 0.2) > 0.9 * tone (unmarked, 60.0, 0.2) && tone (cut, 1000.0, 0.45) < 0.01 * tone (unmarked, 1000.0, 0.45),
           "solo kick: the kick up to its OUT, nothing after it until the next hit");
}

// The Master: volume, a delay synced to the tempo, a reverb tail; off =
// untouched, and its settings survive being saved.
void checkMaster()
{
    const double sr = 48000.0;
    juce::AudioBuffer<float> click (2, (int) (2.0 * sr));
    click.clear();
    for (int ch = 0; ch < 2; ++ch)
        click.setSample (ch, (int) (0.1 * sr), 0.5f);

    auto copy = click;
    applyMaster (copy, sr, 120.0, MasterSettings {});
    check (std::abs (copy.getSample (0, (int) (0.1 * sr)) - 0.5f) < 1.0e-6f && copy.getMagnitude (0, (int) (0.2 * sr), (int) sr) < 1.0e-9f,
           "master off: the beat as it is");

    MasterSettings quieter;
    quieter.volumeDb = -6.0f;
    copy = click;
    applyMaster (copy, sr, 120.0, quieter);
    check (std::abs (copy.getSample (0, (int) (0.1 * sr)) - 0.25f) < 0.01f, "master volume -6 dB halves it");

    // 1/8 at 120 BPM = 0.25 s: the first echo 0.25 s after the click.
    MasterSettings echo;
    echo.delay = { true, 0.5f, 4 };
    copy = click;
    applyMaster (copy, sr, 120.0, echo);
    const int at = (int) (0.35 * sr);
    check (copy.getMagnitude (0, at - 200, 400) > 0.02f && copy.getMagnitude (0, (int) (0.2 * sr), (int) (0.1 * sr)) < 1.0e-6f,
           "delay 1/8 at 120 BPM: an echo 0.25 s later, nothing before it");

    MasterSettings hall;
    hall.reverb = { true, 0.5f, 0.8f };
    copy = click;
    applyMaster (copy, sr, 120.0, hall);
    check (copy.getMagnitude (0, (int) (0.5 * sr), (int) (0.2 * sr)) > 1.0e-4f, "reverb: the click rings on");

    echo.reverb = { false, 0.7f, 0.3f };   // off, settings kept
    const auto back = MasterSettings::fromString (echo.toString());
    check (back.delay.on && back.delay.length == 4 && std::abs (back.delay.amount - 0.5f) < 1.0e-3f && ! back.reverb.on
               && std::abs (back.reverb.amount - 0.7f) < 1.0e-3f && std::abs (back.reverb.length - 0.3f) < 1.0e-3f,
           "master settings saved and read back (an effect off keeps its settings)");
}

// A marker put by hand lands on the start of the sound near it.
void checkFindAttack()
{
    const double sr = 48000.0;
    juce::AudioBuffer<float> audio (1, (int) sr);
    audio.clear();
    auto burst = [&] (double at)
    {
        for (int i = (int) (at * sr); i < (int) ((at + 0.1) * sr); ++i)
            audio.setSample (0, i, 0.8f * (float) std::exp (-(i / sr - at) * 30.0) * (float) std::sin (i * 0.05));
    };
    burst (0.0);
    burst (0.5);
    const double atStart = findAttack (audio, sr, 0.02, 0.04), atMiddle = findAttack (audio, sr, 0.53, 0.04);
    check (atStart >= 0.0 && atStart < 0.003, "a sound at the very start of the file is found");
    check (std::abs (atMiddle - 0.499) < 0.002, "a marker 30 ms late snaps to the sound's start");
    check (findAttack (audio, sr, 0.3, 0.04) < 0.0, "no sound there = nothing found");
}

int main (int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (juce::String (argv[i]) == "--analyse")
            return analyseFile (juce::File::getCurrentWorkingDirectory().getChildFile (argv[i + 1]));

    juce::File writeDir;
    for (int i = 1; i + 1 < argc; ++i)
        if (juce::String (argv[i]) == "--write")
            writeDir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i + 1]);

    checkBasics();
    checkPlayer();
    checkMetronome();
    checkMidi();
    checkRebuild();
    checkHitGains();
    checkFindAttack();
    checkMaster();
    runAnalysis (writeDir);

    std::printf ("%s (%d failed checks)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
