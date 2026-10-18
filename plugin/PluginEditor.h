#pragma once

#include "PluginProcessor.h"

class DrumRebuilderEditor : public juce::AudioProcessorEditor
{
public:
    explicit DrumRebuilderEditor (DrumRebuilderProcessor&);
    ~DrumRebuilderEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumRebuilderEditor)
};
