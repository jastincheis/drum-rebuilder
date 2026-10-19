#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

DrumRebuilderProcessor::DrumRebuilderProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    library.rescan();
}

DrumRebuilderProcessor::~DrumRebuilderProcessor() = default;

void DrumRebuilderProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    player.prepare (sampleRate);
    previewPlayer.prepare (sampleRate);
    auditionPlayer.prepare (sampleRate);
    metronome.prepare (sampleRate);
    positions.assign ((size_t) std::max (samplesPerBlock, 512), -1);
    if (std::abs (sampleRate - preparedRate) > 0.5)
    {
        preparedRate = sampleRate;
        installPlayback();
    }
}

bool DrumRebuilderProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void DrumRebuilderProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // In chunks that fit `positions` (hosts may send bigger blocks than
    // announced).
    const int total = buffer.getNumSamples(), chunk = (int) positions.size();
    for (int start = 0; chunk > 0 && start < total; start += chunk)
    {
        const int num = std::min (chunk, total - start);
        player.render (buffer, start, num, positions.data());
        metronome.render (buffer, start, num, positions.data());
    }
    previewPlayer.render (buffer, 0, buffer.getNumSamples());
    auditionPlayer.render (buffer, 0, buffer.getNumSamples());
}

juce::AudioProcessorEditor* DrumRebuilderProcessor::createEditor()
{
    return new DrumRebuilderEditor (*this);
}

// The recording, the preview and the rebuilt beat at the device's rate,
// keeping the play positions.
void DrumRebuilderProcessor::installPlayback()
{
    if (preparedRate <= 0.0)
        return;

    if (loaded != nullptr)
    {
        originalAtDevice = std::make_unique<juce::AudioBuffer<float>> (drb::resample (loaded->audio, loaded->sampleRate, preparedRate));
        bands.reset();
        rebuiltAtDevice.reset();
        showSource (timelineFactor());
        requestRender();
    }
    if (preview != nullptr)
    {
        const double at = previewPlayer.getPositionSeconds();
        previewPlayer.setSource (std::make_unique<juce::AudioBuffer<float>> (drb::resample (preview->audio, preview->sampleRate, preparedRate)));
        previewPlayer.setPositionSeconds (at);
    }
}

// The player gets the recording, or the rebuilt beat while listening to it
// (B), at the same musical spot: positions scale with the rebuilt tempo.
void DrumRebuilderProcessor::showSource (double previousFactor)
{
    const bool rebuilt = listenRebuilt && rebuiltAtDevice != nullptr;
    if (! rebuilt && originalAtDevice == nullptr)
        return;
    const double at = player.getPositionSeconds() * previousFactor / timelineFactor();
    player.setSource (rebuilt ? std::make_unique<juce::AudioBuffer<float>> (*rebuiltAtDevice) : originalWithGains());
    player.setPositionSeconds (at);
    applyLoop();
    updateMetronomeGrid();
}

void DrumRebuilderProcessor::applyLoop()
{
    const double f = timelineFactor();
    if (! previewLoop.isEmpty())
        player.setLoop (true, { previewLoop.getStart() / f, previewLoop.getEnd() / f });
    else
        player.setLoop (player.isLooping(), { region.getStart() / f, region.getEnd() / f });
}

const drb::Bands& DrumRebuilderProcessor::getBands() const
{
    if (bands == nullptr || bands->low.getNumSamples() != originalAtDevice->getNumSamples())
        bands = std::make_unique<drb::Bands> (drb::splitBands (*originalAtDevice, preparedRate > 0.0 ? preparedRate : 44100.0));
    return *bands;
}

// Untouched until a hit is changed by hand; then from the bands, with the
// hits added or moved by hand mixed in.
std::unique_ptr<juce::AudioBuffer<float>> DrumRebuilderProcessor::originalWithGains() const
{
    if (! drb::hasHitGains (analysis.hits))
        return std::make_unique<juce::AudioBuffer<float>> (*originalAtDevice);
    auto out = std::make_unique<juce::AudioBuffer<float>> (drb::applyHitGains (getBands(), analysis.hits));
    addHandHits (*out, nullptr);
    return out;
}

// The hits added or moved by hand (of the instruments with gain > 0), each
// playing its sound at its place, added into `out`.
void DrumRebuilderProcessor::addHandHits (juce::AudioBuffer<float>& out, const drb::InstrumentGains* only) const
{
    auto input = makeRebuildInput();
    drb::RebuildInput hand;
    hand.audio = input.audio;
    hand.sampleRate = input.sampleRate;
    for (size_t i = 0; i < input.hits.size(); ++i)
    {
        const auto& h = input.hits[i];
        const bool byHand = h.added || std::abs (h.sourceTime() - h.time) > 1.0e-9;
        if (byHand && (only == nullptr || (*only)[(size_t) h.instrument] > 0.0f))
        {
            hand.hits.push_back (h);
            hand.slices.push_back (input.slices[i]);
        }
    }
    if (hand.hits.empty())
        return;
    const double rate = preparedRate > 0.0 ? preparedRate : 44100.0;
    const auto sounds = drb::renderRebuild (hand, drb::RebuildSettings(), rate, 0.0);
    const int n = std::min (out.getNumSamples(), sounds.getNumSamples());
    for (int ch = 0; ch < out.getNumChannels(); ++ch)
        out.addFrom (ch, 0, sounds, std::min (ch, sounds.getNumChannels() - 1), 0, n);
}

float DrumRebuilderProcessor::getHitLevelDb (int hitIndex) const
{
    if (originalAtDevice == nullptr)
        return -100.0f;
    return drb::hitLevelDb (getBands(), analysis.hits, hitIndex);
}

void DrumRebuilderProcessor::startHitPreview (int hitIndex)
{
    if (loaded == nullptr || ! juce::isPositiveAndBelow (hitIndex, (int) analysis.hits.size()) || ! previewLoop.isEmpty())
        return;

    // Just the hit, over and over: a little before it (to hear it come in)
    // and its sound, so every change is heard at once.
    const double at = analysis.hits[(size_t) hitIndex].time;
    juce::Range<double> loop (at - 0.15, at + 0.45);
    loop = loop.getIntersectionWith ({ 0.0, loaded->audio.getNumSamples() / loaded->sampleRate });
    if (loop.getLength() < 0.05)
        return;

    playingBeforePreview = player.isPlaying();
    soloPlayingBeforePreview = auditionPlayer.isPlaying();
    loopingBeforePreview = player.isLooping();
    positionBeforePreview = soloPlayingBeforePreview ? auditionPlayer.getPositionSeconds() : getPlayheadSeconds();
    player.pause();
    auditionPlayer.pause();
    previewPlayer.stop();
    previewLoop = loop;

    if (anySolo())
    {
        // Solo on: only the solo instruments loop.
        buildSoloAudio();
        auditionPlayer.setPositionSeconds (loop.getStart());
        auditionPlayer.play();
    }
    else
    {
        applyLoop();
        setPlayheadSeconds (loop.getStart());
        player.play();
    }
    sendChangeMessage();
}

