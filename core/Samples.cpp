#include "Samples.h"
#include "AudioFile.h"
#include "SampleSort.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <mutex>
#include <cmath>
#include <map>

namespace drb
{

struct LazyAudio
{
    std::mutex lock;
    bool read = false;
    juce::AudioBuffer<float> audio;
};

const juce::AudioBuffer<float>& Sample::getAudio() const
{
    if (lazy == nullptr)
        return audio;
    const std::scoped_lock hold (lazy->lock);
    if (! lazy->read)
    {
        lazy->read = true;
        juce::String error;
        if (auto loaded = loadAudioFile (file, error, 10.0))   // drum hits: 10 s is plenty
        {
            lazy->audio = std::move (loaded->audio);
            if (std::abs (loaded->sampleRate - sampleRate) > 0.5 && loaded->sampleRate > 0.0)
                lazy->audio = resample (lazy->audio, loaded->sampleRate, sampleRate);   // the file changed since
        }
    }
    return lazy->audio;
}

Sample Sample::withOwnAudio() const
{
    Sample copy = *this;
    if (lazy != nullptr)
    {
        copy.audio = getAudio();
        copy.lazy.reset();
    }
    return copy;
}

namespace
{
// One line of the index: what rescan() learnt about a file.
struct IndexEntry
{
    juce::int64 modified = 0, bytes = 0;
    double sampleRate = 0.0, seconds = 0.0;
    int length = 0, channels = 0;
    float peak = 0.0f, pitch = 0.0f;
};

juce::String indexKey (int folder, const juce::File& f)   { return juce::String (folder) + "\t" + f.getFullPathName(); }

std::map<juce::String, IndexEntry> readIndex (const juce::File& file)
{
    std::map<juce::String, IndexEntry> index;
    juce::StringArray lines;
    lines.addLines (file.loadFileAsString());
    if (lines.isEmpty() || lines[0] != "drum-rebuilder-index 1")
        return index;
    for (int i = 1; i < lines.size(); ++i)
    {
        const auto parts = juce::StringArray::fromTokens (lines[i], "\t", "");
        if (parts.size() != 10)
            continue;
        IndexEntry e;
        e.modified = parts[2].getLargeIntValue();
        e.bytes = parts[3].getLargeIntValue();
        e.sampleRate = parts[4].getDoubleValue();
        e.length = parts[5].getIntValue();
        e.channels = parts[6].getIntValue();
        e.peak = parts[7].getFloatValue();
        e.pitch = parts[8].getFloatValue();
        e.seconds = parts[9].getDoubleValue();
        index[parts[0] + "\t" + parts[1]] = e;
    }
    return index;
}

void writeIndex (const juce::File& file, const std::array<std::vector<SamplePtr>, numFolders>& lists)
{
    juce::MemoryOutputStream text;
    text << "drum-rebuilder-index 1\n";
    for (int folder = 0; folder < numFolders; ++folder)
        for (const auto& s : lists[(size_t) folder])
            text << folder << "\t" << s->file.getFullPathName() << "\t" << s->modified.toMilliseconds() << "\t" << s->bytes
                 << "\t" << juce::String (s->sampleRate, 1) << "\t" << s->numSamples() << "\t" << s->numChannels()
                 << "\t" << juce::String (s->peak, 6) << "\t" << juce::String (s->pitch, 3) << "\t" << juce::String (s->seconds, 4) << "\n";
    juce::TemporaryFile temp (file);
    if (temp.getFile().replaceWithData (text.getData(), text.getDataSize()))
        temp.overwriteTargetFileWithTemporary();
}
} // namespace


float estimatePitch (const juce::AudioBuffer<float>& audio, double sampleRate, Instrument instrument)
{
    // The first 150 ms, mono, zero-padded for fine frequency steps.
    constexpr int order = 14, size = 1 << order;
    const int n = std::min (audio.getNumSamples(), std::min (size, (int) (0.15 * sampleRate)));
    if (n < 64)
        return 0.0f;

    std::vector<float> data ((size_t) size * 2, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        float v = 0.0f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            v += audio.getSample (ch, i);
        const float window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (n - 1));
        data[(size_t) i] = v * window;
    }
    juce::dsp::FFT fft (order);
    fft.performFrequencyOnlyForwardTransform (data.data(), true);
    const double binHz = sampleRate / size;

    const bool cymbal = instrument == Instrument::HiHatClosed || instrument == Instrument::HiHatOpen
                     || instrument == Instrument::Crash || instrument == Instrument::Ride
                     || instrument == Instrument::Clap;
    if (cymbal)
    {
        // Brightness: spectral centroid from 1 to 16 kHz.
        double sum = 0.0, weighted = 0.0;
        for (int k = (int) (1000.0 / binHz); k < std::min (size / 2, (int) (16000.0 / binHz)); ++k)
        {
            sum += data[(size_t) k];
            weighted += data[(size_t) k] * k * binHz;
        }
        return sum > 0.0 ? (float) (weighted / sum) : 0.0f;
    }

