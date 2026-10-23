#pragma once

#include "Model.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <functional>
#include <memory>
#include <vector>

// The sample library: one folder per instrument ("Kick", "Snare", ...) under
// a root folder, plus "Unsorted" and "Loops" for imported sounds we couldn't
// put anywhere. Each folder holds the user's own sounds, the original hits
// cut from beats, named "<beat> (original) 01.wav", and links to sounds that
// stay where they are (links.txt: one full path per line, from "Import a
// folder").
namespace drb
{

struct Sample
{
    juce::File file;
    juce::AudioBuffer<float> audio;   // 1 or 2 channels
    double sampleRate = 44100.0;
    bool original = false;            // cut from a beat by Drum Rebuilder
    juce::String fromBeat;            // the beat's name, for originals
    float peak = 0.0f;                // loudest sample value
    float pitch = 0.0f;               // Hz, see estimatePitch()
    bool linked = false;              // a link: the file is elsewhere, the user's own
    double seconds = 0.0;             // length of the file (Loops: audio isn't loaded)
    juce::Time modified;              // to reuse it when the folders are read again
    juce::int64 bytes = 0;
};

// The library's folders: the instruments by their number, then these two.
inline constexpr int unsortedFolder = numInstruments, loopsFolder = numInstruments + 1, numFolders = numInstruments + 2;
juce::String folderName (int folder);   // "Kick" ... "Unsorted", "Loops"

using SamplePtr = std::shared_ptr<const Sample>;

// How high a drum sound is: the strongest low partial for kick, snare, toms
// (Hz), the spectral centroid for hi-hats and cymbals. Only ratios between
// sounds of the same instrument matter. 0 = unknown.
float estimatePitch (const juce::AudioBuffer<float>&, double sampleRate, Instrument);

// One hit to cut out of a recording, between its IN and OUT (seconds).
struct Cut
{
    Instrument instrument;
    double in = 0.0, out = 0.0;
};

class SampleLibrary
{
public:
    explicit SampleLibrary (const juce::File& root);

    static juce::File defaultRoot();   // ~/Music/Drum Rebuilder/Samples

    juce::File getRoot() const { return root; }
    void setRoot (const juce::File&);
    juce::File folderFor (Instrument i) const { return folderAt ((int) i); }
    juce::File folderAt (int folder) const;

    // Reads every folder again (loads the sounds into memory; the Loops
    // only by their length, they play from the file). Sounds that didn't
    // change are kept from the last time.
    void rescan();
    const std::vector<SamplePtr>& samples (Instrument i) const { return lists[(size_t) i]; }
    const std::vector<SamplePtr>& inFolder (int folder) const { return lists[(size_t) folder]; }
    int missingLinks() const { return missing; }   // linked files not found at the last rescan

    // Writes the cuts of one beat as "<beat> (original) NN.wav" in each
    // instrument's folder, after deleting that beat's earlier originals (so
    // saving again after fixing things replaces them). Fades of 2 ms in and
    // 10 ms out. Returns how many files were written; rescans.
    int saveOriginals (const juce::String& beatName, const juce::AudioBuffer<float>& audio, double sampleRate,
                       const std::vector<Cut>&);

    // Copies sound files into an instrument's folder (keeps the names,
    // adds " 2", " 3" ... on a clash). Returns how many were copied; rescans.
    int addFiles (Instrument i, const juce::Array<juce::File>& files) { return addFiles ((int) i, files); }
    int addFiles (int folder, const juce::Array<juce::File>&);

    // The user's order of an instrument's sounds: moves one from position
    // `from` to `to` and remembers it (order.txt in the folder; sounds not
    // in it, e.g. new ones, come after, by name).
    void moveSample (Instrument i, int from, int to) { moveSample ((int) i, from, to); }
    void moveSample (int folder, int from, int to);

    // Out of the library: a link is forgotten (the file stays where it is),
    // a sound of the library's own goes to the trash. Rescans.
    bool remove (int folder, const SamplePtr&);

    // Into another folder (a link moves, an own file is moved). Rescans.
    bool moveToFolder (int from, const SamplePtr&, int to);

    // "Import a folder": every sound file in `dir` and below gets a link in
    // the folder it belongs in (by its name, else by its sound; loops in
    // Loops, unclear ones in Unsorted), see sortSound(). Files already in
    // the library are skipped. Only reads and writes the disk (safe on a
    // background thread); `progress` (done, total) returns false to stop.
    // Call rescan() afterwards.
    struct ImportSummary
    {
        std::array<int, numFolders> added {};
        int found = 0, skipped = 0;
        bool stopped = false;
        int total() const;
    };
    ImportSummary importFolder (const juce::File& dir, const std::function<bool (int, int)>& progress) const;

    static juce::String originalsTag() { return " (original) "; }
    static juce::String orderFileName() { return "order.txt"; }
    static juce::String linksFileName() { return "links.txt"; }

private:
    juce::File root;
    void saveOrder (int folder) const;
    juce::StringArray readLinks (int folder) const;
    bool writeLinks (int folder, const juce::StringArray&) const;
    std::array<std::vector<SamplePtr>, numFolders> lists;
    int missing = 0;
};

} // namespace drb
