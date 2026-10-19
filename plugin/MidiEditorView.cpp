#include "MidiEditorView.h"
#include "Focus.h"
#include "Theme.h"

#include <cmath>

namespace
{
constexpr int headerWidth = 140;
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

// A hit's bar: taller (from the row's middle out) the harder it is hit.
juce::Rectangle<float> MidiEditorView::hitBox (int index) const
{
    const auto& h = processor.getAnalysis().hits[(size_t) index];
    const auto r = laneBounds (laneOf (h.instrument));
    const float height = (float) r.getHeight() * (0.25f + 0.6f * h.velocity);
    return { secondsToX (h.time) - 1.0f, (float) r.getCentreY() - height / 2.0f, 9.0f, height };
}

juce::Rectangle<int> MidiEditorView::soloButton (int lane) const
{
    const auto r = laneBounds (lane);
    return { headerWidth - 50, r.getCentreY() - 12, 42, 24 };
}

// The volume flag on top of a hit: drag it up / down.
juce::Rectangle<float> MidiEditorView::volumeFlag (int index) const
{
    const auto box = hitBox (index);
    const float top = (float) laneBounds (laneOf (processor.getAnalysis().hits[(size_t) index].instrument)).getY() + 2.0f;
    return { box.getCentreX() - 7.0f, std::max (top, box.getY() - 16.0f), 14.0f, 12.0f };
}

// On a hit's volume flag, or its top or bottom edge: dragging there sets how
// loud it is.
bool MidiEditorView::onVelocityEdge (int index, juce::Point<int> p) const
{
    if (volumeFlag (index).expanded (4.0f).contains (p.toFloat()))
        return true;
    const auto box = hitBox (index);
    return std::abs ((float) p.y - box.getY()) <= 5.0f || std::abs ((float) p.y - box.getBottom()) <= 5.0f;
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
    // Hits added or removed (here or by undo): the indices no longer hold.
    if (processor.getAnalysis().hits.size() != shownHitCount)
    {
        shownHitCount = processor.getAnalysis().hits.size();
        picked.clear();
        pickAnchor = -1;
    }
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
    const double pos = processor.getShownPlayheadSeconds();
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
        const auto name = juce::Rectangle<int> (0, r.getY(), headerWidth - 52, r.getHeight()).withTrimmedLeft (10);
        g.setColour (theme::instrument (lanes[(size_t) l]));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawFittedText (drb::instrumentName (lanes[(size_t) l]), name.withTrimmedBottom (r.getHeight() - r.getHeight() / 2 + 1),
                          juce::Justification::bottomLeft, 1, 0.75f);
        g.setColour (theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (juce::String (count) + (count == 1 ? " hit" : " hits"), name.withTrimmedTop (r.getHeight() / 2 + 2),
                    juce::Justification::topLeft);

        // Solo: hear only this row.
        const auto sb = soloButton (l).toFloat();
        const bool on = processor.isSolo (lanes[(size_t) l]);
        g.setColour (on ? theme::instrument (lanes[(size_t) l]) : theme::panelLight);
        g.fillRoundedRectangle (sb, 4.0f);
        g.setColour (on ? juce::Colours::black : theme::instrument (lanes[(size_t) l]).withAlpha (0.8f));
        g.drawRoundedRectangle (sb.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (on ? juce::Colours::black : juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("Solo", sb, juce::Justification::centred);
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

    // Hits: taller and brighter when harder; "?" when the analysis is unsure;
    // faded when other rows are on solo.
    const bool anySolo = processor.anySolo();
    g.saveState();
    g.reduceClipRegion (area);
    for (size_t i = 0; i < hits.size(); ++i)
    {
        const auto& h = hits[i];
        const int lane = laneOf (h.instrument);
        if (lane < 0 || h.time < viewStart - 0.1 || h.time > viewEnd + 0.1)
            continue;

        const float x = secondsToX (h.time);
        const auto box = hitBox ((int) i);
        const auto colour = theme::instrument (h.instrument);
        const bool muted = anySolo && ! processor.isSolo (h.instrument);
        g.setColour (colour.withAlpha (muted ? 0.15f : 0.45f + 0.55f * h.velocity));
        g.fillRoundedRectangle (box, 2.0f);

        // Its volume flag: a little tab on a stem.
        const auto flag = volumeFlag ((int) i);
        const bool active = (int) i == hoverHit || (int) i == selected || (int) i == dragHit;
        g.setColour (colour.withAlpha (muted ? 0.2f : 1.0f));
        g.drawVerticalLine ((int) box.getCentreX(), flag.getBottom(), box.getY());
        g.fillRoundedRectangle (flag, 3.0f);
        if (active)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (flag.expanded (1.0f), 3.0f, 1.5f);
        }
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        for (int k = 0; k < 2; ++k)   // grip lines
            g.drawHorizontalLine ((int) flag.getY() + 4 + 3 * k, flag.getX() + 3.0f, flag.getRight() - 3.0f);
        if ((int) i == selected || isPicked ((int) i))
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (box.expanded (1.5f), 3.0f, 2.0f);
            if (isPicked ((int) i))
                g.drawRoundedRectangle (flag.expanded (1.0f), 3.0f, 1.5f);
        }
        if (h.confidence < 0.5f && ! h.manual)
        {
            g.setColour (colour);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("?", juce::Rectangle<float> (x + 9.0f, box.getY() - 2.0f, 12.0f, 12.0f), juce::Justification::centred);
        }
    }
    // How loud the hit under the mouse (or being dragged, or selected) is.
    const int shown = dragHit >= 0 && velocityDrag ? dragHit : hoverHit >= 0 ? hoverHit : selected;
    if (juce::isPositiveAndBelow (shown, (int) hits.size()) && laneOf (hits[(size_t) shown].instrument) >= 0)
    {
        const float db = processor.getHitLevelDb (shown);
        const auto text = db <= -99.0f ? juce::String ("silent") : juce::String (db, 1) + " dB";
        const auto box = hitBox (shown);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        const float w = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 12.0f;
        auto label = juce::Rectangle<float> (box.getRight() + 4.0f, box.getCentreY() - 10.0f, w, 20.0f);
        if (label.getRight() > (float) area.getRight())
            label.setX (box.getX() - 4.0f - w);
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.fillRoundedRectangle (label, 4.0f);
        g.setColour (db > 0.0f ? juce::Colours::red : juce::Colours::white);
        g.drawText (text, label, juce::Justification::centred);
    }
    if (boxing && ! box.isEmpty())
    {
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRect (box);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawRect (box, 1);
    }
    g.restoreState();

    // Playhead.
    const double pos = processor.getShownPlayheadSeconds();
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
    dragMoved = justAdded = velocityDrag = false;
    if (processor.getLoaded() == nullptr)
        return;

    for (int l = 0; l < numLanes(); ++l)
        if (soloButton (l).contains (e.getPosition()))
        {
            // Only this row; again = everything again. Ctrl / Shift = one more.
            const auto inst = lanes[(size_t) l];
            const bool on = ! processor.isSolo (inst);
            if (! (e.mods.isCtrlDown() || e.mods.isShiftDown()))
                for (auto other : processor.getSolo())
                    if (other != inst)
                        processor.setSolo (other, false);
            processor.setSolo (inst, on);
            repaint();
            return;
        }

    boxing = false;
    const int hit = hitAt (e.getPosition());
    if (e.mods.isPopupMenu())
    {
        if (hit >= 0)
        {
            selected = hit;
            repaint();
            if (picked.size() > 1 && isPicked (hit))
                showPickedMenu();
            else
            {
                clearPicked();
                showHitMenu (hit);
            }
        }
        return;
    }

    const bool ctrl = e.mods.isCommandDown(), shift = e.mods.isShiftDown();
    if (hit >= 0 && (ctrl || shift) && ! onVelocityEdge (hit, e.getPosition()))
    {
        // Ctrl: this one in / out. Shift: every hit of its row from the
        // last one picked up to here.
        const auto& hits = processor.getAnalysis().hits;
        if (picked.empty() && selected >= 0 && selected != hit)
            picked.push_back (selected);
        if (shift && juce::isPositiveAndBelow (pickAnchor, (int) hits.size())
            && hits[(size_t) pickAnchor].instrument == hits[(size_t) hit].instrument)
        {
            const auto inst = hits[(size_t) hit].instrument;
            for (int i = std::min (pickAnchor, hit); i <= std::max (pickAnchor, hit); ++i)
                if (hits[(size_t) i].instrument == inst && ! isPicked (i))
                    picked.push_back (i);
        }
        else if (isPicked (hit))
            picked.erase (std::find (picked.begin(), picked.end(), hit));
        else
            picked.push_back (hit);
        pickAnchor = hit;
        selected = hit;
        repaint();
        return;
    }
    if (hit < 0 && (ctrl || shift) && laneAt (e.y) >= 0 && e.x >= laneArea().getX())
    {
        boxing = true;   // a box picking the hits in it
        box = {};
        return;
    }

    if (hit >= 0)
    {
        if (! isPicked (hit))
            picked.clear();
        selected = dragHit = hit;
        dragOffset = xToSeconds ((float) e.x) - processor.getAnalysis().hits[(size_t) hit].time;
        velocityDrag = onVelocityEdge (hit, e.getPosition());
        velocityAtStart = processor.getAnalysis().hits[(size_t) hit].velocity;
        grabbedAbove = (float) e.y < hitBox (hit).getCentreY();
        pickedStart.clear();
        for (int i : picked)
            pickedStart.push_back (processor.getAnalysis().hits[(size_t) i].velocity);
    }
    else if (const int lane = laneAt (e.y); lane >= 0 && e.x >= laneArea().getX() && e.getNumberOfClicks() == 1)
    {
        // A new hit, medium-hard, with its instrument's own sound (one it
        // plays alone in the recording); it can be dragged right away.
        picked.clear();
        selected = dragHit = processor.addHitAt (place (xToSeconds ((float) e.x), e.mods.isAltDown()), lanes[(size_t) lane], 0.8f);
        dragOffset = 0.0;
        justAdded = dragHit >= 0;
    }
    else if (e.y < rulerHeight && e.x >= laneArea().getX())
    {
        processor.setPlayheadSeconds (juce::jlimit (0.0, duration, xToSeconds ((float) e.x)));
    }
    repaint();
}

void MidiEditorView::mouseDrag (const juce::MouseEvent& e)
{
    if (boxing)
    {
        box = juce::Rectangle<int> (e.getMouseDownPosition(), e.getPosition()).getIntersection (laneArea());
        repaint();
        return;
    }
    if (dragHit < 0 || (! dragMoved && e.getDistanceFromDragStart() < 3))
        return;

    if (! dragMoved && ! justAdded)
        processor.beginHitEdit();   // the whole drag is one undo step
    dragMoved = true;

    if (velocityDrag)
    {
        if (! hitPreviewing)
        {
            hitPreviewing = true;
            processor.startHitPreview (dragHit);
        }
        // Away from the row's middle = louder: the bar's edge follows the
        // mouse from where it was grabbed (the flag too). Shift = finer.
        const auto r = laneBounds (laneOf (processor.getAnalysis().hits[(size_t) dragHit].instrument));
        const float moved = (float) (grabbedAbove ? -e.getDistanceFromDragStartY() : e.getDistanceFromDragStartY());
        const float perPixel = 1.0f / (0.3f * (float) r.getHeight()) * (e.mods.isShiftDown() ? 0.25f : 1.0f);
        const float delta = moved * perPixel;
        if (picked.size() > 1 && isPicked (dragHit) && pickedStart.size() == picked.size())
        {
            // All the selected ones, by the same amount.
            std::vector<std::pair<int, float>> changes;
            for (size_t k = 0; k < picked.size(); ++k)
                changes.push_back ({ picked[k], pickedStart[k] + delta });
            processor.setHitVelocities (changes, false);
        }
        else
            processor.setHitVelocity (dragHit, juce::jlimit (0.05f, 1.0f, velocityAtStart + delta), false);
        repaint();
        return;
    }
    picked.clear();   // moving a hit: just that one

    const int lane = juce::jlimit (0, numLanes() - 1, laneAt (juce::jlimit (laneArea().getY(), laneArea().getBottom() - 1, e.y)));
    const double t = place (xToSeconds ((float) e.x) - dragOffset, e.mods.isAltDown());
    selected = dragHit = processor.moveHitTo (dragHit, t, lanes[(size_t) lane]);
    repaint();
}

void MidiEditorView::mouseMove (const juce::MouseEvent& e)
{
    for (int l = 0; l < numLanes(); ++l)
        if (soloButton (l).contains (e.getPosition()))
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            return;
        }
    const int hit = processor.getLoaded() != nullptr ? hitAt (e.getPosition()) : -1;
    if (hit != hoverHit)
    {
        hoverHit = hit;
        repaint();
    }
    setMouseCursor (hit >= 0 && onVelocityEdge (hit, e.getPosition()) ? juce::MouseCursor::UpDownResizeCursor
                    : hit >= 0                                         ? juce::MouseCursor::LeftRightResizeCursor
                                                                       : juce::MouseCursor::NormalCursor);
}

void MidiEditorView::mouseExit (const juce::MouseEvent&)
{
    if (hoverHit >= 0)
    {
        hoverHit = -1;
        repaint();
    }
}

void MidiEditorView::mouseUp (const juce::MouseEvent& e)
{
    if (boxing)
    {
        boxing = false;
        if (! box.isEmpty())
        {
            const auto& hits = processor.getAnalysis().hits;
            if (picked.empty() && selected >= 0)
                picked.push_back (selected);
            for (int i = 0; i < (int) hits.size(); ++i)
                if (laneOf (hits[(size_t) i].instrument) >= 0 && box.toFloat().intersects (hitBox (i)) && ! isPicked (i))
                    picked.push_back (i);
        }
        box = {};
        repaint();
        return;
    }
    if (hitPreviewing)
    {
        hitPreviewing = false;
        processor.endHitPreview();
    }
    // A plain click on a hit: the playhead goes there.
    if (dragHit >= 0 && ! dragMoved && ! justAdded && ! e.mods.isPopupMenu())
        processor.setPlayheadSeconds (processor.getAnalysis().hits[(size_t) dragHit].time);
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
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), focus::afterMenu (this));
}