    // The strongest partial in the drum's range, refined between bins.
    const double lo = instrument == Instrument::Kick ? 30.0 : instrument == Instrument::Snare ? 120.0 : 60.0;
    const double hi = instrument == Instrument::Kick ? 200.0 : instrument == Instrument::Snare ? 500.0 : 400.0;
    int best = -1;
    for (int k = (int) (lo / binHz); k <= (int) (hi / binHz) && k + 1 < size / 2; ++k)
        if (best < 0 || data[(size_t) k] > data[(size_t) best])
            best = k;
    if (best < 1)
        return 0.0f;

    const double a = data[(size_t) best - 1], b = data[(size_t) best], c = data[(size_t) best + 1];
    const double denom = a - 2.0 * b + c;
    const double offset = std::abs (denom) > 1.0e-12 ? 0.5 * (a - c) / denom : 0.0;
    return (float) ((best + std::clamp (offset, -0.5, 0.5)) * binHz);
}

juce::String folderName (int folder)
{
    if (folder == unsortedFolder)
        return "Unsorted";
    if (folder == loopsFolder)
        return "Loops";
    return instrumentName (allInstruments[(size_t) juce::jlimit (0, numInstruments - 1, folder)]);
}

namespace
{
double fileSeconds (const juce::File& f)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    return reader != nullptr && reader->sampleRate > 0.0 ? (double) reader->lengthInSamples / reader->sampleRate : -1.0;
}
} // namespace

SampleLibrary::SampleLibrary (const juce::File& r) : root (r) {}

juce::File SampleLibrary::defaultRoot()
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Drum Rebuilder").getChildFile ("Samples");
}

void SampleLibrary::setRoot (const juce::File& r)
{
    root = r;
    for (auto& list : lists)
        list.clear();
    missing = 0;
}

juce::File SampleLibrary::folderAt (int folder) const
{
    return root.getChildFile (folderName (folder));
}

juce::StringArray SampleLibrary::readLinks (int folder) const
{
    auto lines = juce::StringArray::fromLines (folderAt (folder).getChildFile (linksFileName()).loadFileAsString());
    lines.trim();
    lines.removeEmptyStrings();
    return lines;
}

bool SampleLibrary::writeLinks (int folder, const juce::StringArray& links) const
{
    const auto dir = folderAt (folder);
    if (! dir.createDirectory())
        return false;
    const auto file = dir.getChildFile (linksFileName());
    if (links.isEmpty())
        return file.deleteFile();
    return file.replaceWithText (links.joinIntoString ("\n") + "\n");
}

