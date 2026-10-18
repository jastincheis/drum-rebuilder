#include "Model.h"

namespace drb
{

const char* instrumentName (Instrument i)
{
    switch (i)
    {
        case Instrument::Kick:        return "Kick";
        case Instrument::Snare:       return "Snare";
        case Instrument::HiHatClosed: return "Hi-hat closed";
        case Instrument::HiHatOpen:   return "Hi-hat open";
        case Instrument::Clap:        return "Clap";
        case Instrument::Rimshot:     return "Rimshot";
        case Instrument::TomLow:      return "Tom low";
        case Instrument::TomMid:      return "Tom mid";
        case Instrument::TomHigh:     return "Tom high";
        case Instrument::Crash:       return "Crash";
        case Instrument::Ride:        return "Ride";
    }
    return "?";
}

int gmNote (Instrument i)
{
    switch (i)
    {
        case Instrument::Kick:        return 36;
        case Instrument::Snare:       return 38;
        case Instrument::HiHatClosed: return 42;
        case Instrument::HiHatOpen:   return 46;
        case Instrument::Clap:        return 39;
        case Instrument::Rimshot:     return 37;
        case Instrument::TomLow:      return 41;
        case Instrument::TomMid:      return 45;
        case Instrument::TomHigh:     return 48;
        case Instrument::Crash:       return 49;
        case Instrument::Ride:        return 51;
    }
    return 36;
}

} // namespace drb
