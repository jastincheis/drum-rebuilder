#pragma once

#include "Analyzer.h"
#include "HitGains.h"
#include "AudioFile.h"
#include "Effects.h"
#include "Midi.h"
#include "Rebuild.h"
#include "Samples.h"
#include "Metronome.h"
#include "Player.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>

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

   #ifdef JucePlugin_Name
    const juce::String getName() const override { return JucePlugin_Name; }
   #else
    const juce::String getName() const override { return "Drum Rebuilder"; }   // inside another app
   #endif
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
    // Several at once (a selection of (onset time, instrument)), one undo step.
    void replaceInstruments (const std::vector<std::pair<double, drb::Instrument>>&, drb::Instrument to);
    void removeInstruments (const std::vector<std::pair<double, drb::Instrument>>&);
    // What else the sound at this hit could be: the instrument other than
    // `current` it sounds most like, if the analysis has an idea.
    std::optional<drb::Instrument> suggestInstead (int index, drb::Instrument current) const;
    // A hit the analysis missed: at the start of the sound near `seconds`
    // (within 40 ms), playing the recording there. Onto the hit already
    // there when one is that close. Returns its onset index.
    int addHit (double seconds, drb::Instrument);
    // Where a sound starts in the recording near `seconds`; negative = none.
    double findSoundStart (double seconds, double window) const;
    bool areHitsEdited() const { return onsetsEdited; }

    // The same, one instrument hit at a time (index into getAnalysis().hits),
    // for the MIDI editor.
    int moveHitTo (int hitIndex, double seconds, drb::Instrument);   // returns its new index
    void deleteHitAt (int hitIndex);
    void deleteHitsAt (std::vector<int> hitIndices);   // one undo step
    // Several hits' loudness at once: (hit index, velocity).
    void setHitVelocities (const std::vector<std::pair<int, float>>&, bool undoStep = true);
    int addHitAt (double seconds, drb::Instrument, float velocity);  // returns its index
    void setHitVelocity (int hitIndex, float velocity, bool undoStep = true);
    // The hits' changes made by hand (loudness, hits added, moved) are heard
    // only where they are made, in the MIDI editor; elsewhere the recording
    // plays as it was. Solo follows the same.
    void setEditsAudible (bool);
    // The Master's effects are heard only on the Master tab; elsewhere B is
    // the rebuilt beat without them.
    void setMasterAudible (bool);
    bool areEditsAudible() const { return editsAudible; }

    // How loud one hit sounds: the peak of its sound, in dBFS (-100 = silent);
    // as the recording has it; and the change made by hand (dB, 0 = none).
    float getHitLevelDb (int hitIndex) const;
    float getHitOriginalDb (int hitIndex) const;
    float getHitChangeDb (int hitIndex) const;
    // Several hits made louder / softer than in the recording: (index, dB
    // against the recording, from -40 to +12).
    void setHitChangesDb (const std::vector<std::pair<int, float>>&, bool undoStep = true);
    static constexpr float minChangeDb = -40.0f, maxChangeDb = 12.0f;
    // While a hit's loudness is being dragged: it plays in a loop, and
    // the change is heard as it goes. End puts the transport back.
    void startHitPreview (int hitIndex);
    void endHitPreview();

    // IN / OUT of one instrument hit: where its sound starts and ends.
    // Automatic until set: from the hit to the next hit of anything (at most
    // 0.4 s). Drags call beginHitEdit() first.
    std::pair<double, double> getSlice (int hitIndex) const;         // around the hit's time
    std::pair<double, double> getSourceSlice (int hitIndex) const;   // where its sound is in the recording
    // Where the sound detected at `time` (an onset of the recording) really
    // starts: a few ms earlier, at its first sample. Remembered per recording.
    double soundStart (double time) const;
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

    // What the views draw: the solo playback's position while it plays.
    double getShownPlayheadSeconds() const
    {
        return auditionPlayer.isPlaying() ? auditionPlayer.getPositionSeconds() : getPlayheadSeconds();
    }
    bool isAnythingPlaying() const { return player.isPlaying() || auditionPlayer.isPlaying(); }

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
    void moveSample (drb::Instrument, int from, int to);   // the user's order in the library
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
    // Which instruments Next sounds changes (all at first).
    bool getChanging (drb::Instrument i) const { return changing[(size_t) i]; }
    void setChanging (drb::Instrument, bool);
    // Forward: the next sounds already heard, else new ones at random for
    // the ticked instruments. Back: the sounds before.
    void nextSounds();
    void previousSounds();
    bool canGoBack() const;

    bool getKeepPitch() const { return keepPitch; }
    void setKeepPitch (bool);
    // Which part of an instrument's sound plays at each hit: from IN (0 =
    // its start) to OUT (0 = its end), seconds into the sound. Saved with
    // the beat; back to all of it when the row gets another sound.
    double getSoundLength (drb::Instrument i) const { return soundLengths[(size_t) i]; }
    double getSoundStart (drb::Instrument i) const { return soundIns[(size_t) i]; }
    void setSoundRange (drb::Instrument, double in, double out);
    // The Master (volume, reverb, delay) on the rebuilt beat (B) and its
    // export. Saved with the beat.
    const drb::MasterSettings& getMaster() const { return master; }
    void setMaster (const drb::MasterSettings&);
    // The sound a Drum Rack row plays now: its chosen sound, or (originals)
    // its loudest hit cut from the recording. Null = none.
    drb::SamplePtr getRowSound (drb::Instrument) const;
    double getRebuildBpm() const;   // the rebuilt beat's tempo
    void setRebuildBpm (double);
    double getTempoFactor() const { return tempoFactor; }

    // A/B: what the transport plays, the recording or the rebuilt beat.
    bool isListeningRebuilt() const { return listenRebuilt; }
    void setListeningRebuilt (bool);
    // B is heard only where it is chosen (Drum Rack, Master); the other
    // tabs always play the recording. The choice is kept for coming back.
    void setRebuiltAllowed (bool);
    bool isRendering() const { return rendering; }
    // Drum Rack: the beat goes round and round over its whole bars, so
    // the sounds can be changed while it plays. Off = the loop as before.
    void setBarLoop (bool);
    bool isBarLoop() const { return barLoop; }

    // The rebuilt beat as a stereo 24-bit WAV at the recording's sample
    // rate, with the last sounds' tails. Done in the background.
    void exportRebuiltWav (const juce::File&, std::function<void (bool ok)> done);
    // The same beat as exportRebuiltWav, handed back as audio (rendered on a
    // thread; `done` runs on the message thread).
    void renderRebuilt (std::function<void (bool ok, juce::AudioBuffer<float>, double sampleRate)> done);

    // Set by an app that has the editor inside it (Adormis Audio): the Export
    // tab then offers to open the rebuilt beat there.
    std::function<void (juce::AudioBuffer<float>, double sampleRate)> sendToHost;
    juce::String hostName;

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
    std::array<double, drb::numInstruments> soundLengths {}, soundIns {};
    drb::MasterSettings master;
    juce::String rowSoundId (drb::Instrument) const;   // which sound a row plays (its IN / OUT belong to it)
    void forgetRangesOfChangedRows (const std::array<juce::String, drb::numInstruments>& before);
    std::array<juce::String, drb::numInstruments> rowSoundIds() const;
    std::array<juce::File, drb::numInstruments> fixedFiles;
    std::array<bool, drb::numInstruments> changing = [] { std::array<bool, drb::numInstruments> a; a.fill (true); return a; }();
    struct SoundSet
    {
        std::array<drb::PickMode, drb::numInstruments> modes;
        std::array<juce::File, drb::numInstruments> files;
        juce::uint32 seed;
        bool operator== (const SoundSet& o) const { return modes == o.modes && files == o.files && seed == o.seed; }
    };
    std::vector<SoundSet> soundHistory;   // what Next / Previous step through
    size_t soundPos = 0;
    SoundSet currentSounds() const { return { pickModes, fixedFiles, seed }; }
    void applySounds (const SoundSet&);
    void rememberSounds();   // the sounds now, at soundPos, if they changed by hand
    bool keepPitch = false, listenRebuilt = false, rendering = false;
    bool wantRebuilt = false, rebuiltAllowed = true;   // the A / B choice; this tab may play B
    double tempoFactor = 1.0;
    juce::uint32 seed = 1;
    int renderGeneration = 0;
    std::unique_ptr<juce::AudioBuffer<float>> originalAtDevice, rebuiltAtDevice;
    std::unique_ptr<juce::AudioBuffer<float>> masteredAtDevice;   // the same with the Master (null = no Master set)
    bool masterAudible = false;
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
    bool barLoop = false, loopingBeforeBarLoop = false;
    juce::Range<double> barRange() const;   // bar 1 to the end of the last bar, recording seconds
    // The recording as it plays (A): each hit's stretch louder or softer
    // as its loudness was changed by hand.
    std::unique_ptr<juce::AudioBuffer<float>> originalWithGains() const;
    bool editsAudible = false;
    std::vector<drb::Hit> audibleHits() const;   // the hits as they sound now (edits or not)
    std::vector<juce::Range<double>> hitSounds (const std::vector<drb::Hit>&) const;   // where each one's sound is in the recording (IN / OUT)
    void addHandHits (juce::AudioBuffer<float>&, const drb::InstrumentGains* only) const;
    int originalGeneration = 0;
    mutable std::unique_ptr<drb::Bands> bands;   // the recording at the device rate, split; made when first needed
    const drb::Bands& getBands() const;
    juce::Range<double> previewLoop;   // a hit's bar while its loudness is dragged
    bool playingBeforePreview = false, soloPlayingBeforePreview = false, loopingBeforePreview = false;
    void applySoloLoop();   // the audition player loops the region, or a hit being dragged
    double positionBeforePreview = 0.0;
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
    mutable std::map<juce::int64, double> soundStarts;   // by onset time in microseconds
    // Each hit's level as the recording has it, worked out again when the
    // hits' places change (not when only their loudness does).
    mutable std::vector<float> originalDb;
    mutable std::vector<std::tuple<double, int, bool>> originalDbKey;
    mutable const void* originalDbBands = nullptr;
    mutable int soundStartsId = -1;
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
    void replaceAt (double onsetTime, drb::Instrument from, drb::Instrument to);   // no undo step, no hitsChanged
    void rebuildOnsets(); // one onset per distinct hit time
    const drb::Hit* borrowedHit (drb::Instrument) const;
    double borrowedSource (drb::Instrument) const;   // the sound a hit added by hand plays
    float borrowedVelocity (drb::Instrument) const;

    void installPlayback();
    void buildSoloAudio();   // the solo instruments' hits, at the device rate
    void updateMetronomeGrid();
    void placeDownbeat (double seconds);

    JUCE_DECLARE_WEAK_REFERENCEABLE (DrumRebuilderProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumRebuilderProcessor)
};
