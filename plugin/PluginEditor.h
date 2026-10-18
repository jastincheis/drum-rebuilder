#pragma once

#include "PluginProcessor.h"
#include "WaveformView.h"

class ImportPage;
class AnalysisPage;

class DrumRebuilderEditor : public juce::AudioProcessorEditor,
                            public juce::FileDragAndDropTarget,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    explicit DrumRebuilderEditor (DrumRebuilderProcessor&);
    ~DrumRebuilderEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    void openFileChooser();

private:
    DrumRebuilderProcessor& processor;
    juce::LookAndFeel_V4 lookAndFeel;

    // Header, visible from every section.
    juce::TextButton openButton { "Open..." };
    juce::Label fileLabel;
    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, loopButton { "Loop" }, clickButton { "Click" };
    juce::Label timeLabel, tempoLabel;
    juce::TextButton halfButton { "1/2" }, doubleButton { "x2" }, barOneButton { "Bar 1 here" };
    juce::Label statusLabel;

    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    ImportPage* importPage = nullptr;       // owned by tabs
    AnalysisPage* analysisPage = nullptr;   // owned by tabs

    std::unique_ptr<juce::FileChooser> chooser;
    int shownId = -1;
    bool focusTaken = false;

    void togglePlay();
    void updateHeader();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumRebuilderEditor)
};
