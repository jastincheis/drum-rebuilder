#include "PluginEditor.h"
#include "Theme.h"

#include <cmath>

namespace
{
constexpr int headerHeight = 44;

juce::String formatTime (double s)
{
    const int minutes = (int) (s / 60.0);
    return juce::String (minutes) + ":" + juce::String (s - minutes * 60.0, 3).paddedLeft ('0', 6);
}

void setupLabel (juce::Label& l, juce::Colour c, float size = 14.0f)
{
    l.setColour (juce::Label::textColourId, c);
    l.setFont (juce::FontOptions (size));
    l.setInterceptsMouseClicks (false, false);
}

// Stand-in for a section that later stages fill in.
class Placeholder : public juce::Component
{
public:
    Placeholder (const juce::String& title, const juce::String& stage)
    {
        label.setText (title + "\n\n" + stage, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        setupLabel (label, theme::textDim, 18.0f);
        addAndMakeVisible (label);
    }

    void resized() override { label.setBounds (getLocalBounds()); }

private:
    juce::Label label;
};
} // namespace

// --- Import --------------------------------------------------------------

// The waveform of the file being previewed, with its playhead. Click = play
// from there.
class PreviewWave : public juce::Component
{
public:
    explicit PreviewWave (DrumRebuilderProcessor& p) : processor (p) {}

