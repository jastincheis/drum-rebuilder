#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

DrumRebuilderProcessor::DrumRebuilderProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{}

DrumRebuilderProcessor::~DrumRebuilderProcessor() = default;

void DrumRebuilderProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    player.prepare (sampleRate);
    previewPlayer.prepare (sampleRate);
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
            const bool restoring = p->loaded->file == p->pendingFile;

            // The hits as they were fixed by hand last time.
            if (restoring && ! p->pendingOnsets.empty())
            {
                auto& onsets = p->analysis.onsets;
                if (onsets.size() != p->pendingOnsets.size())
                    onsets.assign (p->pendingOnsets.size(), drb::Onset { 0.0, 1.0f });
                for (size_t i = 0; i < onsets.size(); ++i)
                    onsets[i].time = p->pendingOnsets[i];
                p->onsetsEdited = true;
            }

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
            p->pendingOnsets.clear();
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
    onsets[(size_t) index].time = juce::jlimit (lo, std::max (lo, hi), seconds);
    onsetsEdited = true;
    sendChangeMessage();
    return onsets[(size_t) index].time;
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
    if (loaded != nullptr && onsetsEdited)
    {
        juce::StringArray times;
        for (const auto& o : analysis.onsets)
            times.add (juce::String (o.time, 6));
        state.setProperty ("hits", times.joinIntoString (" "), nullptr);
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
        pendingOnsets.clear();
        for (const auto& t : juce::StringArray::fromTokens (state.getProperty ("hits").toString(), " ", ""))
            pendingOnsets.push_back (t.getDoubleValue());
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
