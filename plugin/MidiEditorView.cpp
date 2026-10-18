#include "MidiEditorView.h"
#include "Theme.h"

#include <cmath>

namespace
{
constexpr int headerWidth = 120;
constexpr int rulerHeight = 18;
constexpr int scrollHeight = 12;
constexpr double minViewSeconds = 0.1;

// The rows that are always there; any other instrument gets one when it
// has hits.
constexpr drb::Instrument baseLanes[] { drb::Instrument::Kick, drb::Instrument::Snare,
                                        drb::Instrument::HiHatClosed, drb::Instrument::HiHatOpen };
} // namespace

int MidiEditorView::laneOf (drb::Instrument i) const
{
    for (int l = 0; l < numLanes(); ++l)
        if (lanes[(size_t) l] == i)
            return l;
    return -1;
}

void MidiEditorView::updateLanes()
{
    lanes.assign (std::begin (baseLanes), std::end (baseLanes));
    for (auto inst : drb::allInstruments)
        if (std::find (lanes.begin(), lanes.end(), inst) == lanes.end())
            for (const auto& h : processor.getAnalysis().hits)
                if (h.instrument == inst)
                {
                    lanes.push_back (inst);
                    break;
                }
}

MidiEditorView::MidiEditorView (DrumRebuilderProcessor& p) : processor (p)
{
    setWantsKeyboardFocus (true);
    scrollBar.setAutoHide (false);
    scrollBar.addListener (this);
    addAndMakeVisible (scrollBar);
    refresh();
    startTimerHz (30);
}

MidiEditorView::~MidiEditorView()
{
    scrollBar.removeListener (this);
}

juce::Rectangle<int> MidiEditorView::laneArea() const
{
    return getLocalBounds().withTrimmedLeft (headerWidth).withTrimmedTop (rulerHeight).withTrimmedBottom (scrollHeight);
}

juce::Rectangle<int> MidiEditorView::laneBounds (int lane) const
{
    const auto a = laneArea();
    const int h = a.getHeight() / numLanes();
    return { a.getX(), a.getY() + lane * h, a.getWidth(), h };
}

int MidiEditorView::laneAt (int y) const
{
    const auto a = laneArea();
    if (y < a.getY() || y >= a.getBottom())
        return -1;
    return juce::jlimit (0, numLanes() - 1, (y - a.getY()) / std::max (1, a.getHeight() / numLanes()));
}

double MidiEditorView::xToSeconds (float x) const
{
    const auto a = laneArea();
    return viewStart + (x - (float) a.getX()) / (float) std::max (1, a.getWidth()) * (viewEnd - viewStart);
}

float MidiEditorView::secondsToX (double s) const
{
    const auto a = laneArea();
    return (float) a.getX() + (float) ((s - viewStart) / (viewEnd - viewStart) * a.getWidth());
}

int MidiEditorView::hitAt (juce::Point<int> p) const
{
    const int lane = laneAt (p.y);
    if (lane < 0 || p.x < laneArea().getX())
        return -1;

    const auto& hits = processor.getAnalysis().hits;
    int best = -1;
    float bestDistance = 7.0f;
    for (size_t i = 0; i < hits.size(); ++i)
        if (laneOf (hits[i].instrument) == lane)
            if (const float d = std::abs (secondsToX (hits[i].time) - (float) p.x); d < bestDistance)
            {
                bestDistance = d;
                best = (int) i;
            }
    return best;
}

double MidiEditorView::place (double seconds, bool free) const
{
    const auto& grid = processor.getAnalysis().grid;
    seconds = juce::jlimit (0.0, duration, seconds);
    return snap && ! free && grid.bpm > 0.0 ? juce::jlimit (0.0, duration, drb::snapToGrid (seconds, grid, division)) : seconds;
}

void MidiEditorView::refresh()
{
    updateLanes();
    if (processor.getLoadedId() != shownId)
    {
        shownId = processor.getLoadedId();
        const auto* l = processor.getLoaded();
        duration = l != nullptr ? l->audio.getNumSamples() / l->sampleRate : 0.0;
        selected = dragHit = -1;
        zoomToFit();
    }
    if (selected >= (int) processor.getAnalysis().hits.size())
        selected = -1;
    repaint();
}

void MidiEditorView::setView (double start, double end)
{
    const double total = std::max (duration, minViewSeconds);
    const double len = juce::jlimit (minViewSeconds, total, end - start);
    start = juce::jlimit (0.0, total - len, start);
    viewStart = start;
    viewEnd = start + len;
    scrollBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    scrollBar.setCurrentRange (viewStart, len, juce::dontSendNotification);
    repaint();
}

void MidiEditorView::zoomToFit()
{
    setView (0.0, std::max (duration, minViewSeconds));
}