void DrumRebuilderProcessor::endHitPreview()
{
    if (previewLoop.isEmpty())
        return;
    previewLoop = {};
    auditionPlayer.pause();
    auditionPlayer.setLoop (loopingBeforePreview, region);
    player.setLoop (loopingBeforePreview, {});
    applyLoop();
    player.pause();
    if (soloPlayingBeforePreview)
    {
        auditionPlayer.setPositionSeconds (positionBeforePreview);
        auditionPlayer.play();
    }
    else if (playingBeforePreview)
    {
        setPlayheadSeconds (positionBeforePreview);
        player.play();
    }
    else
        setPlayheadSeconds (positionBeforePreview);
    sendChangeMessage();
}

void DrumRebuilderProcessor::previewSample (const drb::Sample& sample)
{
    const double rate = preparedRate > 0.0 ? preparedRate : 44100.0;
    player.pause();
    auditionPlayer.pause();
    previewPlayer.stop();
    previewTarget = juce::File();
    preview.reset();
    previewPlayer.setSource (std::make_unique<juce::AudioBuffer<float>> (drb::resample (sample.audio, sample.sampleRate, rate)));
    previewPlayer.setLoop (false, {});
    previewPlayer.setPositionSeconds (0.0);
    previewPlayer.play();
}

void DrumRebuilderProcessor::previewFile (const juce::File& file)
{
    if (file == previewTarget)
    {
        if (preview != nullptr)   // already here: play it again from the start
        {
            player.pause();
            previewPlayer.setPositionSeconds (0.0);
            previewPlayer.play();
            sendChangeMessage();
        }
        return;                   // or still loading
    }

    const int generation = ++previewGeneration;
    previewTarget = file;
    previewError = {};
    previewPlayer.stop();
    previewPlayer.clearSource();
    preview.reset();
    sendChangeMessage();

    struct Result
    {
        std::optional<drb::LoadedAudio> audio;
        std::unique_ptr<juce::AudioBuffer<float>> playback;
        juce::String error;
    };

    juce::WeakReference<DrumRebuilderProcessor> self (this);
    const double rate = preparedRate > 0.0 ? preparedRate : 44100.0;

    juce::Thread::launch ([self, file, generation, rate]
    {
        auto result = std::make_shared<Result>();
        result->audio = drb::loadAudioFile (file, result->error);
        if (result->audio)
            result->playback = std::make_unique<juce::AudioBuffer<float>> (
                drb::resample (result->audio->audio, result->audio->sampleRate, rate));

        juce::MessageManager::callAsync ([self, generation, result, rate]
        {
            auto* p = self.get();
            if (p == nullptr || generation != p->previewGeneration)
                return;

            if (! result->audio)
            {
                p->previewError = result->error;
                p->sendChangeMessage();
                return;
            }

            p->preview = std::make_unique<drb::LoadedAudio> (std::move (*result->audio));
            if (std::abs (rate - p->preparedRate) < 0.5 || p->preparedRate <= 0.0)
                p->previewPlayer.setSource (std::move (result->playback));
            else
                p->installPlayback();
            p->previewPlayer.setPositionSeconds (0.0);
            p->player.pause();
            p->previewPlayer.play();
            p->sendChangeMessage();
        });
    });
}

void DrumRebuilderProcessor::loadFile (const juce::File& file)
{
    saveBeatMemory();   // the beat we leave keeps its fixes
    const int generation = ++loadGeneration;
    player.stop();
    previewPlayer.stop();
    auditionPlayer.stop();
    busy = true;
    status = "Loading " + file.getFileName() + "...";
    sendChangeMessage();

    // Everything slow happens on its own thread; the result comes back to the
    // message thread, and is dropped if another file was opened meanwhile.
    struct Result
    {
        std::optional<drb::LoadedAudio> audio;
        drb::AnalysisResult analysis;
        std::unique_ptr<juce::AudioBuffer<float>> playback;
        juce::String error;
    };

    juce::WeakReference<DrumRebuilderProcessor> self (this);
    const double rate = preparedRate > 0.0 ? preparedRate : 44100.0;

    juce::Thread::launch ([self, file, generation, rate]
    {
        auto result = std::make_shared<Result>();
        result->audio = drb::loadAudioFile (file, result->error);
        if (result->audio)
        {
            juce::MessageManager::callAsync ([self, generation, file]
            {
                if (self != nullptr && generation == self->loadGeneration)
                {
                    self->status = "Analysing " + file.getFileName() + "...";
                    self->sendChangeMessage();
                }
            });

            result->analysis = drb::analyse (result->audio->audio, result->audio->sampleRate);
            result->playback = std::make_unique<juce::AudioBuffer<float>> (
                drb::resample (result->audio->audio, result->audio->sampleRate, rate));
        }

        juce::MessageManager::callAsync ([self, generation, result, rate]
        {
            auto* p = self.get();
            if (p == nullptr || generation != p->loadGeneration)
                return;

            p->busy = false;
            if (! result->audio)
            {
                p->status = result->error;
                p->sendChangeMessage();
                return;
            }

            p->loaded = std::make_unique<drb::LoadedAudio> (std::move (*result->audio));
            p->browseFolder = p->loaded->file.getParentDirectory();
            p->loadedId = generation;
            p->analysis = std::move (result->analysis);
            p->tempoManual = false;
            p->onsetsEdited = false;
            p->undoStack.clear();
            p->redoStack.clear();
            p->auditionPlayer.stop();
            p->auditionPlayer.clearSource();
            p->solo = {};
            p->checks = {};
            // Opened before? Its fixes come back. The beat's own memory is
            // written at every change, so it wins over an older saved state.
            bool restoring = p->loaded->file == p->pendingFile;
            if (auto xml = juce::XmlDocument::parse (beatMemoryFile (p->loaded->file)))
            {
                p->readBeatState (juce::ValueTree::fromXml (*xml));
                p->pendingFile = p->loaded->file;
                restoring = true;
            }

            // The hits as they were fixed by hand last time.
            if (restoring && ! p->pendingHits.empty())
            {
                p->analysis.hits = p->pendingHits;
                p->analysis.onsets.clear();
                for (const auto& h : p->analysis.hits)
                    if (p->analysis.onsets.empty() || h.time > p->analysis.onsets.back().time + 1.0e-9)
                        p->analysis.onsets.push_back ({ h.time, 1.0f });
                p->onsetsEdited = true;
            }
            if (restoring)
                p->checks = p->pendingChecks;

            // The beat starts at its first sound, unless bar 1 was put
            // somewhere else by hand.
            if (p->analysis.grid.bpm > 0.0 && ! p->analysis.onsets.empty())
            {
                if (restoring && p->pendingBpm > 0.0)
                {
                    p->analysis.grid.bpm = p->pendingBpm;
                    p->placeDownbeat (p->pendingDownbeat);
                    p->tempoManual = true;
                }
                else
                    p->placeDownbeat (p->analysis.onsets.front().time);
            }
            p->pendingBpm = 0.0;
            p->pendingHits.clear();
            p->pendingFile = juce::File();
            p->updateMetronomeGrid();
            p->region = {};
            p->player.setLoop (p->player.isLooping(), {});

            p->listenRebuilt = false;
            p->rebuiltAtDevice.reset();
            p->tempoFactor = 1.0;
            p->keepPitch = false;
            p->chooseDefaultSounds();
            if (restoring && p->pendingRebuild.isValid())
            {
                const auto& r = p->pendingRebuild;
                p->keepPitch = r.getProperty ("keepPitch", false);
                p->tempoFactor = juce::jlimit (0.5, 2.0, (double) r.getProperty ("tempoFactor", 1.0));
                const auto modes = r.getProperty ("modes").toString();
                const auto files = juce::StringArray::fromTokens (r.getProperty ("fixed").toString(), "|", "");
                for (int i = 0; i < drb::numInstruments; ++i)
                {
                    if (i < modes.length())
                        p->pickModes[(size_t) i] = (drb::PickMode) juce::jlimit (0, 4, (int) (modes[i] - '0'));
                    if (i < files.size() && files[i].isNotEmpty())
                        p->fixedFiles[(size_t) i] = juce::File (files[i]);
                }
            }
            p->pendingRebuild = {};

            if (std::abs (rate - p->preparedRate) < 0.5 || p->preparedRate <= 0.0)
            {
                p->originalAtDevice = std::move (result->playback);
                p->bands.reset();
                p->player.setSource (p->originalWithGains());
                p->requestRender();
            }
            else
                p->installPlayback();   // the device rate changed while we worked
            p->player.setPositionSeconds (0.0);

            p->status = {};
            p->sendChangeMessage();
        });
    });
}