    void rebuild()
    {
        peaks.clear();
        const auto* l = processor.getPreview();
        if (l == nullptr)
        {
            duration = 0.0;
            repaint();
            return;
        }

        const auto& a = l->audio;
        const int n = a.getNumSamples(), columns = 800;
        duration = n / l->sampleRate;
        for (int c = 0; c < columns; ++c)
        {
            const int s0 = (int) ((juce::int64) n * c / columns), s1 = (int) ((juce::int64) n * (c + 1) / columns);
            float lo = 0.0f, hi = 0.0f;
            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                if (s1 > s0)
                {
                    const auto r = a.findMinMax (ch, s0, s1 - s0);
                    lo = std::min (lo, r.getStart());
                    hi = std::max (hi, r.getEnd());
                }
            peaks.emplace_back (lo, hi);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.setColour (theme::panel);
        g.fillRoundedRectangle (r, 6.0f);

        if (peaks.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (juce::FontOptions (14.0f));
            const auto error = processor.getPreviewError();
            g.drawText (error.isNotEmpty() ? error
                        : processor.getPreviewFile() != juce::File() ? juce::String ("Loading...")
                                                                     : juce::String ("Click a file on the left to hear it"),
                        getLocalBounds(), juce::Justification::centred);
            return;
        }

        const float mid = r.getCentreY(), halfH = r.getHeight() * 0.45f;
        g.setColour (theme::wave);
        for (int x = 0; x < getWidth(); ++x)
        {
            const auto& [lo, hi] = peaks[(size_t) (x * (int) peaks.size() / std::max (1, getWidth()))];
            g.drawVerticalLine (x, mid - hi * halfH, mid - lo * halfH + 1.0f);
        }

        if (duration > 0.0)
        {
            const float x = (float) (processor.getPreviewPlayer().getPositionSeconds() / duration * getWidth());
            g.setColour (theme::accent);
            g.fillRect (x - 0.75f, 0.0f, 1.5f, r.getHeight());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (duration <= 0.0)
            return;
        auto& p = processor.getPreviewPlayer();
        processor.getPlayer().pause();
        p.setPositionSeconds (juce::jlimit (0.0, duration, e.x / (double) std::max (1, getWidth()) * duration));
        p.play();
    }

private:
    DrumRebuilderProcessor& processor;
    std::vector<std::pair<float, float>> peaks;
    double duration = 0.0;
};

// A file browser on the left: click a file to hear it, double-click (or
// Enter, or "Open this beat") to analyse it.
class ImportPage : public juce::Component,
                   private juce::FileBrowserListener,
                   private juce::Timer
{
public:
    explicit ImportPage (DrumRebuilderProcessor& p)
        : processor (p),
          filter (drb::supportedWildcard(), "*", "Audio files"),
          browser (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                       | juce::FileBrowserComponent::filenameBoxIsReadOnly,
                   startFolder (p), &filter, nullptr),
          wave (p)
    {
        browser.addListener (this);
        browser.setFilenameBoxLabel ("File:");
        addAndMakeVisible (browser);
        addAndMakeVisible (wave);

        for (auto* b : { &playButton, &openButton })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        playButton.onClick = [this] { togglePreview(); };
        openButton.onClick = [this]
        {
            if (processor.getPreview() != nullptr)
                processor.loadFile (processor.getPreviewFile());
        };

        setupLabel (title, theme::text, 18.0f);
        setupLabel (details, theme::textDim, 14.0f);
        setupLabel (current, theme::text, 14.0f);
        setupLabel (help, theme::textDim, 13.0f);
        current.setJustificationType (juce::Justification::topLeft);
        help.setJustificationType (juce::Justification::topLeft);
        help.setText ("Click a file to hear it. Use the arrow keys to go through the list.\n"
                      "Double-click, press Enter or \"Open this beat\" to analyse it.\n"
                      "You can also drag a file here from your file manager.\n\n"
                      "Only drums and percussion: bass, voice or melody in the recording will confuse the analysis.",
                      juce::dontSendNotification);
        for (auto* l : { &title, &details, &current, &help })
            addAndMakeVisible (l);

        update();
        startTimerHz (30);
    }

    ~ImportPage() override { browser.removeListener (this); }

    // Space on this page plays / stops the preview; false = nothing to play.
    bool togglePreview()
    {
        if (processor.getPreview() == nullptr)
            return false;
        auto& p = processor.getPreviewPlayer();
        if (p.isPlaying())
            p.stop();
        else
        {
            processor.getPlayer().pause();
            p.play();
        }
        return true;
    }

    void update()
    {
        const auto* pv = processor.getPreview();
        const auto file = processor.getPreviewFile();
        title.setText (file != juce::File() ? file.getFileName() : juce::String ("Preview"), juce::dontSendNotification);

        juce::String d;
        if (pv != nullptr)
            d << formatTime (pv->audio.getNumSamples() / pv->sampleRate) << "   "
              << juce::String (pv->sampleRate / 1000.0, 1) << " kHz   "
              << (pv->audio.getNumChannels() == 1 ? "mono" : "stereo");
        details.setText (d, juce::dontSendNotification);

        if (pv != previewShown)
        {
            previewShown = pv;
            wave.rebuild();
        }
        wave.repaint();
        playButton.setEnabled (pv != nullptr);
        openButton.setEnabled (pv != nullptr && ! processor.isBusy());

        juce::String c;
        if (const auto* l = processor.getLoaded())
            c << "Open now: " << l->file.getFileName() << "   (" << formatTime (l->audio.getNumSamples() / l->sampleRate) << ")";
        else if (processor.getStatus().isNotEmpty())
            c = processor.getStatus();   // loading, or the last error
        current.setText (c, juce::dontSendNotification);

        // Opened from elsewhere (the Open dialog, a drop): show that folder.
        if (processor.getBrowseFolder().isDirectory() && processor.getBrowseFolder() != browser.getRoot())
            browser.setRoot (processor.getBrowseFolder());
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        browser.setBounds (r.removeFromLeft (r.getWidth() * 11 / 20));
        r.removeFromLeft (16);

        title.setBounds (r.removeFromTop (28));
        details.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        wave.setBounds (r.removeFromTop (120));
        r.removeFromTop (10);
        auto buttons = r.removeFromTop (32);
        playButton.setBounds (buttons.removeFromLeft (90));
        buttons.removeFromLeft (8);
        openButton.setBounds (buttons.removeFromLeft (140));
        r.removeFromTop (18);
        current.setBounds (r.removeFromTop (24));
        r.removeFromTop (12);
        help.setBounds (r.removeFromTop (110));
    }

private:
    DrumRebuilderProcessor& processor;
    juce::WildcardFileFilter filter;
    juce::FileBrowserComponent browser;
    PreviewWave wave;
    juce::TextButton playButton { "Play" }, openButton { "Open this beat" };
    juce::Label title, details, current, help;
    const drb::LoadedAudio* previewShown = nullptr;

    static juce::File startFolder (DrumRebuilderProcessor& p)
    {
        if (p.getBrowseFolder().isDirectory())
            return p.getBrowseFolder();
        if (const auto* l = p.getLoaded())
            return l->file.getParentDirectory();
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    }

    void selectionChanged() override
    {
        // Arrow keys through the list: hear each file.
        const auto f = browser.getHighlightedFile();
        if (f.existsAsFile() && f != processor.getPreviewFile())
            processor.previewFile (f);
    }

    void fileClicked (const juce::File& f, const juce::MouseEvent&) override
    {
        if (f.existsAsFile())
            processor.previewFile (f);
    }

    void fileDoubleClicked (const juce::File& f) override
    {
        if (f.existsAsFile())
            processor.loadFile (f);
    }

    void browserRootChanged (const juce::File& folder) override { processor.setBrowseFolder (folder); }

    void timerCallback() override
    {
        const auto& p = processor.getPreviewPlayer();
        const auto text = p.isPlaying() ? "Stop" : "Play";
        if (playButton.getButtonText() != text)
            playButton.setButtonText (text);
        if (p.isPlaying() || playheadWasMoving)
            wave.repaint();
        playheadWasMoving = p.isPlaying();
    }

    bool playheadWasMoving = false;
};

// --- Analysis ------------------------------------------------------------

class AnalysisPage : public juce::Component
{
public:
    explicit AnalysisPage (DrumRebuilderProcessor& p) : processor (p), waveform (p)
    {
        addAndMakeVisible (waveform);
        for (auto* b : { &zoomOut, &zoomIn, &fit })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        zoomIn.onClick = [this] { waveform.zoomBy (2.0, waveform.zoomCentre()); };
        zoomOut.onClick = [this] { waveform.zoomBy (0.5, waveform.zoomCentre()); };
        fit.onClick = [this] { waveform.zoomToFit(); };

        setupLabel (info, theme::text);
        setupLabel (hint, theme::textDim, 12.5f);
        hint.setText ("Click: play from there   Double-click: one hit   Drag: region   Right-click: bar 1   Wheel, Up / Down: zoom   Left / Right: hits   Space: play   W / E: start / end",
                      juce::dontSendNotification);
        hint.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (info);
        addAndMakeVisible (hint);
    }

    void update()
    {
        waveform.refresh();
        const auto& a = processor.getAnalysis();
        juce::String text;
        if (processor.getLoaded() != nullptr)
        {
            text << a.onsets.size() << " hits found";
            if (a.grid.bpm > 0.0)
                text << "    tempo confidence " << juce::roundToInt (a.grid.confidence * 100.0f) << "%"
                     << "    bar 1 at " << juce::String (a.grid.firstDownbeat, 3) << " s";
            else
                text << "    no steady tempo found";
        }
        info.setText (text, juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto bottom = r.removeFromBottom (30);
        r.removeFromBottom (6);
        waveform.setBounds (r);

        for (auto* b : { &zoomOut, &zoomIn, &fit })
        {
            b->setBounds (bottom.removeFromLeft (b == &fit ? 52 : 34).reduced (0, 2));
            bottom.removeFromLeft (4);
        }
        bottom.removeFromLeft (12);
        info.setBounds (bottom.removeFromLeft (bottom.getWidth() / 2));
        hint.setBounds (bottom);
    }

    WaveformView& getWaveform() { return waveform; }

private:
    DrumRebuilderProcessor& processor;
    WaveformView waveform;
    juce::TextButton zoomOut { "-" }, zoomIn { "+" }, fit { "Fit" };
    juce::Label info, hint;
};

// --- Editor --------------------------------------------------------------

DrumRebuilderEditor::DrumRebuilderEditor (DrumRebuilderProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::windowBackground, theme::background);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::widgetBackground, theme::panelLight);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::outline, theme::panelLight);
    scheme.setUIColour (juce::LookAndFeel_V4::ColourScheme::defaultFill, theme::accent);
    lookAndFeel.setColourScheme (scheme);
    lookAndFeel.setColour (juce::TextButton::buttonColourId, theme::panelLight);
    lookAndFeel.setColour (juce::TextButton::buttonOnColourId, theme::accent);
    lookAndFeel.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    lookAndFeel.setColour (juce::ScrollBar::thumbColourId, theme::panelLight.brighter (0.25f));
    setLookAndFeel (&lookAndFeel);

