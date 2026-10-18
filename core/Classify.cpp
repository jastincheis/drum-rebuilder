#include "Classify.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace drb
{
namespace
{
// Third-octave-ish bands, Hz.
constexpr std::array<double, 27> bandEdges { 30, 45, 60, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800,
                                             1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
                                             10000, 12500, 16000 };
constexpr int numBands = (int) bandEdges.size() - 1;
using Bands = std::array<double, (size_t) numBands>;

// The three sounds we separate. Open and closed hi-hat share one, told apart
// afterwards by how long they ring.
enum Part { kick, snare, hat, numParts };

double bandCentre (int b) { return std::sqrt (bandEdges[(size_t) b] * bandEdges[(size_t) b + 1]); }

// Rough spectra of each sound, magnitude per band before scaling to the
// recording. They only need to start the separation on the right foot.
double prior (Part p, double hz)
{
    switch (p)
    {
        case kick:  return hz < 130.0 ? 1.0 : hz < 300.0 ? 0.3 : hz < 5000.0 ? 0.06 : 0.03;
        case snare: return hz < 120.0 ? 0.05 : hz < 400.0 ? 0.6 : hz < 1000.0 ? 0.5 : hz < 6000.0 ? 1.0 : hz < 12000.0 ? 0.4 : 0.2;
        case hat:   return hz < 2000.0 ? 0.01 : hz < 5000.0 ? 0.12 : hz < 8000.0 ? 0.6 : 1.0;
        case numParts: break;
    }
    return 0.0;
}

// Power per band of mono[start, start + size), with 2 ms tapers.
class BandMeter
{
public:
    explicit BandMeter (double sr)
        : sampleRate (sr), order (sr > 60000.0 ? 12 : 11), size (1 << order), fft (order),
          window ((size_t) size), data ((size_t) size * 2)
    {
        const int taper = std::max (1, (int) (0.002 * sr));
        for (int i = 0; i < size; ++i)
        {
            const int edge = std::min (i, size - 1 - i);
            window[(size_t) i] = edge >= taper ? 1.0f
                               : 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) edge / (float) taper);
        }
    }

    int length() const { return size; }

    Bands measure (const std::vector<float>& mono, int start)
    {
        std::fill (data.begin(), data.end(), 0.0f);
        for (int i = 0; i < size; ++i)
        {
            const int s = start + i;
            if (s >= 0 && s < (int) mono.size())
                data[(size_t) i] = mono[(size_t) s] * window[(size_t) i];
        }
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        Bands b {};
        const double binHz = sampleRate / size;
        for (int k = 1; k < size / 2; ++k)
        {
            const double hz = k * binHz;
            if (hz < bandEdges.front() || hz >= bandEdges.back())
                continue;
            const auto it = std::upper_bound (bandEdges.begin(), bandEdges.end(), hz);
            const auto band = (size_t) (it - bandEdges.begin() - 1);
            b[band] += (double) data[(size_t) k] * data[(size_t) k];
        }
        return b;
    }

private:
    double sampleRate;
    int order, size;
    juce::dsp::FFT fft;
    std::vector<float> window, data;
};

// High-frequency power of mono over [from, to), for how long a hi-hat rings.
double highPower (const std::vector<float>& mono, int from, int to)
{
    from = std::max (2, from);
    to = std::min ((int) mono.size(), to);
    if (to <= from)
        return 0.0;
    double sum = 0.0;
    for (int i = from; i < to; ++i)
    {
        // Second difference: a steep high-pass, enough to keep the kick out.
        const double d = mono[(size_t) i] - 2.0 * mono[(size_t) i - 1] + mono[(size_t) i - 2];
        sum += d * d;
    }
    return sum / (to - from);
}

double percentile (std::vector<double> v, double q)
{
    if (v.empty())
        return 0.0;
    const auto k = (size_t) std::clamp ((int) std::lround (q * (double) (v.size() - 1)), 0, (int) v.size() - 1);
    std::nth_element (v.begin(), v.begin() + (long) k, v.end());
    return v[k];
}
} // namespace

