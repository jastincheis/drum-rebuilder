#pragma once

#include "../core/Model.h"

#include <juce_graphics/juce_graphics.h>

namespace theme
{
const juce::Colour background   { 0xff1e1f24 };
const juce::Colour panel        { 0xff2a2c33 };
const juce::Colour panelLight   { 0xff353842 };
const juce::Colour text         { 0xffe8e8ec };
const juce::Colour textDim      { 0xff9a9ca6 };
const juce::Colour accent       { 0xfff2a33a };   // amber: playhead, active buttons
const juce::Colour wave         { 0xff5fb3d9 };
const juce::Colour onset        { 0xffff6b6b };
const juce::Colour startFlag    { 0xffffd93d };   // yellow: bar 1, where the beat starts
const juce::Colour barLine      { 0x66ffffff };
const juce::Colour beatLine     { 0x22ffffff };
const juce::Colour selection    { 0x33f2a33a };

// One colour per instrument, for markers, labels and MIDI lanes.
inline juce::Colour instrument (drb::Instrument i)
{
    switch (i)
    {
        case drb::Instrument::Kick:        return juce::Colour (0xffff6b6b);
        case drb::Instrument::Snare:       return juce::Colour (0xff7ee081);
        case drb::Instrument::HiHatClosed: return juce::Colour (0xffc792ea);
        case drb::Instrument::HiHatOpen:   return juce::Colour (0xffff9ff3);
        case drb::Instrument::Clap:
        case drb::Instrument::Rimshot:
        case drb::Instrument::TomLow:
        case drb::Instrument::TomMid:
        case drb::Instrument::TomHigh:
        case drb::Instrument::Crash:
        case drb::Instrument::Ride:        break;
    }
    return juce::Colour (0xffb0b3bd);
}

// Short names for the markers: full when there is room, one letter when not.
inline juce::String shortName (drb::Instrument i, bool tiny)
{
    switch (i)
    {
        case drb::Instrument::Kick:        return tiny ? "K" : "Kick";
        case drb::Instrument::Snare:       return tiny ? "S" : "Snare";
        case drb::Instrument::HiHatClosed: return tiny ? "H" : "Hat";
        case drb::Instrument::HiHatOpen:   return tiny ? "O" : "Open hat";
        case drb::Instrument::Clap:
        case drb::Instrument::Rimshot:
        case drb::Instrument::TomLow:
        case drb::Instrument::TomMid:
        case drb::Instrument::TomHigh:
        case drb::Instrument::Crash:
        case drb::Instrument::Ride:        break;
    }
    return tiny ? juce::String (drb::instrumentName (i)).substring (0, 1) : juce::String (drb::instrumentName (i));
}
} // namespace theme
