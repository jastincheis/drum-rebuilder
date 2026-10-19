#include "WaveformView.h"
#include "Focus.h"
#include "Theme.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr int rulerHeight = 18;
constexpr int scrollHeight = 12;
constexpr int markerHeight = 34;   // the triangle and up to two instrument names
constexpr double minViewSeconds = 0.05;
} // namespace

WaveformView::WaveformView (DrumRebuilderProcessor& p) : processor (p)
{
    scrollBar.setAutoHide (false);
    scrollBar.addListener (this);
    addAndMakeVisible (scrollBar);
    refresh();
    startTimerHz (30);
}

WaveformView::~WaveformView()
{
    scrollBar.removeListener (this);
}

juce::Rectangle<int> WaveformView::waveArea() const
{
    return getLocalBounds().withTrimmedTop (rulerHeight).withTrimmedBottom (scrollHeight + markerHeight);
}

double WaveformView::xToSeconds (float x) const
{
    const auto a = waveArea();
    return viewStart + (x - (float) a.getX()) / (float) std::max (1, a.getWidth()) * (viewEnd - viewStart);
}

float WaveformView::secondsToX (double s) const
{
    const auto a = waveArea();
    return (float) a.getX() + (float) ((s - viewStart) / (viewEnd - viewStart) * a.getWidth());
}

void WaveformView::refresh()
{
    // A new recording: rebuild the overview and show all of it.
    if (processor.getLoadedId() != shownId)
    {
        shownId = processor.getLoadedId();
        selection.clear();
        buildPeaks();
        zoomToFit();
    }
    repaint();
}

void WaveformView::buildPeaks()
{
    peaks.clear();
    const auto* l = processor.getLoaded();
    if (l == nullptr)
    {
        duration = 0.0;
        return;
    }

    sampleRate = l->sampleRate;
    const auto& a = l->audio;
    const int n = a.getNumSamples(), channels = a.getNumChannels();
    duration = n / sampleRate;
    peakBlock = std::max (16, juce::roundToInt (sampleRate / 1000.0));   // ~1 ms

    peaks.reserve ((size_t) (n / peakBlock + 1));
    for (int start = 0; start < n; start += peakBlock)
    {
        const int len = std::min (peakBlock, n - start);
        float lo = 0.0f, hi = 0.0f;
        for (int i = 0; i < len; ++i)
        {
            float v = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                v += a.getSample (ch, start + i);
            v /= (float) channels;
            lo = std::min (lo, v);
            hi = std::max (hi, v);
        }
        peaks.emplace_back (lo, hi);
    }
}

void WaveformView::setView (double start, double end)
{
    const double total = std::max (duration, minViewSeconds);
    double len = juce::jlimit (minViewSeconds, total, end - start);
    start = juce::jlimit (0.0, total - len, start);
    viewStart = start;
    viewEnd = start + len;

    scrollBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    scrollBar.setCurrentRange (viewStart, len, juce::dontSendNotification);
    repaint();
}

void WaveformView::zoomToFit()
{
    setView (0.0, std::max (duration, minViewSeconds));
}

void WaveformView::zoomBy (double factor, double around)
{
    const double len = (viewEnd - viewStart) / factor;
    const double rel = (around - viewStart) / (viewEnd - viewStart);
    setView (around - rel * len, around - rel * len + len);
}

double WaveformView::zoomCentre() const
{
    const double pos = processor.getPlayheadSeconds();
    return pos >= viewStart && pos <= viewEnd ? pos : centreSeconds();
}

void WaveformView::scrollToShow (double seconds)
{
    if (seconds < viewStart || seconds > viewEnd)
    {
        const double len = viewEnd - viewStart;
        setView (seconds - len * 0.05, seconds + len * 0.95);
    }
}

void WaveformView::jumpToHit (int direction)
{
    const auto& onsets = processor.getAnalysis().onsets;
    auto& solo = processor.getAuditionPlayer();
    const bool soloPlaying = processor.isSoloPlaying();
    const double pos = soloPlaying ? solo.getPositionSeconds() : processor.getPlayheadSeconds();

    // On the Instruments tab with instruments on solo: only their hits.
    const bool onlySolo = instrumentMode && ! focus.empty();
    const auto wanted = [&] (size_t i)
    {
        if (! onlySolo)
            return true;
        const auto present = processor.getInstrumentsAt ((int) i);
        return std::any_of (present.begin(), present.end(), [this] (drb::Instrument inst) { return focused (inst); });
    };

    // A little slack, so pressing again while playing goes on to the next hit.
    std::optional<double> target;
    for (size_t i = 0; i < onsets.size(); ++i)
    {
        const double t = onsets[i].time;
        if (direction > 0 && t > pos + 0.005 && wanted (i))
        {
            target = t;
            break;
        }
        if (direction < 0 && t < pos - 0.005 && wanted (i))
            target = t;
    }
    if (! target)
    {
        if (onlySolo)
            return;   // no more of them that way: stay
        target = direction > 0 ? duration : 0.0;
    }

    processor.setPlayheadSeconds (*target);
    if (soloPlaying)
        solo.setPositionSeconds (*target);
    scrollToShow (*target);
    repaint();
}

