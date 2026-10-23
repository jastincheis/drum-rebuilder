#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <optional>

// The recording with its bar and beat grid, the detected hits, the selected
// region and the playhead. Click = play from there, double-click = select the
// hit under the mouse (up to the next one), drag = select a region, right
// click = "Bar 1 starts here", mouse wheel = zoom, shift + wheel or the
// scrollbar = move along. The yellow flag (bar 1, where the beat starts) and
// the hit markers under the waveform can be dragged.
class BeatWaveformView : public juce::Component,
                     private juce::Timer,
                     private juce::ScrollBar::Listener
{
public:
    explicit BeatWaveformView (DrumRebuilderProcessor&);
    ~BeatWaveformView() override;

    // Call when the recording or its analysis changed.
    void refresh();

    void zoomBy (double factor, double aroundSeconds);
    void zoomToFit();
    double centreSeconds() const { return (viewStart + viewEnd) * 0.5; }
    double zoomCentre() const;
    void scrollToShow (double seconds);   // keeps the zoom

    // The Instruments tab: every hit's instruments written on the waveform
    // in their colours; a click on one says what it really is.
    void setInstrumentMode (bool on) { instrumentMode = on; }

    // Instruments tab: several labels selected (Ctrl + click, Shift + drag
    // a box), then a click on one of them changes them all.
    bool clearSelection();   // false = nothing was selected
    int getSelectionSize() const { return (int) selection.size(); }

    // Solo instruments: their hits stand out, the others fade.
    void setFocus (std::vector<drb::Instrument> instruments) { focus = std::move (instruments); repaint(); }

    // Moves the playhead to the previous / next hit (direction -1 / +1).
    void jumpToHit (int direction);   // the playhead if it is in view, else the centre

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    DrumRebuilderProcessor& processor;
    juce::ScrollBar scrollBar { false };

    // Min / max per block of `peakBlock` samples, mono.
    std::vector<std::pair<float, float>> peaks;
    int peakBlock = 64;
    double sampleRate = 44100.0, duration = 0.0;
    int shownId = -1;

    double viewStart = 0.0, viewEnd = 1.0;   // seconds
    double dragAnchor = 0.0;
    bool dragging = false;

    // What a press grabbed: the bar 1 flag, a hit marker, or nothing (= a
    // click or a region drag).
    enum class Grab { none, flag, hit };
    Grab grab = Grab::none;
    int grabbedHit = -1;
    int hoverMarker = -1;
    double markerGrabOffset = 0.0;   // seconds from the mouse to the grabbed hit
    double markerSeconds (int onset) const;   // Instruments tab: where its marker is drawn (its earliest IN)   // the marker under the mouse (its handle lights up)

    Grab findGrab (juce::Point<int>, int& hitIndex) const;
    double snapToHit (double seconds, int ignoreIndex) const;
    double lastPlayhead = -1.0;
    std::vector<drb::Instrument> focus;   // empty = none
    bool focused (drb::Instrument i) const { return std::find (focus.begin(), focus.end(), i) != focus.end(); }
    bool instrumentMode = false;

    // Instruments tab with instruments on solo: only theirs can be changed
    // (names, IN / OUT, markers); the others are shown but locked.
    bool editable (drb::Instrument i) const { return ! instrumentMode || focus.empty() || focused (i); }
    bool editableOnset (int onset) const;   // has at least one editable instrument

    // The instrument bars drawn on the waveform (name + IN..OUT), for clicks.
    struct Pill { juce::Rectangle<int> label, bar; int onset, hit; drb::Instrument instrument; };
    std::vector<Pill> pills;
    enum class PillPart { none, label, in, out };
    PillPart findPill (juce::Point<int>, const Pill*& found) const;
    bool pillPressed = false;
    PillPart slicePart = PillPart::none;
    int sliceHit = -1, pillHit = -1;
    drb::Instrument pillInstrument = drb::Instrument::Kick;
    void showPillMenu (int hitIndex, drb::Instrument);

    using Selected = std::pair<double, drb::Instrument>;   // onset time, instrument
    std::vector<Selected> selection;
    bool isSelected (double time, drb::Instrument) const;
    void toggleSelected (double time, drb::Instrument);
    void pruneSelection();   // drops what is no longer there
    void showSelectionMenu();
    bool lassoing = false, selectClick = false;
    juce::Rectangle<int> lasso;


    juce::Rectangle<int> waveArea() const;
    double xToSeconds (float x) const;
    float secondsToX (double s) const;
    void setView (double start, double end);
    void buildPeaks();
    void showMenu (const juce::MouseEvent&);
    void showRenameMenu (int hitIndex);
    void showAddMenu (double seconds);   // "Add a marker here - it is a:"
    void addInstrumentItems (juce::PopupMenu&, double seconds);

    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
};
