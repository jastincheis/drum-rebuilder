#include "Midi.h"

#include <algorithm>
#include <cmath>

namespace drb
{

const std::vector<GridDivision>& gridDivisions()
{
    static const std::vector<GridDivision> divisions {
        { "1/4", 1.0 }, { "1/8", 0.5 }, { "1/16", 0.25 }, { "1/32", 0.125 },
        { "1/8T", 1.0 / 3.0 }, { "1/16T", 1.0 / 6.0 },
    };
    return divisions;
}

double snapToGrid (double seconds, const BeatGrid& grid, double divisionBeats)
{
    const double beat = grid.beatSeconds();
    if (beat <= 0.0 || divisionBeats <= 0.0)
        return seconds;
    const double steps = std::round ((seconds - grid.firstDownbeat) / beat / divisionBeats);
    return std::max (0.0, grid.firstDownbeat + steps * divisionBeats * beat);
}

std::vector<Hit> quantize (std::vector<Hit> hits, const BeatGrid& grid, double divisionBeats, double strength)
{
    strength = std::clamp (strength, 0.0, 1.0);
    for (auto& h : hits)
        h.time += (snapToGrid (h.time, grid, divisionBeats) - h.time) * strength;
    std::stable_sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.time < b.time; });
    return hits;
}

juce::MidiFile makeMidiFile (const std::vector<Hit>& hits, const BeatGrid& grid, int ticksPerQuarter)
{
    juce::MidiFile file;
    file.setTicksPerQuarterNote (ticksPerQuarter);

    const double bpm = grid.bpm > 0.0 ? grid.bpm : 120.0;
    const double beat = 60.0 / bpm;
    const int perBar = grid.timeSignature.beatsPerBar;
    const double bar = perBar * beat;

    // Time 0 of the file: bar 1, or earlier bars when hits come before it.
    double origin = grid.bpm > 0.0 ? grid.firstDownbeat : (hits.empty() ? 0.0 : hits.front().time);
    if (! hits.empty() && hits.front().time < origin - 1.0e-6)
        origin -= std::ceil ((origin - hits.front().time - 1.0e-6) / bar) * bar;

    juce::MidiMessageSequence conductor;
    conductor.addEvent (juce::MidiMessage::textMetaEvent (3, "Drum Rebuilder"), 0.0);
    conductor.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::lround (60000000.0 / bpm)), 0.0);
    conductor.addEvent (juce::MidiMessage::timeSignatureMetaEvent (perBar, grid.timeSignature.beatUnit), 0.0);
    file.addTrack (conductor);

    const double noteTicks = ticksPerQuarter / 4.0;   // a sixteenth: drums only care about the start
    for (auto inst : allInstruments)
    {
        juce::MidiMessageSequence track;
        for (const auto& h : hits)
        {
            if (h.instrument != inst)
                continue;
            const double tick = std::max (0.0, std::round ((h.time - origin) / beat * ticksPerQuarter));
            const auto velocity = (juce::uint8) std::clamp ((int) std::lround (1.0 + h.velocity * 126.0), 1, 127);
            track.addEvent (juce::MidiMessage::noteOn (10, gmNote (inst), velocity), tick);
            track.addEvent (juce::MidiMessage::noteOff (10, gmNote (inst)), tick + noteTicks);
        }
        if (track.getNumEvents() == 0)
            continue;

        track.addEvent (juce::MidiMessage::textMetaEvent (3, instrumentName (inst)), 0.0);
        track.sort();
        track.updateMatchedPairs();
        file.addTrack (track);
    }
    return file;
}

bool writeMidiFile (const juce::File& target, const std::vector<Hit>& hits, const BeatGrid& grid)
{
    const auto midi = makeMidiFile (hits, grid);
    juce::TemporaryFile temp (target);
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! midi.writeTo (out, 1))
            return false;
    }
    return temp.overwriteTargetFileWithTemporary();
}

} // namespace drb
