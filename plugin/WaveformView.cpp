#include "WaveformView.h"
#include "Theme.h"

#include <cmath>

namespace
{
constexpr int rulerHeight = 18;
constexpr int scrollHeight = 12;
constexpr int markerHeight = 10;
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
    const double pos = processor.getPlayer().getPositionSeconds();
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
    auto& player = processor.getPlayer();
    const double pos = player.getPositionSeconds();

    // A little slack, so pressing again while playing goes on to the next hit.
    double target = direction > 0 ? duration : 0.0;
    for (const auto& o : onsets)
    {
        if (direction > 0 && o.time > pos + 0.005)
        {
            target = o.time;
            break;
        }
        if (direction < 0 && o.time < pos - 0.005)
            target = o.time;
    }
    player.setPositionSeconds (target);
    scrollToShow (target);
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

    // The waveform, one min/max line per pixel.
    const double secondsPerPx = (viewEnd - viewStart) / std::max (1, area.getWidth());
    const float mid = (float) area.getCentreY(), halfH = area.getHeight() * 0.48f;
    g.setColour (theme::wave);
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
        g.drawVerticalLine (area.getX() + x, mid - hi * halfH, mid - lo * halfH + 1.0f);
    }

    // Detected hits: a thin line and a marker under the waveform, brighter
    // for stronger hits.
    float strongest = 0.0f;
    for (const auto& o : analysis.onsets)
        strongest = std::max (strongest, o.strength);

    for (const auto& o : analysis.onsets)
    {
        if (o.time < viewStart || o.time > viewEnd)
            continue;
        const float x = secondsToX (o.time);
        const bool grabbed = grab == Grab::hit && &o == &analysis.onsets[(size_t) grabbedHit];
        const float a = grabbed ? 1.0f : 0.35f + 0.65f * (strongest > 0.0f ? o.strength / strongest : 1.0f);
        g.setColour (theme::onset.withAlpha (a * 0.35f));
        g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
        g.setColour (grabbed ? juce::Colours::white : theme::onset.withAlpha (a));
        juce::Path tri;
        const float y = (float) area.getBottom() + 1.0f;
        tri.addTriangle (x, y, x - 4.0f, y + markerHeight - 2, x + 4.0f, y + markerHeight - 2);
        g.fillPath (tri);
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

    // Playhead.
    const double pos = processor.getPlayer().getPositionSeconds();
    if (pos >= viewStart && pos <= viewEnd)
    {
        g.setColour (theme::accent);
        g.fillRect (juce::Rectangle<float> (secondsToX (pos) - 0.75f, 0.0f, 1.5f, (float) area.getBottom()));
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
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
    int hit = -1;
    setMouseCursor (processor.getLoaded() != nullptr && findGrab (e.getPosition(), hit) != Grab::none
                        ? juce::MouseCursor::LeftRightResizeCursor
                        : juce::MouseCursor::NormalCursor);
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || processor.getLoaded() == nullptr || (! dragging && e.getDistanceFromDragStart() < 4))
        return;

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
    if (e.mods.isPopupMenu() || processor.getLoaded() == nullptr)
        return;

    auto& player = processor.getPlayer();
    if (grab != Grab::none)
    {
        // Moved a flag or a hit: nothing else. A plain click on it plays
        // from there.
        if (! dragging)
            player.setPositionSeconds (grab == Grab::flag ? processor.getAnalysis().grid.firstDownbeat
                                                          : processor.getAnalysis().onsets[(size_t) grabbedHit].time);
    }
    else if (dragging)
    {
        // Play from the start of the new region.
        player.setPositionSeconds (processor.getRegion().getStart());
    }
    else
    {
        // A click clears the region and moves the playhead there.
        processor.setRegion ({});
        player.setPositionSeconds (dragAnchor);
    }
    dragging = false;
    grab = Grab::none;
    repaint();
}

void WaveformView::showMenu (const juce::MouseEvent& e)
{
    if (processor.getLoaded() == nullptr || processor.getAnalysis().grid.bpm <= 0.0)
        return;

    const double t = juce::jlimit (0.0, duration, xToSeconds ((float) e.x));
    juce::PopupMenu menu;
    menu.addItem ("Bar 1 starts here", [this, t]
    {
        processor.setFirstDownbeat (t);
        repaint();
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
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
    player.setPositionSeconds (start);
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
    const double pos = processor.getPlayer().getPositionSeconds();
    if (std::abs (pos - lastPlayhead) < 1.0e-4)
        return;
    lastPlayhead = pos;

    // Keep the playhead in view while playing.
    if (processor.getPlayer().isPlaying() && (pos > viewEnd || pos < viewStart))
    {
        const double len = viewEnd - viewStart;
        setView (pos - len * 0.05, pos + len * 0.95);
        return;
    }
    repaint();
}