void DrumRebuilderProcessor::scaleTempo (double factor)
{
    auto& g = analysis.grid;
    if (g.bpm <= 0.0 || analysis.onsets.empty())
        return;

    const double bpm = g.bpm * factor;
    if (bpm < 30.0 || bpm > 400.0)
        return;

    g.bpm = bpm;
    const double bar = g.timeSignature.beatsPerBar * g.beatSeconds();
    g.bars = std::max (1, (int) std::ceil ((analysis.onsets.back().time - g.firstDownbeat + 0.05) / bar));
    tempoManual = true;
    updateMetronomeGrid();
    requestRender();
    sendChangeMessage();
}

// Bar 1 at `seconds`, the grid and the bar count following it.
void DrumRebuilderProcessor::placeDownbeat (double seconds)
{
    auto& g = analysis.grid;
    g.firstDownbeat = std::max (0.0, seconds);
    const double beat = g.beatSeconds();
    g.firstBeat = g.firstDownbeat - std::floor (g.firstDownbeat / beat) * beat;
    const double bar = g.timeSignature.beatsPerBar * beat;
    g.bars = std::max (1, (int) std::ceil ((analysis.onsets.back().time - g.firstDownbeat + 0.05) / bar));
    updateMetronomeGrid();
}

double DrumRebuilderProcessor::setFirstDownbeat (double seconds, bool snapToHit)
{
    auto& g = analysis.grid;
    if (g.bpm <= 0.0 || analysis.onsets.empty())
        return g.firstDownbeat;

    double best = seconds, distance = 0.040;
    if (snapToHit)
        for (const auto& o : analysis.onsets)
            if (std::abs (o.time - seconds) < distance)
            {
                distance = std::abs (o.time - seconds);
                best = o.time;
            }

    placeDownbeat (best);
    tempoManual = true;
    sendChangeMessage();
    return g.firstDownbeat;
}

double DrumRebuilderProcessor::moveOnset (int index, double seconds)
{
    auto& onsets = analysis.onsets;
    if (! juce::isPositiveAndBelow (index, (int) onsets.size()))
        return seconds;

    const double lo = index > 0 ? onsets[(size_t) index - 1].time + 0.001 : 0.0;
    const double hi = index + 1 < (int) onsets.size() ? onsets[(size_t) index + 1].time - 0.001
                                                      : (loaded != nullptr ? loaded->audio.getNumSamples() / loaded->sampleRate : seconds);
    const double from = onsets[(size_t) index].time;
    const double to = juce::jlimit (lo, std::max (lo, hi), seconds);
    onsets[(size_t) index].time = to;
    for (auto& h : analysis.hits)
        if (std::abs (h.time - from) < 1.0e-9)
        {
            // A correction: the sound really is where the marker goes.
            h.time = to;
            if (! h.added)
                h.source = -1.0;
            h.manual = true;
        }
    hitsChanged();
    return to;
}

std::vector<drb::Instrument> DrumRebuilderProcessor::getInstrumentsAt (int index) const
{
    std::vector<drb::Instrument> out;
    if (juce::isPositiveAndBelow (index, (int) analysis.onsets.size()))
        for (const auto& h : analysis.hits)
            if (std::abs (h.time - analysis.onsets[(size_t) index].time) < 1.0e-9)
                out.push_back (h.instrument);
    return out;
}

void DrumRebuilderProcessor::toggleInstrument (int index, drb::Instrument instrument)
{
    if (! juce::isPositiveAndBelow (index, (int) analysis.onsets.size()))
        return;
    beginHitEdit();

    const double t = analysis.onsets[(size_t) index].time;
    auto& hits = analysis.hits;
    const auto same = [&] (const drb::Hit& h) { return std::abs (h.time - t) < 1.0e-9 && h.instrument == instrument; };

    if (std::any_of (hits.begin(), hits.end(), same))
    {
        hits.erase (std::remove_if (hits.begin(), hits.end(), same), hits.end());
        if (getInstrumentsAt (index).empty())
            analysis.onsets.erase (analysis.onsets.begin() + index);
    }
    else
    {
        // As loud as the other hits at this onset. Where the recording has
        // a sound (a hit found there, or a new marker on it), that is its
        // sound; only next to hits added in the MIDI editor it borrows one.
        float velocity = -1.0f;
        bool ownSound = true;
        for (const auto& h : hits)
            if (std::abs (h.time - t) < 1.0e-9)
            {
                velocity = h.velocity;
                ownSound = ! h.added && h.source < 0.0;
            }
        if (velocity < 0.0f && loaded != nullptr)
        {
            // How loud the recording is there, against its loudest moment.
            const auto& audio = loaded->audio;
            const int a = juce::jlimit (0, audio.getNumSamples(), (int) (t * loaded->sampleRate));
            const int len = std::min (audio.getNumSamples() - a, (int) (0.03 * loaded->sampleRate));
            const float peak = audio.getMagnitude (0, audio.getNumSamples());
            velocity = len > 0 && peak > 0.0f ? juce::jlimit (0.05f, 1.0f, audio.getMagnitude (a, len) / peak) : 0.8f;
        }
        drb::Hit hit { t, instrument, velocity < 0.0f ? 0.8f : velocity, 1.0f, true };
        if (! ownSound)
        {
            hit.added = true;
            hit.source = borrowedSource (instrument);
            hit.soundVelocity = borrowedVelocity (instrument);
        }
        hits.insert (std::upper_bound (hits.begin(), hits.end(), hit,
                                       [] (const drb::Hit& a, const drb::Hit& b) { return a.time < b.time; }),
                     hit);
    }
    hitsChanged();
}