void WaveformView::resized()
{
    scrollBar.setBounds (getLocalBounds().removeFromBottom (scrollHeight));
}

void WaveformView::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    const auto area = waveArea();
    g.setColour (theme::panel);
    g.fillRect (area);

    const auto* l = processor.getLoaded();
    if (l == nullptr)
    {
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (16.0f));
        g.drawText (processor.isBusy() ? processor.getStatus()
                                       : juce::String ("Drop a drums-only recording here, or press Open"),
                    area, juce::Justification::centred);
        return;
    }

    const auto& analysis = processor.getAnalysis();
    const auto& grid = analysis.grid;

    // Bar and beat lines, bar numbers in the ruler.
    if (grid.bpm > 0.0)
    {
        const double beat = grid.beatSeconds();
        const int perBar = grid.timeSignature.beatsPerBar;
        const double pxPerBeat = area.getWidth() * beat / (viewEnd - viewStart);
        const int firstK = (int) std::floor ((viewStart - grid.firstDownbeat) / beat) - 1;
        const int lastK = (int) std::ceil ((viewEnd - grid.firstDownbeat) / beat) + 1;

        g.setFont (juce::FontOptions (11.0f));
        for (int k = firstK; k <= lastK; ++k)
        {
            const float x = secondsToX (grid.firstDownbeat + k * beat);
            const bool isBar = ((k % perBar) + perBar) % perBar == 0;
            if (! isBar && pxPerBeat < 6.0)
                continue;

            g.setColour (isBar ? theme::barLine : theme::beatLine);
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());

            const int barNumber = (int) std::floor ((double) k / perBar) + 1;
            if (isBar && barNumber >= 2 && pxPerBeat * perBar > 24.0)
            {
                g.setColour (theme::textDim);
                g.drawText (juce::String (barNumber), (int) x + 3, 0, 40, rulerHeight, juce::Justification::centredLeft);
            }
        }
    }

    // Colour: on the Instruments tab each instrument hit colours the
    // waveform between its IN and OUT; two instruments at once = top half
    // one, bottom half the other. With instruments on solo only theirs keep
    // a colour.
    struct Slice { double in, out; juce::Colour colour; int row; };
    std::vector<Slice> slices;
    const bool tint = instrumentMode;
    if (tint)
    {
        const auto& onsetList = analysis.onsets;
        for (size_t i = 0; i < onsetList.size(); ++i)
        {
            auto present = processor.getInstrumentsAt ((int) i);
            std::sort (present.begin(), present.end());
            for (size_t k = 0; k < present.size(); ++k)
            {
                if (! focus.empty() && ! focused (present[k]))
                    continue;
                const int hit = processor.findHit (onsetList[i].time, present[k]);
                if (hit < 0)
                    continue;
                const auto [in, out] = processor.getSlice (hit);
                if (out >= viewStart && in <= viewEnd)
                    slices.push_back ({ in, out, theme::instrument (present[k]), (int) k });
            }
        }
    }
    const auto plain = tint ? theme::wave.withAlpha (focus.empty() ? 0.35f : 0.22f) : theme::wave;

    // The waveform, one min/max line per pixel.
    const double secondsPerPx = (viewEnd - viewStart) / std::max (1, area.getWidth());
    const float mid = (float) area.getCentreY(), halfH = area.getHeight() * 0.48f;
    for (int x = 0; x < area.getWidth(); ++x)
    {
        const double t0 = viewStart + x * secondsPerPx, t1 = t0 + secondsPerPx;
        const int b0 = std::max (0, (int) (t0 * sampleRate / peakBlock));
        const int b1 = std::min ((int) peaks.size(), (int) std::ceil (t1 * sampleRate / peakBlock));
        if (b0 >= b1)
            continue;
        float lo = 0.0f, hi = 0.0f;
        for (int b = b0; b < b1; ++b)
        {
            lo = std::min (lo, peaks[(size_t) b].first);
            hi = std::max (hi, peaks[(size_t) b].second);
        }

        auto top = plain, bottom = plain;
        if (tint)
        {
            // The latest-starting slice on each row wins.
            const Slice* rowTop = nullptr;
            const Slice* rowBottom = nullptr;
            for (const auto& sl : slices)
                if (sl.in < t1 && sl.out > t0)
                {
                    auto*& slot = sl.row == 0 ? rowTop : rowBottom;
                    if (slot == nullptr || sl.in > slot->in)
                        slot = &sl;
                }
            if (rowTop == nullptr)
                std::swap (rowTop, rowBottom);
            if (rowTop != nullptr)
            {
                top = rowTop->colour;
                bottom = rowBottom != nullptr ? rowBottom->colour : rowTop->colour;
            }
        }
        const float yTop = mid - hi * halfH, yBottom = mid - lo * halfH + 1.0f;
        g.setColour (top);
        g.drawVerticalLine (area.getX() + x, yTop, mid + 0.5f);
        g.setColour (bottom);
        g.drawVerticalLine (area.getX() + x, mid + 0.5f, yBottom);
    }

    // Hits: a thin line in the colour of the (first) instrument, a marker
    // under the waveform and the instruments' names under it ("?" when the
    // analysis is unsure). Stronger hits are brighter.
    float strongest = 0.0f;
    for (const auto& o : analysis.onsets)
        strongest = std::max (strongest, o.strength);

    const auto& onsets = analysis.onsets;
    const auto& hits = analysis.hits;
    size_t h = 0;
    for (size_t i = 0; i < onsets.size(); ++i)
    {
        const auto& o = onsets[i];
        while (h < hits.size() && hits[h].time < o.time - 1.0e-9)
            ++h;
        if (o.time < viewStart || o.time > viewEnd)
            continue;

        std::vector<const drb::Hit*> here;
        for (size_t k = h; k < hits.size() && hits[k].time < o.time + 1.0e-9; ++k)
            here.push_back (&hits[k]);
        std::sort (here.begin(), here.end(), [] (auto* a, auto* b) { return a->instrument < b->instrument; });

        const float x = secondsToX (o.time);
        const bool grabbed = grab == Grab::hit && (int) i == grabbedHit;
        // Checking an instrument: its hits stand out (thicker, its colour,
        // its name first), every other hit fades.
        const bool isFocus = std::any_of (here.begin(), here.end(), [this] (auto* hit) { return focused (hit->instrument); });
        const bool faded = ! focus.empty() && ! isFocus;
        if (isFocus)
            std::stable_partition (here.begin(), here.end(), [this] (auto* hit) { return focused (hit->instrument); });

        const float a = grabbed ? 1.0f : faded ? 0.12f : isFocus ? 1.0f : 0.45f + 0.55f * (strongest > 0.0f ? o.strength / strongest : 1.0f);
        const auto colour = here.empty() ? theme::onset : theme::instrument (here.front()->instrument);

        g.setColour (colour.withAlpha (isFocus ? 0.85f : a * 0.4f));
        if (isFocus)
            g.fillRect (juce::Rectangle<float> (x - 1.0f, (float) area.getY(), 2.0f, (float) area.getHeight()));
        else
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
        g.setColour (grabbed ? juce::Colours::white : colour.withAlpha (a));
        juce::Path tri;
        const float y = (float) area.getBottom() + 1.0f;
        const float half = isFocus ? 6.0f : 4.0f;
        tri.addTriangle (x, y, x - half, y + 7.0f, x + half, y + 7.0f);
        g.fillPath (tri);

        // Names: full when the neighbours leave room, one letter when not.
        // (On the Instruments tab they are on the waveform, see below.)
        if (instrumentMode)
            continue;
        const float before = i > 0 ? x - secondsToX (onsets[i - 1].time) : 1000.0f;
        const float after = i + 1 < onsets.size() ? secondsToX (onsets[i + 1].time) - x : 1000.0f;
        const float room = std::min (before, after);
        if (room < 9.0f)
            continue;
        const bool tiny = room < 46.0f;
        g.setFont (juce::FontOptions (tiny ? 10.5f : 11.0f, juce::Font::bold));
        for (size_t k = 0; k < here.size() && k < 2; ++k)
        {
            auto name = theme::shortName (here[k]->instrument, tiny);
            if (k == 1 && here.size() > 2)
                name << "+";
            if (here[k]->confidence < 0.5f && ! here[k]->manual)
                name << "?";
            const bool dim = faded || (isFocus && ! focused (here[k]->instrument));
            g.setColour (theme::instrument (here[k]->instrument).withAlpha (grabbed ? 1.0f : dim ? 0.22f : 0.95f));
            g.drawText (name, juce::Rectangle<float> (x - 30.0f, y + 8.0f + 12.0f * (float) k, 60.0f, 12.0f),
                        juce::Justification::centred, false);
        }
    }

    // Instruments tab: for each instrument of each hit a bar from its IN to
    // its OUT, its name at the start; rows one under the other. Click the
    // name: what it really is. Drag the bar's ends: IN and OUT.
    pills.clear();
    if (instrumentMode)
    {
        for (size_t i = 0; i < onsets.size(); ++i)
        {
            const double t = onsets[i].time;
            if (t < viewStart - 1.0 || t > viewEnd)
                continue;
            auto present = processor.getInstrumentsAt ((int) i);
            std::sort (present.begin(), present.end());
            const float x = secondsToX (t);
            const float room = i + 1 < onsets.size() ? secondsToX (onsets[i + 1].time) - x : 1000.0f;
            if (room < 13.0f)
                continue;

            for (size_t k = 0; k < present.size(); ++k)
            {
                const auto inst = present[k];
                const int hit = processor.findHit (t, inst);
                if (hit < 0)
                    continue;
                const auto [in, out] = processor.getSlice (hit);
                const float y = (float) area.getY() + 6.0f + 23.0f * (float) k;
                const juce::Rectangle<float> bar (secondsToX (in), y, std::max (4.0f, secondsToX (out) - secondsToX (in)), 20.0f);
                const bool dim = ! focus.empty() && ! focused (inst);
                const auto colour = theme::instrument (inst);

                g.setColour (colour.withAlpha (dim ? 0.06f : 0.22f));
                g.fillRoundedRectangle (bar, 4.0f);
                g.setColour (colour.withAlpha (dim ? 0.2f : 0.8f));
                g.drawRoundedRectangle (bar.reduced (0.5f), 4.0f, 1.0f);
                // The OUT grip.
                g.fillRect (juce::Rectangle<float> (bar.getRight() - 3.0f, bar.getY() + 4.0f, 2.0f, bar.getHeight() - 8.0f));

                juce::String name = drb::instrumentName (inst);
                g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
                float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 12.0f;
                const float space = std::max (room, bar.getRight() - x) - 8.0f;
                if (w > space)
                {
                    name = theme::shortName (inst, true);
                    w = 15.0f;
                }
                const juce::Rectangle<float> label (x + 2.0f, y + 1.0f, w, 18.0f);
                g.setColour (colour.withAlpha (dim ? 0.25f : 0.95f));
                g.fillRoundedRectangle (label, 4.0f);
                g.setColour (juce::Colours::black.withAlpha (dim ? 0.5f : 0.85f));
                g.drawText (name, label, juce::Justification::centred, false);
                if (isSelected (t, inst))
                {
                    g.setColour (juce::Colours::white);
                    g.drawRoundedRectangle (label.expanded (1.5f), 5.0f, 2.0f);
                    g.drawRoundedRectangle (bar.expanded (1.0f), 5.0f, 1.0f);
                }
                pills.push_back ({ label.toNearestInt(), bar.toNearestInt(), (int) i, hit, inst });
            }
        }
    }

    if (lassoing && ! lasso.isEmpty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (lasso);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawRect (lasso, 1);
    }

    // Region.
    const auto region = processor.getRegion();
    if (! region.isEmpty())
    {
        const float x0 = secondsToX (region.getStart()), x1 = secondsToX (region.getEnd());
        g.setColour (theme::selection);
        g.fillRect (juce::Rectangle<float> (x0, (float) area.getY(), x1 - x0, (float) area.getHeight()));
        g.setColour (theme::accent.withAlpha (0.7f));
        g.drawVerticalLine ((int) x0, (float) area.getY(), (float) area.getBottom());
        g.drawVerticalLine ((int) x1, (float) area.getY(), (float) area.getBottom());
    }

    // Bar 1: a yellow line with a flag in the ruler.
    if (grid.bpm > 0.0 && grid.firstDownbeat >= viewStart && grid.firstDownbeat <= viewEnd)
    {
        const float x = secondsToX (grid.firstDownbeat);
        g.setColour (theme::startFlag.withAlpha (0.8f));
        g.fillRect (juce::Rectangle<float> (x - 0.75f, 0.0f, 1.5f, (float) area.getBottom()));

        juce::Path flag;
        flag.addRectangle (x, 1.0f, 48.0f, (float) rulerHeight - 3.0f);
        g.setColour (theme::startFlag);
        g.fillPath (flag);
        g.setColour (juce::Colours::black);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText ("1 START", juce::Rectangle<float> (x + 3.0f, 1.0f, 45.0f, (float) rulerHeight - 3.0f),
                    juce::Justification::centredLeft);
    }

    // Playhead (the checked instrument's, while listening to it).
    const bool auditioning = processor.isSoloPlaying();
    const double pos = auditioning ? processor.getAuditionPlayer().getPositionSeconds() : processor.getPlayheadSeconds();
    if (pos >= viewStart && pos <= viewEnd)
    {
        const auto solo = processor.firstSolo();
        g.setColour (auditioning && solo ? theme::instrument (*solo) : theme::accent);
        g.fillRect (juce::Rectangle<float> (secondsToX (pos) - 0.75f, 0.0f, 1.5f, (float) area.getBottom()));
    }
}