void MidiEditorView::zoomBy (double factor, double around)
{
    const double len = (viewEnd - viewStart) / factor;
    const double rel = (around - viewStart) / (viewEnd - viewStart);
    setView (around - rel * len, around - rel * len + len);
}

double MidiEditorView::zoomCentre() const
{
    const double pos = processor.getPlayer().getPositionSeconds();
    return pos >= viewStart && pos <= viewEnd ? pos : (viewStart + viewEnd) * 0.5;
}

void MidiEditorView::resized()
{
    scrollBar.setBounds (getLocalBounds().withTrimmedLeft (headerWidth).removeFromBottom (scrollHeight));
}

void MidiEditorView::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    const auto area = laneArea();
    const auto& analysis = processor.getAnalysis();
    const auto& hits = analysis.hits;

    // Rows and their names, with how many hits each has.
    for (int l = 0; l < numLanes(); ++l)
    {
        const auto r = laneBounds (l);
        g.setColour (l % 2 == 0 ? theme::panel : theme::panel.brighter (0.04f));
        g.fillRect (r);

        int count = 0;
        for (const auto& h : hits)
            count += h.instrument == lanes[(size_t) l] ? 1 : 0;
        const auto name = juce::Rectangle<int> (0, r.getY(), headerWidth, r.getHeight()).reduced (10, 0);
        g.setColour (theme::instrument (lanes[(size_t) l]));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (drb::instrumentName (lanes[(size_t) l]), name.withTrimmedBottom (r.getHeight() - r.getHeight() / 2 + 1), juce::Justification::bottomLeft);
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (juce::String (count) + (count == 1 ? " hit" : " hits"), name.withTrimmedTop (r.getHeight() / 2 + 2),
                    juce::Justification::topLeft);
    }

    if (processor.getLoaded() == nullptr)
    {
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (16.0f));
        g.drawText ("Open a drums-only recording first", area, juce::Justification::centred);
        return;
    }

    // Grid: divisions, beats, bars (numbered in the ruler).
    const auto& grid = analysis.grid;
    if (grid.bpm > 0.0)
    {
        const double beat = grid.beatSeconds();
        const int perBar = grid.timeSignature.beatsPerBar;
        const double pxPerSecond = area.getWidth() / (viewEnd - viewStart);
        const double step = division * beat;
        const bool showDivisions = step * pxPerSecond >= 6.0;
        const double unit = showDivisions ? step : beat;
        const int first = (int) std::floor ((viewStart - grid.firstDownbeat) / unit) - 1;
        const int last = (int) std::ceil ((viewEnd - grid.firstDownbeat) / unit) + 1;

        g.setFont (juce::FontOptions (11.0f));
        for (int k = first; k <= last; ++k)
        {
            const double t = grid.firstDownbeat + k * unit;
            const double beats = (t - grid.firstDownbeat) / beat;
            const bool onBeat = std::abs (beats - std::round (beats)) < 1.0e-6;
            const int beatIndex = (int) std::round (beats);
            const bool onBar = onBeat && ((beatIndex % perBar) + perBar) % perBar == 0;
            if (! showDivisions && ! onBeat)
                continue;
            if (onBeat && ! onBar && beat * pxPerSecond < 6.0)
                continue;

            const float x = secondsToX (t);
            g.setColour (onBar ? theme::barLine : onBeat ? theme::beatLine.withMultipliedAlpha (2.0f) : theme::beatLine);
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());

            const int bar = (int) std::floor ((double) beatIndex / perBar) + 1;
            if (onBar && bar >= 1 && beat * perBar * pxPerSecond > 24.0)
            {
                g.setColour (bar == 1 ? theme::startFlag : theme::textDim);
                g.drawText (juce::String (bar), (int) x + 3, 0, 40, rulerHeight, juce::Justification::centredLeft);
            }
        }
    }

    // Hits: taller and brighter when harder; "?" when the analysis is unsure.
    g.saveState();
    g.reduceClipRegion (area);
    for (size_t i = 0; i < hits.size(); ++i)
    {
        const auto& h = hits[i];
        const int lane = laneOf (h.instrument);
        if (lane < 0 || h.time < viewStart - 0.1 || h.time > viewEnd + 0.1)
            continue;

        const auto r = laneBounds (lane);
        const float x = secondsToX (h.time);
        const float height = (float) r.getHeight() * (0.25f + 0.6f * h.velocity);
        const auto box = juce::Rectangle<float> (x - 1.0f, (float) r.getCentreY() - height / 2.0f, 9.0f, height);
        const auto colour = theme::instrument (h.instrument);
        g.setColour (colour.withAlpha (0.45f + 0.55f * h.velocity));
        g.fillRoundedRectangle (box, 2.0f);
        if ((int) i == selected)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (box.expanded (1.5f), 3.0f, 2.0f);
        }
        if (h.confidence < 0.5f && ! h.manual)
        {
            g.setColour (colour);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("?", juce::Rectangle<float> (x + 9.0f, box.getY() - 2.0f, 12.0f, 12.0f), juce::Justification::centred);
        }
    }
    g.restoreState();

    // Playhead.
    const double pos = processor.getPlayer().getPositionSeconds();
    if (pos >= viewStart && pos <= viewEnd)
    {
        g.setColour (theme::accent);
        g.fillRect (juce::Rectangle<float> (secondsToX (pos) - 0.75f, 0.0f, 1.5f, (float) area.getBottom()));
    }
}

