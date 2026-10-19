#pragma once

#include "Model.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>
#include <vector>

// The sample library: one folder per instrument ("Kick", "Snare", ...) under
// a root folder. Each folder holds the user's own sounds and the original
// hits cut from beats, named "<beat> (original) 01.wav".
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
};

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
    juce::File folderFor (Instrument) const;

    // Reads every folder again (loads the sounds into memory).
    void rescan();
    const std::vector<SamplePtr>& samples (Instrument i) const { return lists[(size_t) i]; }

    // Writes the cuts of one beat as "<beat> (original) NN.wav" in each
    // instrument's folder, after deleting that beat's earlier originals (so
    // saving again after fixing things replaces them). Fades of 2 ms in and
    // 10 ms out. Returns how many files were written; rescans.
    int saveOriginals (const juce::String& beatName, const juce::AudioBuffer<float>& audio, double sampleRate,
                       const std::vector<Cut>&);

    // Copies sound files into an instrument's folder (keeps the names,
    // adds " 2", " 3" ... on a clash). Returns how many were copied; rescans.
    int addFiles (Instrument, const juce::Array<juce::File>&);

    // The user's order of an instrument's sounds: moves one from position
    // `from` to `to` and remembers it (order.txt in the folder; sounds not
    // in it, e.g. new ones, come after, by name).
    void moveSample (Instrument, int from, int to);

    static juce::String originalsTag() { return " (original) "; }
    static juce::String orderFileName() { return "order.txt"; }

private:
    juce::File root;
    void saveOrder (Instrument) const;
    std::array<std::vector<SamplePtr>, numInstruments> lists;
};

} // namespace drb
