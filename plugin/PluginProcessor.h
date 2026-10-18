#pragma once

#include "Analyzer.h"
#include "AudioFile.h"
#include "Metronome.h"
#include "Player.h"

#include <juce_audio_processors/juce_audio_processors.h>

class DrumRebuilderProcessor : public juce::AudioProcessor,
                               public juce::ChangeBroadcaster
{
public:
    DrumRebuilderProcessor();
    ~DrumRebuilderProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    using AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int size) override;

    // --- Session (message thread). Every change is announced through the
    // ChangeBroadcaster.

    // Loads and analyses in the background.
    void loadFile (const juce::File&);
    bool isBusy() const { return busy; }
    const juce::String& getStatus() const { return status; }

    const drb::LoadedAudio* getLoaded() const { return loaded.get(); }
    int getLoadedId() const { return loadedId; }   // changes with every newly loaded recording
    const drb::AnalysisResult& getAnalysis() const { return analysis; }

    // Manual tempo fix: x2 or x0.5 (keeps the bar 1 position).
    void scaleTempo (double factor);
    bool isTempoManual() const { return tempoManual; }

    // Manual fix: bar 1 (where the beat starts) is at this time. With
    // `snapToHit` it moves to the hit right there when one is within 40 ms,
    // so a click near the hit is enough. Returns where bar 1 ended up.
    double setFirstDownbeat (double seconds, bool snapToHit = true);

    // Manual fix: a detected hit moved in time (kept between its
    // neighbours). Returns where it ended up.
    double moveOnset (int index, double seconds);

    drb::Player& getPlayer() { return player; }

    juce::Range<double> getRegion() const { return region; }
    void setRegion (juce::Range<double>);   // empty = none
    bool isLooping() const { return player.isLooping(); }
    void setLooping (bool);

    // Clicks on the detected beats while the recording plays.
    bool isMetronomeOn() const { return metronome.isEnabled(); }
    void setMetronome (bool);

    // Listening to a file before opening it (Import page). It plays on its
    // own player; starting one stops the other.
    void previewFile (const juce::File&);   // the same file again = from the start
    void stopPreview() { previewPlayer.stop(); }
    drb::Player& getPreviewPlayer() { return previewPlayer; }
    const drb::LoadedAudio* getPreview() const { return preview.get(); }   // null while loading
    const juce::File& getPreviewFile() const { return previewTarget; }
    const juce::String& getPreviewError() const { return previewError; }

    // The folder shown in the Import page browser, kept with the state.
    juce::File getBrowseFolder() const { return browseFolder; }
    void setBrowseFolder (const juce::File& f) { browseFolder = f; }

private:
    drb::Player player, previewPlayer;
    drb::Metronome metronome;
    std::vector<juce::int64> positions;   // audio thread: what the player played, per sample
    std::unique_ptr<drb::LoadedAudio> loaded, preview;
    drb::AnalysisResult analysis;
    bool tempoManual = false;
    juce::Range<double> region;

    bool busy = false;
    juce::String status;
    int loadGeneration = 0;   // a newer load makes older results stale
    int loadedId = 0;
    double preparedRate = 0.0;

    juce::File previewTarget, browseFolder;

    // A manual tempo / bar 1 from the saved state, put back once that file
    // is analysed again.
    juce::File pendingFile;
    double pendingBpm = 0.0, pendingDownbeat = 0.0;
    std::vector<double> pendingOnsets;
    juce::String previewError;
    int previewGeneration = 0;

    bool onsetsEdited = false;

    void installPlayback();
    void updateMetronomeGrid();
    void placeDownbeat (double seconds);

    JUCE_DECLARE_WEAK_REFERENCEABLE (DrumRebuilderProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumRebuilderProcessor)
};
