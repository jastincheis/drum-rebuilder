#include "PluginEditor.h"

namespace
{
const juce::Colour background (0xff1e1f24);
const juce::Colour tabColour (0xff2a2c33);

// Stand-in for a section that later stages fill in.
class Placeholder : public juce::Component
{
public:
    Placeholder (const juce::String& title, const juce::String& stage)
    {
        label.setText (title + "\n\n" + stage, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.55f));
        label.setFont (juce::FontOptions (18.0f));
        addAndMakeVisible (label);
    }

    void resized() override { label.setBounds (getLocalBounds()); }

private:
    juce::Label label;
};
} // namespace

DrumRebuilderEditor::DrumRebuilderEditor (DrumRebuilderProcessor& p)
    : AudioProcessorEditor (&p)
{
    const std::pair<const char*, const char*> sections[] {
        { "Import",         "Open a drum recording (WAV, AIFF, FLAC, MP3) - stage 1" },
        { "Analysis",       "Tempo, bars and detected hits - stages 1-2" },
        { "MIDI Editor",    "One lane per instrument, editable - stage 2" },
        { "Sample Browser", "Your sample folders, one per instrument - stage 3" },
        { "Drum Rack",      "Volume, pan, pitch, mute, solo, Reverb and Delay per instrument - stages 3 and 5" },
        { "Master",         "Master Reverb and Delay, A/B with the original - stages 3 and 5" },
        { "Export",         "MIDI, stereo WAV and stems - stages 2, 3 and 5" },
    };

    for (const auto& [name, stage] : sections)
        tabs.addTab (name, tabColour, new Placeholder (name, stage), true);

    tabs.setTabBarDepth (34);
    addAndMakeVisible (tabs);

    setResizable (true, true);
    setResizeLimits (800, 500, 2400, 1600);
    setSize (1100, 700);
}

DrumRebuilderEditor::~DrumRebuilderEditor() = default;

void DrumRebuilderEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
}

void DrumRebuilderEditor::resized()
{
    tabs.setBounds (getLocalBounds());
}