bool SampleLibrary::rescan (const std::function<bool (int, int)>& progress)
{
    // What we have now, to keep the sounds that didn't change.
    std::map<juce::String, SamplePtr> before;
    for (const auto& list : lists)
        for (const auto& s : list)
            before[s->file.getFullPathName()] = s;
    // And what was learnt about them the last times (from the disk).
    const auto indexFile = root.getChildFile (indexFileName());
    const auto index = readIndex (indexFile);

    // First every folder's files, so the progress knows the total.
    std::array<juce::Array<juce::File>, numFolders> found;
    int stillThere = 0, total = 0;
    for (int folder = 0; folder < numFolders; ++folder)
    {
        const auto dir = folderAt (folder);
        if (! dir.isDirectory())
            continue;

        auto files = dir.findChildFiles (juce::File::findFiles, false, supportedWildcard());
        for (const auto& path : readLinks (folder))
        {
            const juce::File f (path);
            if (f.existsAsFile())
                files.add (f);
            else
                ++stillThere;   // kept in links.txt: it may come back (a disk plugged in again)
        }

        // The user's order first, then the rest by name.
        auto byName = [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; };
        std::sort (files.begin(), files.end(), byName);
        const auto order = juce::StringArray::fromLines (dir.getChildFile (orderFileName()).loadFileAsString());
        std::stable_sort (files.begin(), files.end(), [&] (const juce::File& a, const juce::File& b)
        {
            auto rank = [&] (const juce::File& f)
            {
                const int i = order.indexOf (f.getFileName());
                return i < 0 ? order.size() : i;
            };
            return rank (a) < rank (b);
        });
        total += files.size();
        found[(size_t) folder] = std::move (files);
    }

    std::array<std::vector<SamplePtr>, numFolders> fresh;
    int done = 0, fromIndex = 0;   // the index is written again only when something changed
    for (int folder = 0; folder < numFolders; ++folder)
    {
        auto& list = fresh[(size_t) folder];
        const auto dir = folderAt (folder);
        for (const auto& f : found[(size_t) folder])
        {
            if (progress != nullptr && ! progress (done, total))
                return false;
            ++done;

            const auto modified = f.getLastModificationTime();
            const auto bytes = f.getSize();
            const bool linked = ! f.isAChildOf (dir);
            if (const auto it = before.find (f.getFullPathName()); it != before.end() && it->second->modified == modified
                                                                   && it->second->bytes == bytes && it->second->linked == linked
                                                                   && (folder == loopsFolder) == it->second->loop)
            {
                if (index.count (indexKey (folder, f)) > 0)
                    ++fromIndex;
                list.push_back (it->second);
                continue;
            }

            auto s = std::make_shared<Sample>();
            s->file = f;
            s->linked = linked;
            s->modified = modified;
            s->bytes = bytes;
            s->loop = folder == loopsFolder;
            const auto known = index.find (indexKey (folder, f));
            if (known != index.end() && known->second.modified == modified.toMilliseconds() && known->second.bytes == bytes
                && (s->loop || (known->second.sampleRate > 0.0 && known->second.length > 0)))
            {
                // Known from the index: nothing to read now.
                ++fromIndex;
                const auto& e = known->second;
                s->seconds = e.seconds;
                if (! s->loop)
                {
                    s->sampleRate = e.sampleRate;
                    s->length = e.length;
                    s->channels = e.channels;
                    s->peak = e.peak;
                    s->pitch = e.pitch;
                    s->lazy = std::make_shared<LazyAudio>();
                }
            }
            else if (s->loop)
            {
                // Loops play straight from the file; only how long they are.
                s->seconds = fileSeconds (f);
                if (s->seconds < 0.0)
                    continue;
            }
            else
            {
                // New or changed: read once to learn its level and pitch; the
                // sound itself loads again when it is used.
                juce::String error;
                auto loaded = loadAudioFile (f, error, 10.0);   // drum hits: 10 s is plenty
                if (! loaded)
                    continue;
                const auto& audio = loaded->audio;
                s->sampleRate = loaded->sampleRate;
                s->length = audio.getNumSamples();
                s->channels = audio.getNumChannels();
                s->seconds = s->length / s->sampleRate;
                s->peak = audio.getMagnitude (0, s->length);
                if (folder < numInstruments)
                    s->pitch = estimatePitch (audio, s->sampleRate, allInstruments[(size_t) folder]);
                s->lazy = std::make_shared<LazyAudio>();
            }
            const auto name = f.getFileNameWithoutExtension();
            s->original = ! linked && name.contains (originalsTag());
            if (s->original)
                s->fromBeat = name.upToFirstOccurrenceOf (originalsTag(), false, false);
            list.push_back (std::move (s));
        }
    }
    if (progress != nullptr)
        progress (total, total);

    lists = std::move (fresh);
    missing = stillThere;
    if (root.isDirectory() && (fromIndex != total || (int) index.size() != fromIndex))
        writeIndex (indexFile, lists);
    return true;
}

int SampleLibrary::saveOriginals (const juce::String& beatName, const juce::AudioBuffer<float>& audio, double sampleRate,
                                  const std::vector<Cut>& cuts)
{
    const auto prefix = juce::File::createLegalFileName (beatName) + originalsTag();

    // This beat's earlier originals go first, in every folder.
    for (auto inst : allInstruments)
        for (const auto& f : folderFor (inst).findChildFiles (juce::File::findFiles, false, "*.wav"))
            if (f.getFileName().startsWith (prefix))
                f.deleteFile();

    std::array<int, numInstruments> numbers {};
    int written = 0;
    const int fadeIn = (int) (0.002 * sampleRate), fadeOut = (int) (0.010 * sampleRate);
    for (const auto& c : cuts)
    {
        const int a = std::max (0, (int) std::floor (c.in * sampleRate));
        const int b = std::min (audio.getNumSamples(), (int) std::ceil (c.out * sampleRate));
        if (b - a < fadeIn + fadeOut)
            continue;

        juce::AudioBuffer<float> slice (audio.getNumChannels(), b - a);
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            slice.copyFrom (ch, 0, audio, ch, a, b - a);
        slice.applyGainRamp (0, fadeIn, 0.0f, 1.0f);
        slice.applyGainRamp (b - a - fadeOut, fadeOut, 1.0f, 0.0f);

        const int number = ++numbers[(size_t) c.instrument];
        const auto file = folderFor (c.instrument).getChildFile (prefix + juce::String (number).paddedLeft ('0', 2) + ".wav");
        if (writeWav (file, slice, sampleRate))
            ++written;
    }

    rescan();
    return written;
}

