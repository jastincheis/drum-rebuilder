#pragma once

#include "Model.h"
#include "Onsets.h"

#include <vector>

// Which drum made each onset: kick, snare, hi-hat closed or open, several at
// once when they were played together.
namespace drb
{

// One or more hits per onset, sorted by time. Velocity is relative to the
// loudest hit of the same instrument in the recording.
std::vector<Hit> classifyHits (const std::vector<Onset>&, const std::vector<float>& mono, double sampleRate);

} // namespace drb