std::vector<Hit> classifyHits (const std::vector<Onset>& onsets, const std::vector<float>& mono, double sampleRate)
{
    std::vector<Hit> hits;
    const int n = (int) onsets.size();
    if (n == 0)
        return hits;

    // 1. What each onset added to the sound: the spectrum just after the
    // attack minus the one just before (the tails of earlier hits).
    BandMeter meter (sampleRate);
    const int len = meter.length(), gap = (int) (0.001 * sampleRate);
    std::vector<Bands> added ((size_t) n);
    for (int j = 0; j < n; ++j)
    {
        const int at = (int) std::lround (onsets[(size_t) j].time * sampleRate);
        const auto after = meter.measure (mono, at - gap);
        const auto before = meter.measure (mono, at - gap - len);
        for (int b = 0; b < numBands; ++b)
            added[(size_t) j][(size_t) b] = std::sqrt (std::max (0.0, after[(size_t) b] - before[(size_t) b]));
    }

    // 2. Split every onset into kick + snare + hat: V ~ W H with the band
    // shapes W starting from the priors and adapting to this recording
    // (non-negative matrix factorisation, multiplicative updates).
    Bands scale {};
    for (int b = 0; b < numBands; ++b)
    {
        for (const auto& a : added)
            scale[(size_t) b] += a[(size_t) b];
        scale[(size_t) b] = scale[(size_t) b] / n + 1.0e-12;
    }

    std::array<Bands, numParts> W {};
    for (int p = 0; p < numParts; ++p)
    {
        double norm = 0.0;
        for (int b = 0; b < numBands; ++b)
        {
            W[(size_t) p][(size_t) b] = prior ((Part) p, bandCentre (b)) * scale[(size_t) b];
            norm += W[(size_t) p][(size_t) b] * W[(size_t) p][(size_t) b];
        }
        for (auto& w : W[(size_t) p])
            w /= std::sqrt (norm) + 1.0e-30;
    }

    std::vector<std::array<double, numParts>> H ((size_t) n);
    for (int j = 0; j < n; ++j)
        for (int p = 0; p < numParts; ++p)
        {
            double dot = 0.0;
            for (int b = 0; b < numBands; ++b)
                dot += W[(size_t) p][(size_t) b] * added[(size_t) j][(size_t) b];
            H[(size_t) j][(size_t) p] = std::max (1.0e-9, dot);
        }

    for (int iteration = 0; iteration < 200; ++iteration)
    {
        const bool updateShapes = iteration < 150;   // the last rounds settle H only

        // H <- H * (W'V) / (W'W H)
        std::array<std::array<double, numParts>, numParts> WtW {};
        for (int p = 0; p < numParts; ++p)
            for (int q = 0; q < numParts; ++q)
                for (int b = 0; b < numBands; ++b)
                    WtW[(size_t) p][(size_t) q] += W[(size_t) p][(size_t) b] * W[(size_t) q][(size_t) b];

        for (int j = 0; j < n; ++j)
        {
            auto& h = H[(size_t) j];
            for (int p = 0; p < numParts; ++p)
            {
                double num = 0.0, den = 0.0;
                for (int b = 0; b < numBands; ++b)
                    num += W[(size_t) p][(size_t) b] * added[(size_t) j][(size_t) b];
                for (int q = 0; q < numParts; ++q)
                    den += WtW[(size_t) p][(size_t) q] * h[(size_t) q];
                h[(size_t) p] *= num / (den + 1.0e-30);
            }
        }

        if (! updateShapes)
            continue;

        // W <- W * (V H') / (W H H'), then unit length.
        std::array<std::array<double, numParts>, numParts> HHt {};
        for (const auto& h : H)
            for (int p = 0; p < numParts; ++p)
                for (int q = 0; q < numParts; ++q)
                    HHt[(size_t) p][(size_t) q] += h[(size_t) p] * h[(size_t) q];

        for (int p = 0; p < numParts; ++p)
        {
            double norm = 0.0;
            for (int b = 0; b < numBands; ++b)
            {
                double num = 0.0, den = 0.0;
                for (int j = 0; j < n; ++j)
                    num += added[(size_t) j][(size_t) b] * H[(size_t) j][(size_t) p];
                for (int q = 0; q < numParts; ++q)
                    den += W[(size_t) q][(size_t) b] * HHt[(size_t) q][(size_t) p];
                auto& w = W[(size_t) p][(size_t) b];
                w *= num / (den + 1.0e-30);
                norm += w * w;
            }
            norm = std::sqrt (norm) + 1.0e-30;
            for (auto& w : W[(size_t) p])
                w /= norm;
            for (auto& h : H)
                h[(size_t) p] *= norm;
        }
    }

    // 3. A part is in an onset when it is loud enough against that part's
    // loud hits in the recording and makes a fair share of the onset.
    std::array<double, numParts> reference {};
    for (int p = 0; p < numParts; ++p)
    {
        std::vector<double> v;
        for (const auto& h : H)
            v.push_back (h[(size_t) p]);
        reference[(size_t) p] = percentile (v, 0.95) + 1.0e-30;
    }

    // A kick always brings some "snare" (its own mids) and a snare some
    // "kick" (its body): that steady leak, measured on the hits where one of
    // them clearly leads, is taken away before deciding. A real kick + snare
    // stands well above it. (Not done for the hi-hat: in many beats every
    // kick has a hat, and that is no leak.)
    auto leak = [&] (int from, int to)
    {
        std::vector<double> ratios;
        for (const auto& h : H)
            if (h[(size_t) from] / reference[(size_t) from] > 0.3
                && h[(size_t) from] / reference[(size_t) from] > 2.0 * h[(size_t) to] / reference[(size_t) to])
                ratios.push_back (h[(size_t) to] / h[(size_t) from]);
        return ratios.size() >= 4 ? percentile (ratios, 0.75) : 0.0;
    };
    const double kickToSnare = leak (kick, snare), snareToKick = leak (snare, kick);
    for (auto& h : H)
    {
        const double k = h[kick], s = h[snare];
        h[snare] = std::max (0.0, s - kickToSnare * k);
        h[kick] = std::max (0.0, k - snareToKick * s);
    }

    // A hi-hat is often much quieter than the kick or snare it is played
    // with, so it needs a smaller share of the onset.
    constexpr double minLevel[] { 0.12, 0.12, 0.10 };
    constexpr double minShare[] { 0.08, 0.12, 0.015 };
    const Instrument partInstrument[] { Instrument::Kick, Instrument::Snare, Instrument::HiHatClosed };

    // How much each onset sounds like each instrument; the hi-hat's share
    // is split between closed and open in step 4.
    std::vector<std::array<float, numInstruments>> like ((size_t) n);
    std::vector<int> onsetOf;   // per hit
    for (int j = 0; j < n; ++j)
    {
        auto& l = like[(size_t) j];
        l[(size_t) Instrument::Kick] = (float) std::clamp (H[(size_t) j][kick] / reference[kick], 0.0, 1.0);
        l[(size_t) Instrument::Snare] = (float) std::clamp (H[(size_t) j][snare] / reference[snare], 0.0, 1.0);
        l[(size_t) Instrument::HiHatClosed] = (float) std::clamp (H[(size_t) j][hat] / reference[hat], 0.0, 1.0);
        l[(size_t) Instrument::HiHatOpen] = 0.3f * l[(size_t) Instrument::HiHatClosed];   // can't tell yet
    }

    for (int j = 0; j < n; ++j)
    {
        const auto& h = H[(size_t) j];
        double total = 0.0;
        for (double v : h)
            total += v;

        int best = 0;
        for (int p = 1; p < numParts; ++p)
            if (h[(size_t) p] / reference[(size_t) p] > h[(size_t) best] / reference[(size_t) best])
                best = p;

        // Nothing new sounded here (a bump in an earlier hit's tail).
        if (h[(size_t) best] / reference[(size_t) best] < 0.01)
            continue;

        for (int p = 0; p < numParts; ++p)
        {
            const double level = h[(size_t) p] / reference[(size_t) p];
            const double share = h[(size_t) p] / (total + 1.0e-30);
            if (p != best && (level < minLevel[p] || share < minShare[p]))
                continue;

            Hit hit;
            hit.time = onsets[(size_t) j].time;
            hit.instrument = partInstrument[p];
            hit.velocity = (float) std::clamp (level, 0.05, 1.0);
            hit.confidence = (float) std::clamp (share * 1.5, 0.1, 1.0);
            hits.push_back (hit);
            onsetOf.push_back (j);
        }
    }

    // 4. Open hi-hat: still ringing 40-70 ms after the attack (a closed one
    // is gone by then). Only measurable when nothing else starts meanwhile
    // and no snare rings along.
    for (size_t i = 0; i < hits.size(); ++i)
    {
        auto& hit = hits[i];
        if (hit.instrument != Instrument::HiHatClosed)
            continue;

        bool withSnare = false;
        for (const auto& other : hits)
            if (std::abs (other.time - hit.time) < 1.0e-9 && other.instrument == Instrument::Snare)
                withSnare = true;

        const auto next = std::upper_bound (onsets.begin(), onsets.end(), hit.time + 1.0e-6,
                                            [] (double t, const Onset& o) { return t < o.time; });
        const double nextTime = next != onsets.end() ? next->time : 1.0e9;
        if (withSnare || nextTime < hit.time + 0.072)
            continue;

        const int at = (int) std::lround (hit.time * sampleRate);
        const double early = highPower (mono, at, at + (int) (0.015 * sampleRate));
        const double late = highPower (mono, at + (int) (0.040 * sampleRate), at + (int) (0.070 * sampleRate));
        if (early <= 0.0)
            continue;
        if (late / early > 0.1)   // within 10 dB
            hit.instrument = Instrument::HiHatOpen;

        // Openness 0 at -20 dB or less, 1 at -10 dB or more.
        const float open = (float) std::clamp ((10.0 * std::log10 (std::max (1.0e-9, late / early)) + 20.0) / 10.0, 0.0, 1.0);
        auto& l = like[(size_t) onsetOf[i]];
        const float hatLevel = l[(size_t) Instrument::HiHatClosed];
        l[(size_t) Instrument::HiHatOpen] = hatLevel * open;
        l[(size_t) Instrument::HiHatClosed] = hatLevel * (1.0f - 0.8f * open);
    }

    for (size_t i = 0; i < hits.size(); ++i)
        hits[i].likeness = like[(size_t) onsetOf[i]];

    // Velocity per instrument after the open / closed split.
    for (auto inst : { Instrument::HiHatClosed, Instrument::HiHatOpen })
    {
        float loudest = 0.0f;
        for (const auto& h : hits)
            if (h.instrument == inst)
                loudest = std::max (loudest, h.velocity);
        if (loudest > 0.0f)
            for (auto& h : hits)
                if (h.instrument == inst)
                    h.velocity = std::clamp (h.velocity / loudest, 0.05f, 1.0f);
    }

    return hits;
}

} // namespace drb
