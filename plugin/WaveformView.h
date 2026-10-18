#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

// The recording with its bar and beat grid, the detected hits, the selected
// region and the playhead. Click = play from there, double-click = select the
// hit under the mouse (up to the next one), drag = select a region, right
// click = "Bar 1 starts here", mouse wheel = zoom, shift + wheel or the
// scrollbar = move along. The yellow flag (bar 1, where the beat starts) and
// the hit markers under the waveform can be dragged.
class WaveformView : public juce::Component,
                     private juce::Timer,
                     private juce::ScrollBar::Listener
{
public:
    explicit WaveformView (DrumRebuilderProcessor&);
    ~WaveformView() override;

    // Call when the recording or its analysis changed.
    void refresh();

    void zoomBy (double factor, double aroundSeconds);
    void zoomToFit();
    double centreSeconds() const { return (viewStart + viewEnd) * 0.5; }
    double zoomCentre() const;
    void scrollToShow (double seconds);   // keeps the zoom

    // While checking an instrument: its markers stand out, the others fade.
    void setFocus (std::optional<drb::Instrument> i) { focus = i; repaint(); }

    // Moves the playhead to the previous / next hit (direction -1 / +1).
    void jumpToHit (int direction);   // the playhead if it is in view, else the centre

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
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

    Grab findGrab (juce::Point<int>, int& hitIndex) const;
    double snapToHit (double seconds, int ignoreIndex) const;
    double lastPlayhead = -1.0;
    std::optional<drb::Instrument> focus;

    juce::Rectangle<int> waveArea() const;
    double xToSeconds (float x) const;
    float secondsToX (double s) const;
    void setView (double start, double end);
    void buildPeaks();
    void showMenu (const juce::MouseEvent&);
    void showRenameMenu (int hitIndex);

    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
};
