#include "PluginProcessor.h"
#include "PluginEditor.h"

DrumRebuilderProcessor::DrumRebuilderProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void DrumRebuilderProcessor::prepareToPlay (double, int)
{
}

bool DrumRebuilderProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void DrumRebuilderProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();   // the sampler and the original-audio player arrive in stage 1 and 3
}

juce::AudioProcessorEditor* DrumRebuilderProcessor::createEditor()
{
    return new DrumRebuilderEditor (*this);
}

void DrumRebuilderProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void DrumRebuilderProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        state = juce::ValueTree::fromXml (*xml);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DrumRebuilderProcessor();
}