void DrumRebuilderProcessor::deleteHit (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) analysis.onsets.size()))
        return;
    beginHitEdit();
    const double t = analysis.onsets[(size_t) index].time;
    auto& hits = analysis.hits;
    hits.erase (std::remove_if (hits.begin(), hits.end(), [t] (const drb::Hit& h) { return std::abs (h.time - t) < 1.0e-9; }),
                hits.end());
    analysis.onsets.erase (analysis.onsets.begin() + index);
    hitsChanged();
}

void DrumRebuilderProcessor::replaceInstrument (int index, drb::Instrument from, drb::Instrument to)
{
    if (! juce::isPositiveAndBelow (index, (int) analysis.onsets.size()) || from == to)
        return;
    beginHitEdit();
    const double t = analysis.onsets[(size_t) index].time;
    auto& hits = analysis.hits;
    const auto atT = [t] (const drb::Hit& h) { return std::abs (h.time - t) < 1.0e-9; };

    // Already there as well: then just drop `from`.
    const bool toPresent = std::any_of (hits.begin(), hits.end(), [&] (const drb::Hit& h) { return atT (h) && h.instrument == to; });
    for (auto it = hits.begin(); it != hits.end(); ++it)
        if (atT (*it) && it->instrument == from)
        {
            if (toPresent)
                hits.erase (it);
            else
            {
                it->instrument = to;
                it->manual = true;
            }
            break;
        }
    hitsChanged();
}

std::optional<drb::Instrument> DrumRebuilderProcessor::suggestInstead (int index, drb::Instrument current) const
{
    if (! juce::isPositiveAndBelow (index, (int) analysis.onsets.size()))
        return std::nullopt;
    const double t = analysis.onsets[(size_t) index].time;
    for (const auto& h : analysis.hits)
        if (std::abs (h.time - t) < 1.0e-9)
        {
            std::optional<drb::Instrument> best;
            float bestValue = 0.05f;
            for (auto i : drb::allInstruments)
                if (i != current && h.likeness[(size_t) i] > bestValue)
                {
                    bestValue = h.likeness[(size_t) i];
                    best = i;
                }
            return best;
        }
    return std::nullopt;
}

double DrumRebuilderProcessor::findSoundStart (double seconds, double window) const
{
    return loaded != nullptr ? drb::findAttack (loaded->audio, loaded->sampleRate, seconds, window) : -1.0;
}

int DrumRebuilderProcessor::addHit (double seconds, drb::Instrument instrument)
{
    auto& onsets = analysis.onsets;
    if (const double start = findSoundStart (seconds, 0.04); start >= 0.0)
        seconds = start;

    // A hit already there: this instrument joins it.
    for (int i = 0; i < (int) onsets.size(); ++i)
        if (std::abs (onsets[(size_t) i].time - seconds) < 0.015)
        {
            const auto present = getInstrumentsAt (i);
            if (std::find (present.begin(), present.end(), instrument) == present.end())
                toggleInstrument (i, instrument);
            return i;
        }

    const auto it = std::upper_bound (onsets.begin(), onsets.end(), seconds,
                                      [] (double t, const drb::Onset& o) { return t < o.time; });
    const int index = (int) (it - onsets.begin());
    onsets.insert (it, { seconds, 1.0f });
    toggleInstrument (index, instrument);   // takes the undo snapshot
    return index;
}

namespace
{
bool earlier (const drb::Hit& a, const drb::Hit& b) { return a.time < b.time; }
} // namespace

int DrumRebuilderProcessor::moveHitTo (int hitIndex, double seconds, drb::Instrument instrument)
{
    auto& hits = analysis.hits;
    if (! juce::isPositiveAndBelow (hitIndex, (int) hits.size()))
        return hitIndex;

    auto hit = hits[(size_t) hitIndex];
    if (hit.source < 0.0)
        hit.source = hit.time;   // its sound stays where it is in the recording
    hit.time = juce::jlimit (0.0, loaded != nullptr ? loaded->audio.getNumSamples() / loaded->sampleRate : seconds, seconds);
    hit.instrument = instrument;
    hit.manual = true;
    hits.erase (hits.begin() + hitIndex);
    const auto it = hits.insert (std::upper_bound (hits.begin(), hits.end(), hit, earlier), hit);
    rebuildOnsets();
    hitsChanged();
    return (int) (it - hits.begin());
}

void DrumRebuilderProcessor::deleteHitAt (int hitIndex)
{
    if (! juce::isPositiveAndBelow (hitIndex, (int) analysis.hits.size()))
        return;
    beginHitEdit();
    analysis.hits.erase (analysis.hits.begin() + hitIndex);
    rebuildOnsets();
    hitsChanged();
}

int DrumRebuilderProcessor::addHitAt (double seconds, drb::Instrument instrument, float velocity)
{
    beginHitEdit();
    auto& hits = analysis.hits;
    drb::Hit hit { std::max (0.0, seconds), instrument, velocity, 1.0f, true };
    hit.added = true;
    hit.source = borrowedSource (instrument);
    hit.soundVelocity = borrowedVelocity (instrument);
    const auto it = hits.insert (std::upper_bound (hits.begin(), hits.end(), hit, earlier), hit);
    const int index = (int) (it - hits.begin());
    rebuildOnsets();
    hitsChanged();
    return index;
}

void DrumRebuilderProcessor::setHitVelocity (int hitIndex, float velocity, bool undoStep)
{
    if (! juce::isPositiveAndBelow (hitIndex, (int) analysis.hits.size()))
        return;
    if (undoStep)
        beginHitEdit();
    auto& h = analysis.hits[(size_t) hitIndex];
    if (h.soundVelocity < 0.0f)
        h.soundVelocity = h.velocity;   // how loud its sound is in the recording
    analysis.hits[(size_t) hitIndex].velocity = juce::jlimit (0.05f, 1.0f, velocity);
    analysis.hits[(size_t) hitIndex].manual = true;
    hitsChanged();
}