// What is under the mouse on the Instruments tab: a bar's OUT or IN end,
// its name, or nothing. An OUT wins over the IN of the next bar it touches
// (by default each bar ends where the next hit starts); later bars (drawn on
// top) win otherwise.
WaveformView::PillPart WaveformView::findPill (juce::Point<int> p, const Pill*& found) const
{
    const auto onRow = [p] (const Pill& pill) { return p.y >= pill.bar.getY() && p.y < pill.bar.getBottom(); };

    for (auto it = pills.rbegin(); it != pills.rend(); ++it)
        if (onRow (*it) && std::abs (p.x - it->bar.getRight()) <= 5)
        {
            found = &*it;
            return PillPart::out;
        }
    for (auto it = pills.rbegin(); it != pills.rend(); ++it)
        if (onRow (*it) && p.x >= it->bar.getX() - 3 && p.x <= it->bar.getX() + 1)
        {
            found = &*it;
            return PillPart::in;
        }
    for (auto it = pills.rbegin(); it != pills.rend(); ++it)
        if (onRow (*it) && (it->label.contains (p) || it->bar.contains (p)))
        {
            found = &*it;
            return PillPart::label;
        }
    found = nullptr;
    return PillPart::none;
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    pillPressed = false;
    slicePart = PillPart::none;
    lassoing = false;
    selectClick = false;
    if (const Pill* pill = nullptr; instrumentMode)
    {
        const auto part = findPill (e.getPosition(), pill);
        // Ctrl (Cmd) or Shift + click on a label: in / out of the selection.
        if (part == PillPart::label && ! e.mods.isPopupMenu() && (e.mods.isCommandDown() || e.mods.isShiftDown()))
        {
            grab = Grab::none;
            dragging = false;
            toggleSelected (processor.getAnalysis().onsets[(size_t) pill->onset].time, pill->instrument);
            selectClick = true;
            return;
        }
        // Shift (or Ctrl) + drag on the waveform: a box selecting labels.
        if (part == PillPart::none && ! e.mods.isPopupMenu() && (e.mods.isShiftDown() || e.mods.isCommandDown())
            && waveArea().contains (e.getPosition()))
        {
            grab = Grab::none;
            dragging = false;
            lassoing = true;
            lasso = {};
            return;
        }
        if (part != PillPart::none)
        {
            grab = Grab::none;
            dragging = false;
            sliceHit = pill->hit;
            pillHit = pill->onset;
            pillInstrument = pill->instrument;
            if (part == PillPart::label)
                pillPressed = true;
            else
                slicePart = part;
            return;
        }
    }

    if (e.mods.isPopupMenu())
    {
        showMenu (e);
        return;
    }

    // Keep Space (play / pause) working after clicking here.
    if (auto* editor = findParentComponentOfClass<juce::AudioProcessorEditor>())
        editor->grabKeyboardFocus();

    dragging = false;
    dragAnchor = juce::jlimit (0.0, duration, xToSeconds ((float) e.x));
    grab = processor.getLoaded() != nullptr ? findGrab (e.getPosition(), grabbedHit) : Grab::none;
}

