#pragma once

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
} // namespace theme