std::pair<double, double> DrumRebuilderProcessor::getSlice (int hitIndex) const
{
    const auto& h = analysis.hits[(size_t) hitIndex];
    if (h.sliceOut > h.sliceIn)
        return { h.time + h.sliceIn, h.time + h.sliceOut };

    const auto& onsets = analysis.onsets;
    const auto next = std::upper_bound (onsets.begin(), onsets.end(), h.time + 1.0e-6,
                                        [] (double t, const drb::Onset& o) { return t < o.time; });
    return { h.time - 0.002, std::min (h.time + 0.4, next != onsets.end() ? next->time - 0.002 : h.time + 0.4) };
}

std::pair<double, double> DrumRebuilderProcessor::getSourceSlice (int hitIndex) const
{
    const auto& h = analysis.hits[(size_t) hitIndex];
    const auto [in, out] = getSlice (hitIndex);
    const double shift = h.sourceTime() - h.time;
    double end = out + shift;

    // Played somewhere else (moved, or a borrowed sound): its automatic cut
    // also stops where the next sound starts in the recording, so whatever
    // followed it there doesn't come along.
    if (std::abs (shift) > 1.0e-9 && ! (h.sliceOut > h.sliceIn))
        for (const auto& o : analysis.hits)
            if (! o.added && o.sourceTime() > h.sourceTime() + 0.005)
                end = std::min (end, o.sourceTime() - 0.002);
    return { in + shift, std::max (in + shift + 0.005, end) };
}

// A hit added by hand has no sound of its own at its time. The sound a hit added by hand plays: the instrument's loudest hit that
// sounds on its own (nothing else struck within 30 ms), so no other
// instrument comes along with it; the loudest of all when none is alone.
const drb::Hit* DrumRebuilderProcessor::borrowedHit (drb::Instrument instrument) const
{
    const auto& hits = analysis.hits;
    auto alone = [&] (const drb::Hit& h)
    {
        return std::none_of (hits.begin(), hits.end(), [&] (const drb::Hit& o)
        {
            return &o != &h && ! o.added && o.instrument != instrument && std::abs (o.sourceTime() - h.sourceTime()) < 0.03;
        });
    };
    const drb::Hit* best = nullptr;
    bool bestAlone = false;
    for (const auto& h : hits)
    {
        if (h.instrument != instrument || h.added)
            continue;
        const bool a = alone (h);
        if (best == nullptr || (a && ! bestAlone) || (a == bestAlone && h.velocity > best->velocity))
        {
            best = &h;
            bestAlone = a;
        }
    }
    return best;
}

double DrumRebuilderProcessor::borrowedSource (drb::Instrument instrument) const
{
    const auto* best = borrowedHit (instrument);
    return best != nullptr ? best->sourceTime() : -1.0;
}

// How loud the borrowed sound is in the recording (as a velocity).
float DrumRebuilderProcessor::borrowedVelocity (drb::Instrument instrument) const
{
    const auto* best = borrowedHit (instrument);
    return best != nullptr ? best->velocity / best->soundGain() : -1.0f;
}

void DrumRebuilderProcessor::setSlice (int hitIndex, double in, double out)
{
    if (! juce::isPositiveAndBelow (hitIndex, (int) analysis.hits.size()) || out <= in)
        return;
    auto& h = analysis.hits[(size_t) hitIndex];
    h.sliceIn = (float) (in - h.time);
    h.sliceOut = (float) (out - h.time);
    h.manual = true;
    hitsChanged();
}

int DrumRebuilderProcessor::findHit (double time, drb::Instrument instrument) const
{
    for (size_t i = 0; i < analysis.hits.size(); ++i)
        if (std::abs (analysis.hits[i].time - time) < 1.0e-9 && analysis.hits[i].instrument == instrument)
            return (int) i;
    return -1;
}

void DrumRebuilderProcessor::quantizeHits (double divisionBeats, double strength)
{
    if (analysis.grid.bpm <= 0.0 || analysis.hits.empty())
        return;
    beginHitEdit();
    for (auto& h : analysis.hits)
        if (h.source < 0.0)
            h.source = h.time;   // the sounds stay where they are in the recording
    analysis.hits = drb::quantize (analysis.hits, analysis.grid, divisionBeats, strength);
    rebuildOnsets();
    hitsChanged();
}