// The flag is grabbed in the ruler, a hit by its marker under the waveform.
WaveformView::Grab WaveformView::findGrab (juce::Point<int> p, int& hitIndex) const
{
    const auto& analysis = processor.getAnalysis();
    const auto area = waveArea();

    if (analysis.grid.bpm > 0.0 && p.y < area.getY())
    {
        const float x = secondsToX (analysis.grid.firstDownbeat);
        if (p.x >= x - 6.0f && p.x <= x + 50.0f)
            return Grab::flag;
    }

    if (p.y >= area.getBottom() && p.y < area.getBottom() + markerHeight + 2)
    {
        float best = 7.0f;
        hitIndex = -1;
        for (size_t i = 0; i < analysis.onsets.size(); ++i)
        {
            const float d = std::abs (secondsToX (analysis.onsets[i].time) - (float) p.x);
            if (d < best)
            {
                best = d;
                hitIndex = (int) i;
            }
        }
        if (hitIndex >= 0)
            return Grab::hit;
    }
    return Grab::none;
}

// The nearest hit within 8 pixels, or `seconds` itself.
double WaveformView::snapToHit (double seconds, int ignoreIndex) const
{
    const auto& onsets = processor.getAnalysis().onsets;
    double best = seconds;
    float distance = 8.0f;
    for (size_t i = 0; i < onsets.size(); ++i)
    {
        const float d = std::abs (secondsToX (onsets[i].time) - secondsToX (seconds));
        if ((int) i != ignoreIndex && d < distance)
        {
            distance = d;
            best = onsets[i].time;
        }
    }
    return best;
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    if (const Pill* pill = nullptr; instrumentMode)
    {
        const auto part = findPill (e.getPosition(), pill);
        if (part != PillPart::none)
        {
            setMouseCursor (part == PillPart::label ? juce::MouseCursor::PointingHandCursor
                                                    : juce::MouseCursor::LeftRightResizeCursor);
            return;
        }
    }
    int hit = -1;
    setMouseCursor (processor.getLoaded() != nullptr && findGrab (e.getPosition(), hit) != Grab::none
                        ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::NormalCursor);
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (pillPressed || selectClick)
        return;
    if (lassoing)
    {
        lasso = juce::Rectangle<int> (e.getMouseDownPosition(), e.getPosition()).getIntersection (waveArea());
        repaint();
        return;
    }
    if (slicePart != PillPart::none)
    {
        // Drag IN or OUT; the other end stays.
        if (! dragging)
            processor.beginHitEdit();
        dragging = true;
        const auto [in, out] = processor.getSlice (sliceHit);
        const double t = juce::jlimit (0.0, duration, xToSeconds ((float) e.x));
        if (slicePart == PillPart::in)
            processor.setSlice (sliceHit, std::min (t, out - 0.005), out);
        else
            processor.setSlice (sliceHit, in, std::max (t, in + 0.005));
        repaint();
        return;
    }
    if (e.mods.isPopupMenu() || processor.getLoaded() == nullptr || (! dragging && e.getDistanceFromDragStart() < 4))
        return;

    if (! dragging && grab == Grab::hit)
        processor.beginHitEdit();   // the whole drag is one undo step
    dragging = true;
    const double t = juce::jlimit (0.0, duration, xToSeconds ((float) e.x));

    if (grab == Grab::flag)
    {
        // Snaps to the hits, Alt (or Option) = free.
        processor.setFirstDownbeat (e.mods.isAltDown() ? t : snapToHit (t, -1), false);
        repaint();
        return;
    }
    if (grab == Grab::hit)
    {
        processor.moveOnset (grabbedHit, t);
        repaint();
        return;
    }

    processor.setRegion ({ std::min (dragAnchor, t), std::max (dragAnchor, t) });
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (selectClick)
    {
        selectClick = false;
        return;
    }
    if (pillPressed)
    {
        pillPressed = false;
        // On one of several selected: they all change. Else just this one.
        pruneSelection();
        const auto& onsets = processor.getAnalysis().onsets;
        if (juce::isPositiveAndBelow (pillHit, (int) onsets.size()) && selection.size() > 1
            && isSelected (onsets[(size_t) pillHit].time, pillInstrument))
            showSelectionMenu();
        else
        {
            clearSelection();
            showPillMenu (pillHit, pillInstrument);
        }
        return;
    }
    if (lassoing)
    {
        lassoing = false;
        const auto& onsets = processor.getAnalysis().onsets;
        for (const auto& pill : pills)
            if ((lasso.intersects (pill.label) || lasso.intersects (pill.bar)) && juce::isPositiveAndBelow (pill.onset, (int) onsets.size())
                && ! isSelected (onsets[(size_t) pill.onset].time, pill.instrument))
                selection.push_back ({ onsets[(size_t) pill.onset].time, pill.instrument });
        lasso = {};
        repaint();
        return;
    }
    if (slicePart != PillPart::none)
    {
        slicePart = PillPart::none;
        dragging = false;
        return;
    }
    if (e.mods.isPopupMenu() || processor.getLoaded() == nullptr)
        return;

    if (grab != Grab::none)
    {
        // Moved a flag or a hit: nothing else. A plain click on the flag
        // plays from there; on a hit's marker it asks what the hit really is.
        if (! dragging && grab == Grab::flag)
            processor.setPlayheadSeconds (processor.getAnalysis().grid.firstDownbeat);
        else if (! dragging)
        {
            const int index = grabbedHit;
            grab = Grab::none;
            showRenameMenu (index);
        }
    }
    else if (dragging)
    {
        // Play from the start of the new region.
        processor.setPlayheadSeconds (processor.getRegion().getStart());
    }
    else
    {
        // A click clears the region (and the selected labels) and moves the
        // playhead there.
        clearSelection();
        processor.setRegion ({});
        processor.setPlayheadSeconds (dragAnchor);
    }
    dragging = false;
    grab = Grab::none;
    repaint();
}

