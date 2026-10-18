#pragma once

#include "Analyzer.h"
#include "AudioFile.h"
#include "Midi.h"
#include "Rebuild.h"
#include "Samples.h"
#include "Metronome.h"
#include "Player.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>

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

    // Manual fixes to the hits. A "hit" here is one onset (one marker)
    // with the instruments played at it.
    double moveOnset (int index, double seconds);   // kept between its neighbours; returns where it ended up
    std::vector<drb::Instrument> getInstrumentsAt (int index) const;
    void toggleInstrument (int index, drb::Instrument);   // the last one off = the hit is deleted
    void deleteHit (int index);
    void replaceInstrument (int index, drb::Instrument from, drb::Instrument to);
    // What else the sound at this hit could be: the instrument other than
    // `current` it sounds most like, if the analysis has an idea.
    std::optional<drb::Instrument> suggestInstead (int index, drb::Instrument current) const;
    int addHit (double seconds, drb::Instrument);          // returns its index
    bool areHitsEdited() const { return onsetsEdited; }

    // The same, one instrument hit at a time (index into getAnalysis().hits),
    // for the MIDI editor.
    int moveHitTo (int hitIndex, double seconds, drb::Instrument);   // returns its new index
    void deleteHitAt (int hitIndex);
    int addHitAt (double seconds, drb::Instrument, float velocity);  // returns its index
    void setHitVelocity (int hitIndex, float velocity);

    // IN / OUT of one instrument hit: where its sound starts and ends.
    // Automatic until set: from the hit to the next hit of anything (at most
    // 0.4 s). Drags call beginHitEdit() first.
    std::pair<double, double> getSlice (int hitIndex) const;
    void setSlice (int hitIndex, double in, double out);
    int findHit (double time, drb::Instrument) const;   // -1 = none
    void quantizeHits (double divisionBeats, double strength);

    // Undo / redo for every change to the hits. A drag is one step: call
    // beginHitEdit() when it starts.
    void beginHitEdit();
    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }
    void undo();
    void redo();

    drb::Player& getPlayer() { return player; }

    // The playhead on the recording's timeline (also while the rebuilt beat
    // plays at another tempo): what the views show and set.
    double getPlayheadSeconds() const { return player.getPositionSeconds() * timelineFactor(); }
    void setPlayheadSeconds (double t) { player.setPositionSeconds (t / timelineFactor()); }

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
    void previewSample (const drb::Sample&);   // a library sound, once
    drb::Player& getPreviewPlayer() { return previewPlayer; }
    const drb::LoadedAudio* getPreview() const { return preview.get(); }   // null while loading
    const juce::File& getPreviewFile() const { return previewTarget; }
    const juce::String& getPreviewError() const { return previewError; }

    // Solo on the Instruments tab: play only the hits of the solo
    // instruments, cut from the recording at their own times. It plays from
    // the main playhead, and when it stops the playhead stays there.
    void setSolo (drb::Instrument, bool on);
    bool isSolo (drb::Instrument i) const { return solo[(size_t) i]; }
    bool anySolo() const;
    std::optional<drb::Instrument> firstSolo() const;
    std::vector<drb::Instrument> getSolo() const;
    void playSolo();
    void pauseSolo();
    bool isSoloPlaying() const { return auditionPlayer.isPlaying(); }
    drb::Player& getAuditionPlayer() { return auditionPlayer; }

    // --- Rebuilding the beat with other sounds (stage 3) ---

    drb::SampleLibrary& getLibrary() { return library; }
    void rescanLibrary();
    juce::String getBeatName() const;   // the recording's file name, for its originals
    // Cuts every hit of this beat between its IN and OUT into the
    // instrument folders. Returns how many sounds were written.
    int saveOriginals();
    // The sounds an instrument can be rebuilt with: its folder, without
    // this beat's own originals.
    std::vector<drb::SamplePtr> getPool (drb::Instrument) const;

    drb::PickMode getPickMode (drb::Instrument i) const { return pickModes[(size_t) i]; }
    void setPickMode (drb::Instrument, drb::PickMode);
    drb::SamplePtr getFixedSample (drb::Instrument) const;   // null = none
    void setFixedSample (drb::Instrument, const juce::File&);
    void nextSounds();   // other sounds, at random, for every instrument not on Original

    bool getKeepPitch() const { return keepPitch; }
    void setKeepPitch (bool);
    double getRebuildBpm() const;   // the rebuilt beat's tempo
    void setRebuildBpm (double);
    double getTempoFactor() const { return tempoFactor; }

    // A/B: what the transport plays, the recording or the rebuilt beat.
    bool isListeningRebuilt() const { return listenRebuilt; }
    void setListeningRebuilt (bool);
    bool isRendering() const { return rendering; }

    // The rebuilt beat as a stereo 24-bit WAV at the recording's sample
    // rate, with the last sounds' tails. Done in the background.
    void exportRebuiltWav (const juce::File&, std::function<void (bool ok)> done);

    // Instrument checks (0 = not yet, 1 = OK, 2 = wrong), kept with the state.
    int getCheck (drb::Instrument i) const { return checks[(size_t) i]; }
    void setCheck (drb::Instrument, int);

    // The folder shown in the Import page browser, kept with the state.
    juce::File getBrowseFolder() const { return browseFolder; }
    void setBrowseFolder (const juce::File& f) { browseFolder = f; }

private:
    drb::Player player, previewPlayer, auditionPlayer;
    std::array<bool, drb::numInstruments> solo {};

    drb::SampleLibrary library { drb::SampleLibrary::defaultRoot() };
    std::array<drb::PickMode, drb::numInstruments> pickModes {};
    std::array<juce::File, drb::numInstruments> fixedFiles;
    bool keepPitch = false, listenRebuilt = false, rendering = false;
    double tempoFactor = 1.0;
    juce::uint32 seed = 1;
    int renderGeneration = 0;
    std::unique_ptr<juce::AudioBuffer<float>> originalAtDevice, rebuiltAtDevice;
    juce::ValueTree pendingRebuild;   // from the saved state, until that file is loaded

    int memoryGeneration = 0;
    juce::ValueTree makeBeatState() const;
    void readBeatState (const juce::ValueTree&);
    static juce::File beatMemoryFile (const juce::File& audio);
    void saveBeatMemory();
    void scheduleBeatMemory();

    drb::RebuildInput makeRebuildInput() const;
    drb::RebuildSettings makeRebuildSettings() const;
    void chooseDefaultSounds();
    void requestRender();
    void showSource (double previousFactor);   // puts the original or the rebuilt beat in the player
    double timelineFactor() const { return listenRebuilt && rebuiltAtDevice != nullptr ? tempoFactor : 1.0; }
    void applyLoop();
    int soloRebuild = 0;
    std::array<int, drb::numInstruments> checks {};
    std::array<int, drb::numInstruments> pendingChecks {};
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
    std::vector<drb::Hit> pendingHits;
    juce::String previewError;
    int previewGeneration = 0;

    bool onsetsEdited = false;

    std::vector<std::vector<drb::Hit>> undoStack, redoStack;

    void hitsChanged();   // after a manual edit
    void rebuildOnsets(); // one onset per distinct hit time

    void installPlayback();
    void buildSoloAudio();   // the solo instruments' hits, at the device rate
    void updateMetronomeGrid();
    void placeDownbeat (double seconds);

    JUCE_DECLARE_WEAK_REFERENCEABLE (DrumRebuilderProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumRebuilderProcessor)
};