void DrumRebuilderProcessor::beginHitEdit()
{
    undoStack.push_back (analysis.hits);
    if (undoStack.size() > 100)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void DrumRebuilderProcessor::undo()
{
    if (undoStack.empty())
        return;
    redoStack.push_back (analysis.hits);
    analysis.hits = undoStack.back();
    undoStack.pop_back();
    rebuildOnsets();
    hitsChanged();
}

void DrumRebuilderProcessor::redo()
{
    if (redoStack.empty())
        return;
    undoStack.push_back (analysis.hits);
    analysis.hits = redoStack.back();
    redoStack.pop_back();
    rebuildOnsets();
    hitsChanged();
}

// Keeps the strength of onsets that are still there; new ones get the
// median, so they draw like an ordinary hit.
void DrumRebuilderProcessor::rebuildOnsets()
{
    auto& onsets = analysis.onsets;
    std::vector<float> strengths;
    for (const auto& o : onsets)
        strengths.push_back (o.strength);
    float median = 1.0f;
    if (! strengths.empty())
    {
        std::nth_element (strengths.begin(), strengths.begin() + (long) strengths.size() / 2, strengths.end());
        median = strengths[strengths.size() / 2];
    }

    std::vector<drb::Onset> rebuilt;
    for (const auto& h : analysis.hits)
    {
        if (! rebuilt.empty() && h.time < rebuilt.back().time + 1.0e-9)
            continue;
        float strength = median;
        const auto it = std::lower_bound (onsets.begin(), onsets.end(), h.time - 1.0e-9,
                                          [] (const drb::Onset& o, double t) { return o.time < t; });
        if (it != onsets.end() && std::abs (it->time - h.time) < 1.0e-9)
            strength = it->strength;
        rebuilt.push_back ({ h.time, strength });
    }
    onsets = std::move (rebuilt);
}

void DrumRebuilderProcessor::hitsChanged()
{
    onsetsEdited = true;
    if (! analysis.onsets.empty() && analysis.grid.bpm > 0.0)
    {
        const auto& g = analysis.grid;
        analysis.grid.bars = std::max (1, (int) std::ceil ((analysis.onsets.back().time - g.firstDownbeat + 0.05)
                                                           / (g.timeSignature.beatsPerBar * g.beatSeconds())));
    }

    requestRender();

    // The recording (A) plays the hits' loudness changes too: put in again
    // once the edits pause for a moment.
    {
        const int generation = ++originalGeneration;
        juce::WeakReference<DrumRebuilderProcessor> self (this);
        juce::Timer::callAfterDelay (40, [self, generation]
        {
            if (self != nullptr && generation == self->originalGeneration && ! self->listenRebuilt)
                self->showSource (1.0);
        });
    }

    // Solo playing: hear the change, from where it was. Once the edits
    // pause for a moment, so a drag doesn't rebuild it at every step.
    if (auditionPlayer.isPlaying())
    {
        const int generation = ++soloRebuild;
        juce::WeakReference<DrumRebuilderProcessor> self (this);
        juce::Timer::callAfterDelay (150, [self, generation]
        {
            if (self == nullptr || generation != self->soloRebuild || ! self->auditionPlayer.isPlaying())
                return;
            const double at = self->auditionPlayer.getPositionSeconds();
            self->buildSoloAudio();
            self->auditionPlayer.setPositionSeconds (at);
        });
    }
    sendChangeMessage();
}

void DrumRebuilderProcessor::setRegion (juce::Range<double> r)
{
    region = r;
    applyLoop();
    auditionPlayer.setLoop (player.isLooping(), region);
    sendChangeMessage();
}

void DrumRebuilderProcessor::setLooping (bool on)
{
    player.setLoop (on, {});
    applyLoop();
    auditionPlayer.setLoop (on, region);
    sendChangeMessage();
}

bool DrumRebuilderProcessor::anySolo() const
{
    return std::any_of (solo.begin(), solo.end(), [] (bool b) { return b; });
}

std::optional<drb::Instrument> DrumRebuilderProcessor::firstSolo() const
{
    for (auto i : drb::allInstruments)
        if (solo[(size_t) i])
            return i;
    return std::nullopt;
}

std::vector<drb::Instrument> DrumRebuilderProcessor::getSolo() const
{
    std::vector<drb::Instrument> out;
    for (auto i : drb::allInstruments)
        if (solo[(size_t) i])
            out.push_back (i);
    return out;
}

void DrumRebuilderProcessor::setSolo (drb::Instrument i, bool on)
{
    solo[(size_t) i] = on;
    if (player.isPlaying() && anySolo())
    {
        // The whole beat is playing: from here on only the solo ones.
        playSolo();
        return;
    }
    if (auditionPlayer.isPlaying())
    {
        if (anySolo())
        {
            const double at = auditionPlayer.getPositionSeconds();
            buildSoloAudio();
            auditionPlayer.setPositionSeconds (at);
        }
        else
        {
            // Nothing solo any more: carry on with the whole beat.
            const double at = auditionPlayer.getPositionSeconds();
            auditionPlayer.stop();
            player.setPositionSeconds (at);
            player.play();
        }
    }
    sendChangeMessage();
}

// Only the solo instruments' hits: with their own sounds (A) or the rebuilt
// beat's sounds (B), at the recording's tempo so the views stay in step.
void DrumRebuilderProcessor::applySoloLoop()
{
    if (! previewLoop.isEmpty())
        auditionPlayer.setLoop (true, previewLoop);
    else
        auditionPlayer.setLoop (player.isLooping(), region);
}

void DrumRebuilderProcessor::buildSoloAudio()
{
    if (loaded == nullptr)
        return;

    // The recording (A): itself, with everything not on solo taken out
    // (band by band where hits are played together).
    if (! listenRebuilt && originalAtDevice != nullptr)
    {
        drb::InstrumentGains only {};
        for (auto i : drb::allInstruments)
            only[(size_t) i] = solo[(size_t) i] ? 1.0f : 0.0f;
        auto audio = std::make_unique<juce::AudioBuffer<float>> (drb::applyHitGains (getBands(), analysis.hits, &only));
        addHandHits (*audio, &only);
        auditionPlayer.setSource (std::move (audio));
        applySoloLoop();
        return;
    }

    auto input = makeRebuildInput();
    drb::RebuildInput only;
    only.audio = input.audio;
    only.sampleRate = input.sampleRate;
    for (size_t i = 0; i < input.hits.size(); ++i)
        if (solo[(size_t) input.hits[i].instrument])
        {
            only.hits.push_back (input.hits[i]);
            only.slices.push_back (input.slices[i]);
        }

    auto settings = listenRebuilt ? makeRebuildSettings() : drb::RebuildSettings();
    settings.tempoFactor = 1.0;
    const double rate = preparedRate > 0.0 ? preparedRate : 44100.0;
    auditionPlayer.setSource (std::make_unique<juce::AudioBuffer<float>> (drb::renderRebuild (only, settings, rate)));
    applySoloLoop();
}

void DrumRebuilderProcessor::playSolo()
{
    if (loaded == nullptr || ! anySolo())
        return;
    const double at = player.getPositionSeconds();
    player.pause();
    previewPlayer.stop();
    buildSoloAudio();
    auditionPlayer.setPositionSeconds (at);
    auditionPlayer.play();
    sendChangeMessage();
}

void DrumRebuilderProcessor::pauseSolo()
{
    if (! auditionPlayer.isPlaying())
        return;
    const double at = auditionPlayer.getPositionSeconds();
    auditionPlayer.pause();
    player.setPositionSeconds (at);
    sendChangeMessage();
}

// --- Rebuild ---------------------------------------------------------------

void DrumRebuilderProcessor::rescanLibrary()
{
    library.rescan();
    chooseDefaultSounds();
    requestRender();
    sendChangeMessage();
}

juce::String DrumRebuilderProcessor::getBeatName() const
{
    return loaded != nullptr ? loaded->file.getFileNameWithoutExtension() : juce::String();
}

int DrumRebuilderProcessor::saveOriginals()
{
    if (loaded == nullptr)
        return 0;
    std::vector<drb::Cut> cuts;
    for (size_t i = 0; i < analysis.hits.size(); ++i)
    {
        if (analysis.hits[i].added)
            continue;   // no sound of its own
        const auto [in, out] = getSourceSlice ((int) i);
        cuts.push_back ({ analysis.hits[i].instrument, in, out });
    }
    const int written = library.saveOriginals (getBeatName(), loaded->audio, loaded->sampleRate, cuts);
    chooseDefaultSounds();
    requestRender();
    sendChangeMessage();
    return written;
}

std::vector<drb::SamplePtr> DrumRebuilderProcessor::getPool (drb::Instrument i) const
{
    const auto own = juce::File::createLegalFileName (getBeatName());
    std::vector<drb::SamplePtr> pool;
    for (const auto& s : library.samples (i))
        if (! (s->original && s->fromBeat == own))
            pool.push_back (s);
    return pool;
}

// Instruments with sounds in their folder start on one of them (picked at
// random), the others on their original sound. Choices still valid stay.
void DrumRebuilderProcessor::chooseDefaultSounds()
{
    auto& random = juce::Random::getSystemRandom();
    for (auto i : drb::allInstruments)
    {
        const auto pool = getPool (i);
        auto& mode = pickModes[(size_t) i];
        auto& fixed = fixedFiles[(size_t) i];
        const bool fixedOk = std::any_of (pool.begin(), pool.end(), [&] (const auto& s) { return s->file == fixed; });

        if (pool.empty())
            mode = drb::PickMode::Original;
        else if (! fixedOk)
        {
            fixed = pool[(size_t) random.nextInt ((int) pool.size())]->file;
            if (mode == drb::PickMode::Original)
                mode = drb::PickMode::Fixed;
        }
    }
}

void DrumRebuilderProcessor::setPickMode (drb::Instrument i, drb::PickMode m)
{
    pickModes[(size_t) i] = m;
    requestRender();
    sendChangeMessage();
}

drb::SamplePtr DrumRebuilderProcessor::getFixedSample (drb::Instrument i) const
{
    for (const auto& s : getPool (i))
        if (s->file == fixedFiles[(size_t) i])
            return s;
    return nullptr;
}

void DrumRebuilderProcessor::setFixedSample (drb::Instrument i, const juce::File& f)
{
    fixedFiles[(size_t) i] = f;
    if (pickModes[(size_t) i] == drb::PickMode::Original)
        pickModes[(size_t) i] = drb::PickMode::Fixed;
    requestRender();
    sendChangeMessage();
}

void DrumRebuilderProcessor::nextSounds()
{
    auto& random = juce::Random::getSystemRandom();
    seed = (juce::uint32) random.nextInt();
    for (auto i : drb::allInstruments)
    {
        const auto pool = getPool (i);
        if (pool.size() < 2 || pickModes[(size_t) i] == drb::PickMode::Original)
            continue;
        // Another one than now.
        juce::File next;
        do
            next = pool[(size_t) random.nextInt ((int) pool.size())]->file;
        while (next == fixedFiles[(size_t) i]);
        fixedFiles[(size_t) i] = next;
    }
    requestRender();
    sendChangeMessage();
}

void DrumRebuilderProcessor::setKeepPitch (bool on)
{
    keepPitch = on;
    requestRender();
    sendChangeMessage();
}

double DrumRebuilderProcessor::getRebuildBpm() const
{
    return analysis.grid.bpm * tempoFactor;
}

void DrumRebuilderProcessor::setRebuildBpm (double bpm)
{
    if (analysis.grid.bpm <= 0.0)
        return;
    tempoFactor = juce::jlimit (0.5, 2.0, bpm / analysis.grid.bpm);
    requestRender();
    sendChangeMessage();
}

void DrumRebuilderProcessor::setListeningRebuilt (bool on)
{
    const double before = timelineFactor();
    listenRebuilt = on;
    showSource (before);
    sendChangeMessage();
}

drb::RebuildInput DrumRebuilderProcessor::makeRebuildInput() const
{
    drb::RebuildInput in;
    in.audio = loaded != nullptr ? &loaded->audio : nullptr;
    in.sampleRate = loaded != nullptr ? loaded->sampleRate : 44100.0;
    in.hits = analysis.hits;
    for (size_t i = 0; i < analysis.hits.size(); ++i)
        in.slices.push_back (getSourceSlice ((int) i));
    return in;
}

drb::RebuildSettings DrumRebuilderProcessor::makeRebuildSettings() const
{
    drb::RebuildSettings settings;
    for (auto i : drb::allInstruments)
    {
        auto& plan = settings.instruments[(size_t) i];
        plan.mode = pickModes[(size_t) i];
        if (plan.mode == drb::PickMode::Original)
            continue;
        plan.pool = getPool (i);
        for (size_t k = 0; k < plan.pool.size(); ++k)
            if (plan.pool[k]->file == fixedFiles[(size_t) i])
                plan.fixed = (int) k;
    }
    settings.keepPitch = keepPitch;
    settings.tempoFactor = tempoFactor;
    settings.seed = seed;
    return settings;
}

// Renders the rebuilt beat in the background once changes pause for a
// moment; the newest request wins.
void DrumRebuilderProcessor::requestRender()
{
    if (loaded == nullptr)
        return;
    scheduleBeatMemory();   // nearly every change comes through here
    const int generation = ++renderGeneration;
    rendering = true;
    juce::WeakReference<DrumRebuilderProcessor> self (this);
    juce::Timer::callAfterDelay (120, [self, generation]
    {
        if (self == nullptr || generation != self->renderGeneration || self->loaded == nullptr)
            return;

        struct Job
        {
            juce::AudioBuffer<float> audio;
            drb::RebuildInput input;
            drb::RebuildSettings settings;
            double rate = 44100.0;
            juce::AudioBuffer<float> result;
        };
        auto job = std::make_shared<Job>();
        job->audio = self->loaded->audio;
        job->input = self->makeRebuildInput();
        job->input.audio = &job->audio;
        job->settings = self->makeRebuildSettings();
        job->rate = self->preparedRate > 0.0 ? self->preparedRate : 44100.0;

        juce::Thread::launch ([self, generation, job]
        {
            job->result = drb::renderRebuild (job->input, job->settings, job->rate);
            juce::MessageManager::callAsync ([self, generation, job]
            {
                if (self == nullptr || generation != self->renderGeneration)
                    return;
                const double before = self->timelineFactor();
                self->rebuiltAtDevice = std::make_unique<juce::AudioBuffer<float>> (std::move (job->result));
                self->rendering = false;
                if (self->listenRebuilt)
                    self->showSource (before);
                self->sendChangeMessage();
            });
        });
    });
}

void DrumRebuilderProcessor::exportRebuiltWav (const juce::File& file, std::function<void (bool)> done)
{
    if (loaded == nullptr)
    {
        done (false);
        return;
    }
    struct Job
    {
        juce::AudioBuffer<float> audio;
        drb::RebuildInput input;
        drb::RebuildSettings settings;
        double rate = 44100.0;
        juce::File file;
        std::function<void (bool)> done;
    };
    auto job = std::make_shared<Job>();
    job->audio = loaded->audio;
    job->input = makeRebuildInput();
    job->input.audio = &job->audio;
    job->settings = makeRebuildSettings();
    job->rate = loaded->sampleRate;
    job->file = file;
    job->done = std::move (done);

    juce::Thread::launch ([job]
    {
        const auto beat = drb::renderRebuild (job->input, job->settings, job->rate, 2.0);
        const bool ok = drb::writeWav (job->file, beat, job->rate);
        juce::MessageManager::callAsync ([job, ok] { job->done (ok); });
    });
}

void DrumRebuilderProcessor::setCheck (drb::Instrument i, int state)
{
    checks[(size_t) i] = state;
    scheduleBeatMemory();
    sendChangeMessage();
}

void DrumRebuilderProcessor::setMetronome (bool on)
{
    metronome.setEnabled (on);
    sendChangeMessage();
}

void DrumRebuilderProcessor::updateMetronomeGrid()
{
    const auto& g = analysis.grid;
    const double f = timelineFactor();
    metronome.setGrid (g.bpm * f, g.firstDownbeat / f, g.timeSignature.beatsPerBar);
}

// Everything fixed by hand on the open beat: tempo, bar 1, the hits with
// their instruments and IN / OUT, the checks, the rebuild's choices.
juce::ValueTree DrumRebuilderProcessor::makeBeatState() const
{
    juce::ValueTree beat ("Beat");
    if (loaded == nullptr)
        return beat;

    beat.setProperty ("file", loaded->file.getFullPathName(), nullptr);
    if (tempoManual)
    {
        beat.setProperty ("bpm", analysis.grid.bpm, nullptr);
        beat.setProperty ("bar1", analysis.grid.firstDownbeat, nullptr);
    }

    juce::StringArray c;
    for (int v : checks)
        c.add (juce::String (v));
    beat.setProperty ("checks", c.joinIntoString (""), nullptr);

    if (onsetsEdited)
    {
        juce::StringArray items;   // time:instrument:velocity:in:out:likeness of kick,snare,hat closed,hat open:source:added:soundVelocity
        for (const auto& h : analysis.hits)
            items.add (juce::String (h.time, 6) + ":" + juce::String ((int) h.instrument) + ":" + juce::String (h.velocity, 3)
                       + ":" + juce::String (h.sliceIn, 4) + ":" + juce::String (h.sliceOut, 4) + ":"
                       + juce::String (h.likeness[0], 2) + "," + juce::String (h.likeness[1], 2) + ","
                       + juce::String (h.likeness[2], 2) + "," + juce::String (h.likeness[3], 2)
                       + ":" + juce::String (h.source, 6) + ":" + (h.added ? "1" : "0") + ":" + juce::String (h.soundVelocity, 3));
        beat.setProperty ("hitList", items.joinIntoString (" "), nullptr);
    }

    juce::ValueTree r ("Rebuild");
    juce::String modes;
    juce::StringArray files;
    for (auto i : drb::allInstruments)
    {
        modes << (int) pickModes[(size_t) i];
        files.add (fixedFiles[(size_t) i].getFullPathName());
    }
    r.setProperty ("modes", modes, nullptr);
    r.setProperty ("fixed", files.joinIntoString ("|"), nullptr);
    r.setProperty ("keepPitch", keepPitch, nullptr);
    r.setProperty ("tempoFactor", tempoFactor, nullptr);
    beat.addChild (r, -1, nullptr);
    return beat;
}

// The other way: what makeBeatState() wrote becomes "pending", put back once
// that file has been analysed again.
void DrumRebuilderProcessor::readBeatState (const juce::ValueTree& beat)
{
    pendingFile = juce::File (beat.getProperty ("file").toString());
    pendingBpm = beat.getProperty ("bpm", 0.0);
    pendingDownbeat = beat.getProperty ("bar1", 0.0);
    const auto c = beat.getProperty ("checks").toString();
    for (int i = 0; i < drb::numInstruments; ++i)
        pendingChecks[(size_t) i] = i < c.length() ? juce::jlimit (0, 2, (int) (c[i] - '0')) : 0;
    pendingRebuild = beat.getChildWithName ("Rebuild").createCopy();
    pendingHits.clear();
    for (const auto& item : juce::StringArray::fromTokens (beat.getProperty ("hitList").toString(), " ", ""))
    {
        const auto parts = juce::StringArray::fromTokens (item, ":", "");
        const int instrument = parts[1].getIntValue();
        if (parts.size() >= 3 && juce::isPositiveAndBelow (instrument, drb::numInstruments))
        {
            drb::Hit h { parts[0].getDoubleValue(), (drb::Instrument) instrument, (float) parts[2].getDoubleValue(), 1.0f, true };
            if (parts.size() >= 6)
            {
                h.sliceIn = (float) parts[3].getDoubleValue();
                h.sliceOut = (float) parts[4].getDoubleValue();
                const auto like = juce::StringArray::fromTokens (parts[5], ",", "");
                for (int i = 0; i < like.size() && i < 4; ++i)
                    h.likeness[(size_t) i] = (float) like[i].getDoubleValue();
            }
            if (parts.size() >= 8)
            {
                h.source = parts[6].getDoubleValue();
                h.added = parts[7] == "1";
            }
            if (parts.size() >= 9)
                h.soundVelocity = (float) parts[8].getDoubleValue();
            pendingHits.push_back (h);
        }
    }
}

// Each beat remembers its own fixes, in a small file of its own, so opening
// another beat and coming back loses nothing.
juce::File DrumRebuilderProcessor::beatMemoryFile (const juce::File& audio)
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Drum Rebuilder").getChildFile ("beats")
        .getChildFile (juce::String::toHexString (audio.getFullPathName().hashCode64()) + ".xml");
}