void WaveformView::showMenu (const juce::MouseEvent& e)
{
    if (processor.getLoaded() == nullptr)
        return;

    const double t = juce::jlimit (0.0, duration, xToSeconds ((float) e.x));
    const auto& onsets = processor.getAnalysis().onsets;
    const bool hasTempo = processor.getAnalysis().grid.bpm > 0.0;
    const auto& choices = drb::allInstruments;

    // The hit under the mouse, if any.
    int index = -1;
    float best = 7.0f;
    for (size_t i = 0; i < onsets.size(); ++i)
        if (const float d = std::abs (secondsToX (onsets[i].time) - (float) e.x); d < best)
        {
            best = d;
            index = (int) i;
        }

    juce::PopupMenu menu;
    juce::WeakReference<juce::Component> self (this);
    if (index >= 0)
    {
        const auto present = processor.getInstrumentsAt (index);
        menu.addSectionHeader ("This hit is:");
        for (auto inst : choices)
        {
            const bool on = std::find (present.begin(), present.end(), inst) != present.end();
            menu.addItem (drb::instrumentName (inst), true, on, [this, self, index, inst]
            {
                if (self != nullptr)
                    processor.toggleInstrument (index, inst);
            });
        }
        menu.addSeparator();
        menu.addItem ("Delete this hit", [this, self, index]
        {
            if (self != nullptr)
                processor.deleteHit (index);
        });
        if (hasTempo)
        {
            const double at = onsets[(size_t) index].time;
            menu.addItem ("Bar 1 starts on this hit", [this, self, at]
            {
                if (self != nullptr)
                    processor.setFirstDownbeat (at, false);
            });
        }
    }
    else
    {
        // A sound the analysis missed: a marker here, on the sound's start.
        menu.addSectionHeader ("Add a marker here - it is a:");
        for (auto inst : choices)
            menu.addItem (drb::instrumentName (inst), [this, self, t, inst]
            {
                if (self != nullptr)
                    processor.addHit (t, inst);
            });
        menu.addSeparator();
        if (hasTempo)
            menu.addItem ("Bar 1 starts here", [this, self, t]
            {
                if (self != nullptr)
                    processor.setFirstDownbeat (t);
            });
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), focus::afterMenu (this));
}