    for (auto* b : { &openButton, &playButton, &stopButton, &loopButton, &clickButton, &halfButton, &doubleButton, &barOneButton })
    {
        b->setWantsKeyboardFocus (false);
        addAndMakeVisible (b);
    }
    loopButton.setClickingTogglesState (false);
    clickButton.setTooltip ("Metronome: clicks on the detected beats, higher on the first beat of each bar");
    halfButton.setTooltip ("Tempo detected twice too fast? Halve it");
    doubleButton.setTooltip ("Tempo detected twice too slow? Double it");
    barOneButton.setTooltip ("Bar 1 starts where the playhead is (on the hit right there). Or right-click the waveform");

    openButton.onClick = [this] { openFileChooser(); };
    playButton.onClick = [this] { togglePlay(); };
    stopButton.onClick = [this] { processor.getPlayer().stop(); };
    loopButton.onClick = [this] { processor.setLooping (! processor.isLooping()); };
    clickButton.onClick = [this] { processor.setMetronome (! processor.isMetronomeOn()); };
    halfButton.onClick = [this] { processor.scaleTempo (0.5); };
    doubleButton.onClick = [this] { processor.scaleTempo (2.0); };
    barOneButton.onClick = [this]
    {
        processor.getPlayer().setPositionSeconds (processor.setFirstDownbeat (processor.getPlayer().getPositionSeconds()));
    };

