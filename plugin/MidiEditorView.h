#pragma once

#include "PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

// The hits as MIDI: one row per instrument over the bar / beat grid. Click an
// empty spot = add a hit, drag = move it (onto another row = change the
// instrument, Alt = off the grid), drag its top or bottom = how hard it is
// hit, double-click or Delete = remove it, right-click = more. S on a row =
// solo. Wheel = zoom, shift + wheel = move along.
class MidiEditorView : public juce::Component,
                       private juce::Timer,
                       private juce::ScrollBar::Listener
{
public:
    explicit MidiEditorView (DrumRebuilderProcessor&);
    ~MidiEditorView() override;

    void refresh();
    void setDivision (double beats) { division = beats; repaint(); }
    void setSnap (bool on) { snap = on; }

    void zoomBy (double factor, double aroundSeconds);
    void zoomToFit();
    double zoomCentre() const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    DrumRebuilderProcessor& processor;
    juce::ScrollBar scrollBar { false };

    double duration = 0.0, viewStart = 0.0, viewEnd = 1.0;
    int shownId = -1;
    double division = 0.25;   // beats
    bool snap = true;

    int selected = -1;        // index into the hits
    int dragHit = -1;
    bool dragMoved = false, justAdded = false, velocityDrag = false;
    double dragOffset = 0.0;  // seconds between the mouse and the hit
    double lastPlayhead = -1.0;

    std::vector<drb::Instrument> lanes;   // rows, top to bottom
    int numLanes() const { return (int) lanes.size(); }
    int laneOf (drb::Instrument) const;   // -1 = no row
    void updateLanes();

    juce::Rectangle<int> laneArea() const;
    juce::Rectangle<int> laneBounds (int lane) const;
    int laneAt (int y) const;           // -1 = none
    int hitAt (juce::Point<int>) const; // -1 = none
    double xToSeconds (float x) const;
    float secondsToX (double s) const;
    double place (double seconds, bool free) const;   // snapped unless free
    juce::Rectangle<float> hitBox (int index) const;
    juce::Rectangle<int> soloButton (int lane) const;
    bool onVelocityEdge (int index, juce::Point<int>) const;
    void setView (double start, double end);
    void showHitMenu (int hit);

    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
};