bool WaveformView::isSelected (double time, drb::Instrument inst) const
{
    return std::any_of (selection.begin(), selection.end(),
                        [&] (const Selected& s) { return s.second == inst && std::abs (s.first - time) < 1.0e-9; });
}

void WaveformView::toggleSelected (double time, drb::Instrument inst)
{
    const auto it = std::find_if (selection.begin(), selection.end(),
                                  [&] (const Selected& s) { return s.second == inst && std::abs (s.first - time) < 1.0e-9; });
    if (it != selection.end())
        selection.erase (it);
    else
        selection.push_back ({ time, inst });
    repaint();
}

bool WaveformView::clearSelection()
{
    if (selection.empty())
        return false;
    selection.clear();
    repaint();
    return true;
}

void WaveformView::pruneSelection()
{
    const auto& hits = processor.getAnalysis().hits;
    selection.erase (std::remove_if (selection.begin(), selection.end(), [&] (const Selected& s)
                     {
                         return std::none_of (hits.begin(), hits.end(), [&] (const drb::Hit& h)
                                              { return h.instrument == s.second && std::abs (h.time - s.first) < 1.0e-9; });
                     }),
                     selection.end());
}

// A click on one of several selected labels: what they all really are.
void WaveformView::showSelectionMenu()
{
    juce::WeakReference<juce::Component> self (this);
    juce::PopupMenu menu;
    menu.addSectionHeader ("These " + juce::String ((int) selection.size()) + " are all really:");
    for (auto to : drb::allInstruments)
        menu.addItem (drb::instrumentName (to), [this, self, to]
        {
            if (self == nullptr)
                return;
            processor.replaceInstruments (selection, to);
            selection.clear();
        });
    menu.addSeparator();
    menu.addItem ("Not hits at all (delete them)", [this, self]
    {
        if (self == nullptr)
            return;
        processor.removeInstruments (selection);
        selection.clear();
    });
    menu.addItem ("Clear the selection", [this, self]
    {
        if (self != nullptr)
            clearSelection();
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), focus::afterMenu (this));
}

