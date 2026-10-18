#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

DrumRebuilderProcessor::DrumRebuilderProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{}

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

// The recording and the preview at the device's rate, keeping the play
// positions.
void DrumRebuilderProcessor::installPlayback()
{
    if (preparedRate <= 0.0)
        return;

    for (auto [audio, p] : { std::pair { loaded.get(), &player }, std::pair { preview.get(), &previewPlayer } })
    {
        if (audio == nullptr)
            continue;
        const double at = p->getPositionSeconds();
        p->setSource (std::make_unique<juce::AudioBuffer<float>> (
            drb::resample (audio->audio, audio->sampleRate, preparedRate)));
        p->setPositionSeconds (at);
    }
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
            p->checks = {};
            const bool restoring = p->loaded->file == p->pendingFile;

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

            if (std::abs (rate - p->preparedRate) < 0.5 || p->preparedRate <= 0.0)
                p->player.setSource (std::move (result->playback));
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
            h.time = to;
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
        // As loud as the other hits at this onset, or a medium hit.
        float velocity = 0.8f;
        for (const auto& h : hits)
            if (std::abs (h.time - t) < 1.0e-9)
                velocity = h.velocity;
        drb::Hit hit { t, instrument, velocity, 1.0f, true };
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

int DrumRebuilderProcessor::addHit (double seconds, drb::Instrument instrument)
{
    auto& onsets = analysis.onsets;
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
    const drb::Hit hit { std::max (0.0, seconds), instrument, velocity, 1.0f, true };
    const auto it = hits.insert (std::upper_bound (hits.begin(), hits.end(), hit, earlier), hit);
    const int index = (int) (it - hits.begin());
    rebuildOnsets();
    hitsChanged();
    return index;
}

void DrumRebuilderProcessor::setHitVelocity (int hitIndex, float velocity)
{
    if (! juce::isPositiveAndBelow (hitIndex, (int) analysis.hits.size()))
        return;
    beginHitEdit();
    analysis.hits[(size_t) hitIndex].velocity = juce::jlimit (0.05f, 1.0f, velocity);
    analysis.hits[(size_t) hitIndex].manual = true;
    hitsChanged();
}

void DrumRebuilderProcessor::quantizeHits (double divisionBeats, double strength)
{
    if (analysis.grid.bpm <= 0.0 || analysis.hits.empty())
        return;
    beginHitEdit();
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
    sendChangeMessage();
}

void DrumRebuilderProcessor::setRegion (juce::Range<double> r)
{
    region = r;
    player.setLoop (player.isLooping(), region);
    sendChangeMessage();
}

void DrumRebuilderProcessor::setLooping (bool on)
{
    player.setLoop (on, region);
    sendChangeMessage();
}

void DrumRebuilderProcessor::auditionInstrument (drb::Instrument instrument)
{
    if (loaded == nullptr)
        return;

    // The recording, silent except from each hit of this instrument until
    // the next hit of anything (at most 0.4 s), with short fades.
    const auto& src = loaded->audio;
    const double sr = loaded->sampleRate;
    juce::AudioBuffer<float> gated (src.getNumChannels(), src.getNumSamples());
    gated.clear();

    const auto& onsets = analysis.onsets;
    const int fadeIn = (int) (0.002 * sr), fadeOut = (int) (0.012 * sr);
    for (const auto& h : analysis.hits)
    {
        if (h.instrument != instrument)
            continue;
        const auto next = std::upper_bound (onsets.begin(), onsets.end(), h.time + 1.0e-6,
                                            [] (double t, const drb::Onset& o) { return t < o.time; });
        const double end = std::min (h.time + 0.4, next != onsets.end() ? next->time - 0.002 : h.time + 0.4);
        const int a = std::max (0, (int) ((h.time - 0.002) * sr));
        const int b = std::min (src.getNumSamples(), (int) (end * sr));
        for (int i = a; i < b; ++i)
        {
            const float g = std::min ({ 1.0f, (float) (i - a) / (float) fadeIn, (float) (b - i) / (float) fadeOut });
            for (int ch = 0; ch < src.getNumChannels(); ++ch)
                gated.setSample (ch, i, src.getSample (ch, i) * g);
        }
    }

    player.pause();
    previewPlayer.stop();
    auditionInstrument_ = instrument;
    const double rate = preparedRate > 0.0 ? preparedRate : 44100.0;
    auditionPlayer.stop();
    auditionPlayer.setSource (std::make_unique<juce::AudioBuffer<float>> (drb::resample (gated, sr, rate)));
    auditionPlayer.setLoop (true, {});
    auditionPlayer.setPositionSeconds (std::max (0.0, (onsets.empty() ? 0.0 : onsets.front().time) - 0.05));
    auditionPlayer.play();
    sendChangeMessage();
}

void DrumRebuilderProcessor::stopAudition()
{
    auditionPlayer.stop();
    sendChangeMessage();
}

void DrumRebuilderProcessor::setCheck (drb::Instrument i, int state)
{
    checks[(size_t) i] = state;
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
    metronome.setGrid (g.bpm, g.firstDownbeat, g.timeSignature.beatsPerBar);
}

void DrumRebuilderProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree state ("DrumRebuilder");
    state.setProperty ("metronome", metronome.isEnabled(), nullptr);
    if (loaded != nullptr && tempoManual)
    {
        state.setProperty ("bpm", analysis.grid.bpm, nullptr);
        state.setProperty ("bar1", analysis.grid.firstDownbeat, nullptr);
    }
    if (loaded != nullptr)
    {
        juce::StringArray c;
        for (int v : checks)
            c.add (juce::String (v));
        state.setProperty ("checks", c.joinIntoString (""), nullptr);
    }
    if (loaded != nullptr && onsetsEdited)
    {
        juce::StringArray items;   // time:instrument:velocity
        for (const auto& h : analysis.hits)
            items.add (juce::String (h.time, 6) + ":" + juce::String ((int) h.instrument) + ":" + juce::String (h.velocity, 3));
        state.setProperty ("hitList", items.joinIntoString (" "), nullptr);
    }
    if (loaded != nullptr)
        state.setProperty ("file", loaded->file.getFullPathName(), nullptr);
    if (browseFolder != juce::File())
        state.setProperty ("browseFolder", browseFolder.getFullPathName(), nullptr);
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
        pendingFile = file;
        pendingBpm = state.getProperty ("bpm", 0.0);
        pendingDownbeat = state.getProperty ("bar1", 0.0);
        const auto c = state.getProperty ("checks").toString();
        for (int i = 0; i < drb::numInstruments; ++i)
            pendingChecks[(size_t) i] = i < c.length() ? juce::jlimit (0, 2, (int) (c[i] - '0')) : 0;
        pendingHits.clear();
        for (const auto& item : juce::StringArray::fromTokens (state.getProperty ("hitList").toString(), " ", ""))
        {
            const auto parts = juce::StringArray::fromTokens (item, ":", "");
            const int instrument = parts[1].getIntValue();
            if (parts.size() == 3 && juce::isPositiveAndBelow (instrument, drb::numInstruments))
                pendingHits.push_back ({ parts[0].getDoubleValue(), (drb::Instrument) instrument,
                                         (float) parts[2].getDoubleValue(), 1.0f, true });
        }
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