    setupLabel (fileLabel, theme::text);
    setupLabel (timeLabel, theme::text, 15.0f);
    setupLabel (tempoLabel, theme::accent, 15.0f);
    setupLabel (statusLabel, theme::textDim, 13.0f);
    timeLabel.setJustificationType (juce::Justification::centred);
    for (auto* l : { &fileLabel, &timeLabel, &tempoLabel, &statusLabel })
        addAndMakeVisible (l);

    importPage = new ImportPage (processor);
    analysisPage = new AnalysisPage (processor);

    tabs.addTab ("Import", theme::panel, importPage, true);
    tabs.addTab ("Analysis", theme::panel, analysisPage, true);
    const std::pair<const char*, const char*> later[] {
        { "MIDI Editor",    "One lane per instrument, editable - stage 2" },
        { "Sample Browser", "Your sample folders, one per instrument - stage 3" },
        { "Drum Rack",      "Volume, pan, pitch, mute, solo, Reverb and Delay per instrument - stages 3 and 5" },
        { "Master",         "Master Reverb and Delay, A/B with the original - stages 3 and 5" },
        { "Export",         "MIDI, stereo WAV and stems - stages 2, 3 and 5" },
    };
    for (const auto& [name, stage] : later)
        tabs.addTab (name, theme::panel, new Placeholder (name, stage), true);
    tabs.setTabBarDepth (32);
    tabs.setOutline (0);
    addAndMakeVisible (tabs);

    processor.addChangeListener (this);
    setWantsKeyboardFocus (true);
    setResizable (true, true);
    setResizeLimits (900, 500, 3000, 2000);
    setSize (1200, 720);

    shownId = processor.getLoadedId();
    if (processor.getLoaded() != nullptr)
        tabs.setCurrentTabIndex (1);
    updateHeader();
    startTimerHz (20);
}

DrumRebuilderEditor::~DrumRebuilderEditor()
{
    processor.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void DrumRebuilderEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    g.setColour (theme::panel);
    g.fillRect (getLocalBounds().removeFromTop (headerHeight));
}

void DrumRebuilderEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (headerHeight).reduced (8, 7);

    openButton.setBounds (header.removeFromLeft (80));
    header.removeFromLeft (10);

    auto right = header.removeFromRight (706);
    for (auto* b : { &stopButton, &playButton, &loopButton, &clickButton })
    {
        b->setBounds (right.removeFromLeft (b == &playButton ? 64 : 52));
        right.removeFromLeft (4);
    }
    timeLabel.setBounds (right.removeFromLeft (92));
    barOneButton.setBounds (right.removeFromRight (86));
    right.removeFromRight (4);
    doubleButton.setBounds (right.removeFromRight (34));
    right.removeFromRight (4);
    halfButton.setBounds (right.removeFromRight (34));
    right.removeFromRight (8);
    tempoLabel.setBounds (right);
    tempoLabel.setJustificationType (juce::Justification::centredRight);

    statusLabel.setBounds (header.removeFromRight (std::min (260, header.getWidth() / 2)));
    fileLabel.setBounds (header);

    tabs.setBounds (r);
}

bool DrumRebuilderEditor::keyPressed (const juce::KeyPress& key)
{
    const bool onImport = tabs.getCurrentContentComponent() == importPage;
    if (key == juce::KeyPress::spaceKey)
    {
        if (! (onImport && importPage->togglePreview()))
            togglePlay();
        return true;
    }

    // W / E: to the start / end of the beat (playing goes on from there).
    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
    if ((c == 'w' || c == 'e') && processor.getLoaded() != nullptr)
    {
        const auto* l = processor.getLoaded();
        const double t = c == 'w' ? 0.0 : std::max (0.0, l->audio.getNumSamples() / l->sampleRate - 0.001);
        processor.getPlayer().setPositionSeconds (t);
        analysisPage->getWaveform().scrollToShow (t);
        return true;
    }

    // Left / right: to the previous / next hit.
    if (! onImport && processor.getLoaded() != nullptr
        && (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey))
    {
        analysisPage->getWaveform().jumpToHit (key == juce::KeyPress::leftKey ? -1 : 1);
        return true;
    }

    // Up / down: zoom in / out, around the playhead when it is in view.
    if (! onImport && (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey))
    {
        tabs.setCurrentTabIndex (1);
        auto& w = analysisPage->getWaveform();
        w.zoomBy (key == juce::KeyPress::upKey ? 2.0 : 0.5, w.zoomCentre());
        return true;
    }
    return false;
}