// A click on an instrument's label: what it really is (the likeliest first),
// or that it isn't there at all.
void WaveformView::showPillMenu (int index, drb::Instrument from)
{
    juce::WeakReference<juce::Component> self (this);
    const auto present = processor.getInstrumentsAt (index);
    juce::PopupMenu menu;
    menu.addSectionHeader ("This " + juce::String (drb::instrumentName (from)) + " is really:");

    const auto suggested = processor.suggestInstead (index, from);
    if (suggested)
    {
        menu.addItem (juce::String (drb::instrumentName (*suggested)) + "   (suggested)", [this, self, index, from, to = *suggested]
        {
            if (self != nullptr)
                processor.replaceInstrument (index, from, to);
        });
        menu.addSeparator();
    }
    for (auto to : drb::allInstruments)
        if (to != from && to != suggested)
            menu.addItem (drb::instrumentName (to), [this, self, index, from, to]
            {
                if (self != nullptr)
                    processor.replaceInstrument (index, from, to);
            });

    menu.addSeparator();
    juce::PopupMenu also;
    for (auto inst : drb::allInstruments)
        if (std::find (present.begin(), present.end(), inst) == present.end())
            also.addItem (drb::instrumentName (inst), [this, self, index, inst]
            {
                if (self != nullptr)
                    processor.toggleInstrument (index, inst);
            });
    menu.addSubMenu ("Another instrument here too", also);
    menu.addItem (present.size() > 1 ? "Not a " + juce::String (drb::instrumentName (from)) + " (remove it)"
                                     : juce::String ("Not a hit at all (delete it)"),
                  [this, self, index, from]
                  {
                      if (self != nullptr)
                          processor.toggleInstrument (index, from);
                  });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), focus::afterMenu (this));
}