// Right-click on one of several selected hits: for all of them.
void MidiEditorView::showPickedMenu()
{
    juce::PopupMenu menu;
    juce::WeakReference<juce::Component> self (this);
    const auto setAll = [this, self] (std::function<float (float)> f)
    {
        if (self == nullptr)
            return;
        std::vector<std::pair<int, float>> changes;
        for (int i : picked)
            if (juce::isPositiveAndBelow (i, (int) processor.getAnalysis().hits.size()))
                changes.push_back ({ i, f (processor.getAnalysis().hits[(size_t) i].velocity) });
        processor.setHitVelocities (changes);
    };
    menu.addSectionHeader (juce::String ((int) picked.size()) + " hits selected");
    menu.addItem ("Louder (+10 %)", [setAll] { setAll ([] (float v) { return v + 0.1f; }); });
    menu.addItem ("Quieter (-10 %)", [setAll] { setAll ([] (float v) { return v - 0.1f; }); });
    juce::PopupMenu same;
    for (int percent : { 100, 75, 50, 25 })
        same.addItem (juce::String (percent) + " %", [setAll, percent] { setAll ([percent] (float) { return (float) percent / 100.0f; }); });
    menu.addSubMenu ("All as hard as", same);
    menu.addSeparator();
    menu.addItem ("Delete them", [this, self]
    {
        if (self == nullptr)
            return;
        processor.deleteHitsAt (picked);
        picked.clear();
        selected = -1;
    });
    menu.addItem ("Clear the selection", [this, self]
    {
        if (self != nullptr)
            clearPicked();
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), focus::afterMenu (this));
}

bool MidiEditorView::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && ! picked.empty())
    {
        clearPicked();
        return true;
    }
    if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && picked.size() > 1)
    {
        processor.deleteHitsAt (picked);
        picked.clear();
        selected = -1;
        repaint();
        return true;
    }
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
    const double pos = processor.getShownPlayheadSeconds();
    if (std::abs (pos - lastPlayhead) < 1.0e-4)
        return;
    lastPlayhead = pos;

    if (processor.isAnythingPlaying() && (pos > viewEnd || pos < viewStart))
    {
        const double len = viewEnd - viewStart;
        setView (pos - len * 0.05, pos + len * 0.95);
        return;
    }
    repaint();
}