bool DrumRebuilderEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && drb::isSupported (juce::File (files[0]));
}

void DrumRebuilderEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty())
        processor.loadFile (juce::File (files[0]));
}

void DrumRebuilderEditor::openFileChooser()
{
    const auto start = processor.getLoaded() != nullptr ? processor.getLoaded()->file.getParentDirectory()
                                                        : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Open a drums-only recording", start, drb::supportedWildcard());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file.existsAsFile())
                                  processor.loadFile (file);
                          });
}

void DrumRebuilderEditor::togglePlay()
{
    auto& player = processor.getPlayer();
    if (player.isPlaying())
        player.pause();
    else
    {
        processor.stopPreview();
        player.play();
    }
    updateHeader();
}

void DrumRebuilderEditor::updateHeader()
{
    const auto* l = processor.getLoaded();
    fileLabel.setText (l != nullptr ? l->file.getFileName() : juce::String ("No recording"), juce::dontSendNotification);
    statusLabel.setText (processor.isBusy() || l == nullptr ? processor.getStatus() : juce::String(),
                         juce::dontSendNotification);

    const auto& grid = processor.getAnalysis().grid;
    juce::String tempo;
    if (l != nullptr && grid.bpm > 0.0)
        tempo << juce::String (grid.bpm, 1) << " BPM   " << grid.bars << (grid.bars == 1 ? " bar   " : " bars   ")
              << grid.timeSignature.beatsPerBar << "/" << grid.timeSignature.beatUnit
              << (processor.isTempoManual() ? "  (edited)" : "");
    else if (l != nullptr)
        tempo = "No steady tempo";
    tempoLabel.setText (tempo, juce::dontSendNotification);

    const bool hasTempo = l != nullptr && grid.bpm > 0.0;
    halfButton.setEnabled (hasTempo);
    doubleButton.setEnabled (hasTempo);
    barOneButton.setEnabled (hasTempo);
    for (auto* b : { &playButton, &stopButton, &loopButton })
        b->setEnabled (l != nullptr);
    clickButton.setEnabled (hasTempo);

    playButton.setButtonText (processor.getPlayer().isPlaying() ? "Pause" : "Play");
    loopButton.setToggleState (processor.isLooping(), juce::dontSendNotification);
    clickButton.setToggleState (processor.isMetronomeOn(), juce::dontSendNotification);
}

void DrumRebuilderEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateHeader();
    importPage->update();
    analysisPage->update();

    if (processor.getLoadedId() != shownId)
    {
        shownId = processor.getLoadedId();
        tabs.setCurrentTabIndex (1);
    }
}

void DrumRebuilderEditor::timerCallback()
{
    // Space works as soon as the window is up: the host doesn't give the
    // editor keyboard focus by itself.
    if (! focusTaken && isShowing() && getPeer() != nullptr && getPeer()->isFocused())
    {
        grabKeyboardFocus();
        focusTaken = hasKeyboardFocus (true);
    }

    const auto& player = processor.getPlayer();
    juce::String t = formatTime (player.getPositionSeconds());

    // Where we are in the music: bar.beat
    const auto& grid = processor.getAnalysis().grid;
    if (processor.getLoaded() != nullptr && grid.bpm > 0.0)
    {
        const double beats = (player.getPositionSeconds() - grid.firstDownbeat) / grid.beatSeconds();
        if (beats >= 0.0)
        {
            const int perBar = grid.timeSignature.beatsPerBar;
            const int whole = (int) std::floor (beats);
            t << "\n" << (whole / perBar + 1) << "." << (whole % perBar + 1);
        }
    }
    timeLabel.setText (t, juce::dontSendNotification);

    const auto playText = player.isPlaying() ? "Pause" : "Play";
    if (playButton.getButtonText() != playText)
        playButton.setButtonText (playText);
}