// "This is the Snare. It is really: ..." for a click on a hit's marker:
// every instrument, the one it sounds most like first.
void WaveformView::showRenameMenu (int index)
{
    const auto present = processor.getInstrumentsAt (index);
    if (present.empty())
        return;

    juce::WeakReference<juce::Component> self (this);
    auto choicesFor = [this, self, index] (drb::Instrument from)
    {
        juce::PopupMenu m;
        const auto suggested = processor.suggestInstead (index, from);
        if (suggested)
        {
            m.addItem (juce::String (drb::instrumentName (*suggested)) + "   (suggested)", [this, self, index, from, to = *suggested]
            {
                if (self != nullptr)
                    processor.replaceInstrument (index, from, to);
            });
            m.addSeparator();
        }
        for (auto to : drb::allInstruments)
            if (to != from && to != suggested)
                m.addItem (drb::instrumentName (to), [this, self, index, from, to]
                {
                    if (self != nullptr)
                        processor.replaceInstrument (index, from, to);
                });
        return m;
    };

    // Several instruments here: the solo one, or ask which.
    auto from = present.front();
    int soloHere = 0;
    for (auto inst : present)
        if (focused (inst))
        {
            from = inst;
            ++soloHere;
        }

    juce::PopupMenu menu;
    if (present.size() == 1 || soloHere == 1)
    {
        menu.addSectionHeader ("This is the " + juce::String (drb::instrumentName (from)) + ". It is really:");
        const auto m = choicesFor (from);
        for (juce::PopupMenu::MenuItemIterator it (m); it.next();)
            menu.addItem (it.getItem());
    }
    else
    {
        menu.addSectionHeader ("Change which one?");
        for (auto inst : present)
            menu.addSubMenu (juce::String (drb::instrumentName (inst)) + " is really", choicesFor (inst));
    }
    menu.addSeparator();
    menu.addItem ("Delete this hit", [this, self, index]
    {
        if (self != nullptr)
            processor.deleteHit (index);
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), focus::afterMenu (this));
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const Pill* pill = nullptr; instrumentMode && findPill (e.getPosition(), pill) != PillPart::none)
        return;
    if (e.mods.isPopupMenu() || processor.getLoaded() == nullptr)
        return;

    // From the hit at or before the mouse up to the next hit (or the ends of
    // the recording), starting a hair early so the attack is not cut.
    const double t = juce::jlimit (0.0, duration, xToSeconds ((float) e.x));
    double start = 0.0, end = duration;
    for (const auto& o : processor.getAnalysis().onsets)
    {
        if (o.time <= t)
            start = o.time;
        else
        {
            end = o.time;
            break;
        }
    }
    start = std::max (0.0, start - 0.002);
    end = std::max (start + 0.01, end - 0.002);

    processor.setRegion ({ start, end });
    auto& player = processor.getPlayer();
    processor.setPlayheadSeconds (start);
    processor.stopPreview();
    player.play();
    repaint();
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (processor.getLoaded() == nullptr)
        return;

    const bool sideways = std::abs (w.deltaX) > std::abs (w.deltaY);
    if (e.mods.isShiftDown() || sideways)
    {
        const float d = sideways ? -w.deltaX : w.deltaY;
        const double shift = -(double) d * (viewEnd - viewStart) * 0.5;
        setView (viewStart + shift, viewEnd + shift);
        return;
    }

    zoomBy (std::pow (2.0, (double) w.deltaY * 2.0), xToSeconds ((float) e.x));
}

void WaveformView::scrollBarMoved (juce::ScrollBar*, double newStart)
{
    const double len = viewEnd - viewStart;
    viewStart = newStart;
    viewEnd = newStart + len;
    repaint();
}

void WaveformView::timerCallback()
{
    const bool auditioning = processor.isSoloPlaying();
    const double pos = auditioning ? processor.getAuditionPlayer().getPositionSeconds() : processor.getPlayheadSeconds();
    if (std::abs (pos - lastPlayhead) < 1.0e-4)
        return;
    lastPlayhead = pos;

    // Keep the playhead in view while playing.
    if ((auditioning || processor.getPlayer().isPlaying()) && (pos > viewEnd || pos < viewStart))
    {
        const double len = viewEnd - viewStart;
        setView (pos - len * 0.05, pos + len * 0.95);
        return;
    }
    repaint();
}