void MidiEditorView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragHit = -1;
    dragMoved = justAdded = false;
    if (processor.getLoaded() == nullptr)
        return;

    const int hit = hitAt (e.getPosition());
    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
        {
            selected = hit;
            repaint();
            showHitMenu (hit);
        }
        return;
    }

    if (hit >= 0)
    {
        selected = dragHit = hit;
        dragOffset = xToSeconds ((float) e.x) - processor.getAnalysis().hits[(size_t) hit].time;
    }
    else if (const int lane = laneAt (e.y); lane >= 0 && e.x >= laneArea().getX() && e.getNumberOfClicks() == 1)
    {
        // A new hit, medium-hard; it can be dragged right away.
        selected = dragHit = processor.addHitAt (place (xToSeconds ((float) e.x), e.mods.isAltDown()), lanes[(size_t) lane], 0.8f);
        dragOffset = 0.0;
        justAdded = true;
    }
    else if (e.y < rulerHeight && e.x >= laneArea().getX())
    {
        processor.getPlayer().setPositionSeconds (juce::jlimit (0.0, duration, xToSeconds ((float) e.x)));
    }
    repaint();
}

void MidiEditorView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragHit < 0 || (! dragMoved && e.getDistanceFromDragStart() < 3))
        return;

    if (! dragMoved && ! justAdded)
        processor.beginHitEdit();   // the whole drag is one undo step
    dragMoved = true;

    const int lane = juce::jlimit (0, numLanes() - 1, laneAt (juce::jlimit (laneArea().getY(), laneArea().getBottom() - 1, e.y)));
    const double t = place (xToSeconds ((float) e.x) - dragOffset, e.mods.isAltDown());
    selected = dragHit = processor.moveHitTo (dragHit, t, lanes[(size_t) lane]);
    repaint();
}

void MidiEditorView::mouseUp (const juce::MouseEvent& e)
{
    // A plain click on a hit: the playhead goes there.
    if (dragHit >= 0 && ! dragMoved && ! justAdded && ! e.mods.isPopupMenu())
        processor.getPlayer().setPositionSeconds (processor.getAnalysis().hits[(size_t) dragHit].time);
    dragHit = -1;
    dragMoved = justAdded = false;
}

void MidiEditorView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const int hit = hitAt (e.getPosition()); hit >= 0 && ! e.mods.isPopupMenu())
    {
        processor.deleteHitAt (hit);
        selected = -1;
        repaint();
    }
}

void MidiEditorView::showHitMenu (int hit)
{
    juce::PopupMenu menu;
    juce::WeakReference<juce::Component> self (this);
    const float now = processor.getAnalysis().hits[(size_t) hit].velocity;
    menu.addSectionHeader ("How hard: " + juce::String (juce::roundToInt (now * 100.0f)) + " %");
    for (int percent : { 100, 75, 50, 25 })
        menu.addItem (juce::String (percent) + " %", [this, self, hit, percent]
        {
            if (self != nullptr)
                processor.setHitVelocity (hit, (float) percent / 100.0f);
        });
    menu.addSeparator();
    menu.addItem ("Delete", [this, self, hit]
    {
        if (self != nullptr)
        {
            processor.deleteHitAt (hit);
            selected = -1;
        }
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

bool MidiEditorView::keyPressed (const juce::KeyPress& key)
{
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && selected >= 0)
    {
        processor.deleteHitAt (selected);
        selected = -1;
        repaint();
        return true;
    }
    return false;   // Space, W, E, arrows ... go to the editor
}

void MidiEditorView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
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

void MidiEditorView::scrollBarMoved (juce::ScrollBar*, double newStart)
{
    const double len = viewEnd - viewStart;
    viewStart = newStart;
    viewEnd = newStart + len;
    repaint();
}

void MidiEditorView::timerCallback()
{
    const double pos = processor.getPlayer().getPositionSeconds();
    if (std::abs (pos - lastPlayhead) < 1.0e-4)
        return;
    lastPlayhead = pos;

    if (processor.getPlayer().isPlaying() && (pos > viewEnd || pos < viewStart))
    {
        const double len = viewEnd - viewStart;
        setView (pos - len * 0.05, pos + len * 0.95);
        return;
    }
    repaint();
}