void SampleLibrary::moveSample (int folder, int from, int to)
{
    auto& list = lists[(size_t) folder];
    if (! juce::isPositiveAndBelow (from, (int) list.size()))
        return;
    to = juce::jlimit (0, (int) list.size() - 1, to);
    if (from == to)
        return;
    auto s = list[(size_t) from];
    list.erase (list.begin() + from);
    list.insert (list.begin() + to, std::move (s));
    saveOrder (folder);
}

void SampleLibrary::saveOrder (int folder) const
{
    juce::StringArray names;
    for (const auto& s : lists[(size_t) folder])
        names.add (s->file.getFileName());
    folderAt (folder).getChildFile (orderFileName()).replaceWithText (names.joinIntoString ("\n") + "\n");
}

int SampleLibrary::addFiles (int folder, const juce::Array<juce::File>& files)
{
    const auto dir = folderAt (folder);
    if (! dir.createDirectory())
        return 0;

    int copied = 0;
    for (const auto& f : files)
        if (f.existsAsFile() && isSupported (f) && f.copyFileTo (dir.getNonexistentChildFile (f.getFileNameWithoutExtension(),
                                                                                             f.getFileExtension(), false)))
            ++copied;
    rescan();
    return copied;
}

bool SampleLibrary::remove (int folder, const SamplePtr& s)
{
    if (s == nullptr || ! juce::isPositiveAndBelow (folder, numFolders))
        return false;
    bool done = false;
    if (s->linked)
    {
        auto links = readLinks (folder);
        const int before = links.size();
        links.removeString (s->file.getFullPathName());
        done = links.size() < before && writeLinks (folder, links);
    }
    else
        done = s->file.isAChildOf (folderAt (folder)) && s->file.moveToTrash();
    rescan();
    return done;
}

bool SampleLibrary::moveToFolder (int from, const SamplePtr& s, int to)
{
    if (s == nullptr || from == to || ! juce::isPositiveAndBelow (from, numFolders) || ! juce::isPositiveAndBelow (to, numFolders))
        return false;
    bool done = false;
    if (s->linked)
    {
        auto links = readLinks (from);
        links.removeString (s->file.getFullPathName());
        auto there = readLinks (to);
        there.addIfNotAlreadyThere (s->file.getFullPathName());
        done = writeLinks (to, there) && writeLinks (from, links);
    }
    else
    {
        const auto dir = folderAt (to);
        done = dir.createDirectory()
            && s->file.moveFileTo (dir.getNonexistentChildFile (s->file.getFileNameWithoutExtension(), s->file.getFileExtension(), false));
    }
    rescan();
    return done;
}

int SampleLibrary::ImportSummary::total() const
{
    int n = 0;
    for (auto a : added)
        n += a;
    return n;
}

SampleLibrary::ImportSummary SampleLibrary::importFolder (const juce::File& dir, const std::function<bool (int, int)>& progress) const
{
    ImportSummary summary;
    const auto files = dir.findChildFiles (juce::File::findFiles, true, supportedWildcard());
    summary.found = files.size();

    // What the library has already: its own files and every link.
    std::array<juce::StringArray, numFolders> links;
    juce::StringArray known;
    for (int folder = 0; folder < numFolders; ++folder)
    {
        links[(size_t) folder] = readLinks (folder);
        known.addArray (links[(size_t) folder]);
    }

    for (int i = 0; i < files.size(); ++i)
    {
        if (progress != nullptr && ! progress (i, files.size()))
        {
            summary.stopped = true;
            break;
        }
        const auto& f = files.getReference (i);
        if (f.isAChildOf (root) || known.contains (f.getFullPathName()))
        {
            ++summary.skipped;
            continue;
        }
        juce::String error;
        auto loaded = loadAudioFile (f, error, 12.0);
        if (! loaded)
        {
            ++summary.skipped;
            continue;
        }
        const auto r = sortSound (f, dir, loaded->audio, loaded->sampleRate);
        const int folder = r.as == SortedAs::Loop ? loopsFolder : r.as == SortedAs::Unsorted ? unsortedFolder : (int) r.instrument;
        links[(size_t) folder].add (f.getFullPathName());
        known.add (f.getFullPathName());
        ++summary.added[(size_t) folder];
    }

    for (int folder = 0; folder < numFolders; ++folder)
        if (summary.added[(size_t) folder] > 0)
            writeLinks (folder, links[(size_t) folder]);
    if (progress != nullptr && ! summary.stopped)
        progress (files.size(), files.size());
    return summary;
}

} // namespace drb