void DrumRebuilderProcessor::saveBeatMemory()
{
    if (loaded == nullptr)
        return;
    const auto file = beatMemoryFile (loaded->file);
    if (file.getParentDirectory().createDirectory())
        if (auto xml = makeBeatState().createXml())
            xml->writeTo (file);
}

void DrumRebuilderProcessor::scheduleBeatMemory()
{
    const int generation = ++memoryGeneration;
    juce::WeakReference<DrumRebuilderProcessor> self (this);
    juce::Timer::callAfterDelay (400, [self, generation]
    {
        if (self != nullptr && generation == self->memoryGeneration)
            self->saveBeatMemory();
    });
}

void DrumRebuilderProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree state ("DrumRebuilder");
    state.setProperty ("metronome", metronome.isEnabled(), nullptr);
    if (browseFolder != juce::File())
        state.setProperty ("browseFolder", browseFolder.getFullPathName(), nullptr);
    if (loaded != nullptr)
    {
        state.setProperty ("file", loaded->file.getFullPathName(), nullptr);
        state.addChild (makeBeatState(), -1, nullptr);
        saveBeatMemory();
    }
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void DrumRebuilderProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr)
        return;

    const auto state = juce::ValueTree::fromXml (*xml);
    metronome.setEnabled (state.getProperty ("metronome", false));
    const juce::File folder (state.getProperty ("browseFolder").toString());
    if (folder.isDirectory())
        browseFolder = folder;

    const juce::File file (state.getProperty ("file").toString());
    if (file.existsAsFile())
    {
        // Older states kept the beat's fixes on the top level.
        auto beat = state.getChildWithName ("Beat");
        readBeatState (beat.isValid() ? beat : state);
        pendingFile = file;
        juce::WeakReference<DrumRebuilderProcessor> self (this);
        juce::MessageManager::callAsync ([self, file]
        {
            if (self != nullptr)
                self->loadFile (file);
        });
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumRebuilderProcessor();
}
