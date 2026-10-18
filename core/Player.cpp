#include "Player.h"

#include <algorithm>
#include <cmath>

namespace drb
{

void Player::prepare (double rate)
{
    deviceRate = rate;
}

juce::int64 Player::toSamples (double seconds) const
{
    return (juce::int64) std::llround (std::max (0.0, seconds) * deviceRate.load());
}

// Visible to the UI at once; the audio thread takes it over on its next block
// (it writes `position` back at the end of each block, which would otherwise
// undo a jump made meanwhile).
void Player::moveTo (juce::int64 p)
{
    position = p;
    seekRequest = p;
}

void Player::setSource (std::unique_ptr<juce::AudioBuffer<float>> audio)
{
    {
        const juce::SpinLock::ScopedLockType sl (sourceLock);
        std::swap (source, audio);
        sourceLength = source != nullptr ? source->getNumSamples() : 0;
        if (position.load() > sourceLength.load())
            moveTo (0);
    }
    // The old buffer is freed here, on this thread, outside the lock.
}

void Player::clearSource()
{
    wantPlaying = false;
    setSource (nullptr);
    moveTo (0);
}

void Player::play()
{
    if (sourceLength.load() == 0)
        return;

    // At the end (or past the loop), start over.
    const auto end = looping.load() && loopEnd.load() > 0 ? loopEnd.load() : sourceLength.load();
    if (position.load() >= end)
        moveTo (looping.load() ? loopStart.load() : 0);

    rewindWhenSilent = false;
    wantPlaying = true;
}

void Player::pause()
{
    wantPlaying = false;
}

void Player::stop()
{
    if (wantPlaying.load())
    {
        rewindWhenSilent = true;   // after the fade-out
        wantPlaying = false;
    }
    else
    {
        moveTo (looping.load() ? loopStart.load() : 0);
    }
}

void Player::setPositionSeconds (double seconds)
{
    moveTo (juce::jlimit<juce::int64> (0, sourceLength.load(), toSamples (seconds)));
}

double Player::getPositionSeconds() const
{
    return (double) position.load() / deviceRate.load();
}

void Player::setLoop (bool on, juce::Range<double> region)
{
    if (region.getLength() > 0.01)
    {
        loopStart = toSamples (region.getStart());
        loopEnd = toSamples (region.getEnd());
    }
    else
    {
        loopStart = 0;
        loopEnd = 0;
    }
    looping = on;
}

void Player::render (juce::AudioBuffer<float>& out, int start, int num, juce::int64* positions)
{
    if (positions != nullptr)
        std::fill (positions, positions + num, (juce::int64) -1);

    const juce::SpinLock::ScopedTryLockType tl (sourceLock);
    if (! tl.isLocked() || source == nullptr)
        return;

    if (const auto seek = seekRequest.exchange (-1); seek >= 0)
        position = seek;

    const auto length = (juce::int64) source->getNumSamples();
    const bool loop = looping.load();
    const auto lStart = loop ? juce::jlimit<juce::int64> (0, length, loopStart.load()) : 0;
    const auto lEnd = loop && loopEnd.load() > lStart ? std::min (length, loopEnd.load()) : length;

    const float target = wantPlaying.load() ? 1.0f : 0.0f;
    const float step = 1.0f / (0.005f * (float) deviceRate.load());
    if (gain <= 0.0f && target <= 0.0f)
    {
        if (rewindWhenSilent.exchange (false))
            position = loop ? lStart : 0;
        return;
    }

    auto pos = position.load();
    const int outChannels = out.getNumChannels(), srcChannels = source->getNumChannels();

    for (int i = 0; i < num; ++i)
    {
        gain = target > gain ? std::min (target, gain + step) : std::max (target, gain - step);
        if (gain <= 0.0f)
            break;

        if (pos >= lEnd)
        {
            if (! loop)
            {
                wantPlaying = false;
                gain = 0.0f;
                pos = 0;
                break;
            }
            pos = lStart;
        }

        for (int ch = 0; ch < outChannels; ++ch)
            out.addSample (ch, start + i, gain * source->getSample (std::min (ch, srcChannels - 1), (int) pos));
        if (positions != nullptr && target > 0.0f)
            positions[i] = pos;
        ++pos;
    }

    position = pos;
    if (gain <= 0.0f && rewindWhenSilent.exchange (false))
        position = loop ? lStart : 0;
}

} // namespace drb
