#include "PluginEditor.h"
#include "Focus.h"
#include "MidiEditorView.h"
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

// --- Solo ----------------------------------------------------------------

// One button per instrument found, with its count. A click puts it on solo
// (or takes it off): Space then plays only the solo instruments' hits, cut
// from the recording in their own rhythm, and only they keep their colour on
// the waveform.
class SoloBar : public juce::Component, private juce::Timer
{
public:
    explicit SoloBar (DrumRebuilderProcessor& p) : processor (p)
    {
        setupLabel (status, theme::textDim, 13.0f);
        status.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (status);
        startTimerHz (10);
    }

    std::function<void (std::vector<drb::Instrument>)> onFocus;

    void update()
    {
        found.clear();
        for (auto inst : drb::allInstruments)
            if (count (inst) > 0)
                found.push_back (inst);
        refresh();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);

        chips.clear();
        auto row = getLocalBounds().reduced (10, 7);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.setColour (theme::textDim);
        g.drawText ("Solo:", row.removeFromLeft (44), juce::Justification::centredLeft);

        for (auto inst : found)
        {
            const juce::String text = juce::String (drb::instrumentName (inst)) + "  " + juce::String (count (inst));
            const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 26;
            const auto chip = row.removeFromLeft (w);
            row.removeFromLeft (8);
            chips.push_back (chip);

            const bool on = processor.isSolo (inst);
            const auto colour = theme::instrument (inst);
            g.setColour (on ? colour : theme::panelLight);
            g.fillRoundedRectangle (chip.toFloat(), 12.0f);
            g.setColour (colour);
            g.drawRoundedRectangle (chip.toFloat().reduced (0.75f), 12.0f, 1.5f);
            g.setColour (on ? juce::Colours::black : theme::text);
            g.drawText (text, chip, juce::Justification::centred);
        }
    }

    void resized() override
    {
        status.setBounds (getLocalBounds().reduced (10, 0).removeFromRight (std::max (260, getWidth() / 3)));
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        for (size_t k = 0; k < chips.size(); ++k)
            if (chips[k].contains (e.getPosition()))
            {
                processor.setSolo (found[k], ! processor.isSolo (found[k]));
                refresh();
            }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        bool over = false;
        for (const auto& c : chips)
            over = over || c.contains (e.getPosition());
        setMouseCursor (over ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

private:
    DrumRebuilderProcessor& processor;
    std::vector<drb::Instrument> found;
    std::vector<juce::Rectangle<int>> chips;
    juce::Label status;
    bool wasPlaying = false;

    int count (drb::Instrument inst) const
    {
        int n = 0;
        for (const auto& h : processor.getAnalysis().hits)
            n += h.instrument == inst ? 1 : 0;
        return n;
    }

    void refresh()
    {
        const auto soloed = processor.getSolo();
        juce::String text;
        if (soloed.empty())
            text = "Click instruments to solo them, then Space. Wrong name on a hit? Click its label.";
        else
        {
            juce::StringArray names;
            for (auto i : soloed)
                names.add (drb::instrumentName (i));
            text = (processor.isSoloPlaying() ? "Playing only: " : "Solo: ") + names.joinIntoString (", ") + "    Space: play / stop";
        }
        status.setText (text, juce::dontSendNotification);
        if (onFocus)
            onFocus (soloed);
        repaint();
    }

    void timerCallback() override
    {
        if (processor.isSoloPlaying() != wasPlaying)
        {
            wasPlaying = processor.isSoloPlaying();
            refresh();
        }
    }
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
        hint.setText ("Click a marker: what it really is   Click: play from there   Double-click: one hit   Drag: region   Right-click: more   Wheel, Up / Down: zoom   Left / Right: hits   Space: play   W / E: start / end",
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

// --- Instruments ---------------------------------------------------------

// Checking what was recognised: every hit's instruments are written on the
// waveform (click one to say what it really is), and each instrument can be
// heard on its own.
class InstrumentsPage : public juce::Component
{
public:
    explicit InstrumentsPage (DrumRebuilderProcessor& p) : processor (p), listen (p), waveform (p)
    {
        waveform.setInstrumentMode (true);
        listen.onFocus = [this] (std::vector<drb::Instrument> i) { waveform.setFocus (std::move (i)); };
        addAndMakeVisible (listen);
        addAndMakeVisible (waveform);

        for (auto* b : { &zoomOut, &zoomIn, &fit })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        zoomIn.onClick = [this] { waveform.zoomBy (2.0, waveform.zoomCentre()); };
        zoomOut.onClick = [this] { waveform.zoomBy (0.5, waveform.zoomCentre()); };
        fit.onClick = [this] { waveform.zoomToFit(); };

        setupLabel (hint, theme::textDim, 12.5f);
        hint.setText ("Click a label: what it really is    Right-click the waveform: add a hit    Drag a marker: move it"
                      "    Wheel, Up / Down: zoom    Space: play / stop (only the solo ones)",
                      juce::dontSendNotification);
        hint.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (hint);
    }

    void update()
    {
        waveform.refresh();
        listen.update();
    }

    WaveformView& getWaveform() { return waveform; }

    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto bottom = r.removeFromBottom (30);
        r.removeFromBottom (6);
        listen.setBounds (r.removeFromTop (40));
        r.removeFromTop (6);
        waveform.setBounds (r);

        for (auto* b : { &zoomOut, &zoomIn, &fit })
        {
            b->setBounds (bottom.removeFromLeft (b == &fit ? 52 : 34).reduced (0, 2));
            bottom.removeFromLeft (4);
        }
        bottom.removeFromLeft (12);
        hint.setBounds (bottom);
    }

private:
    DrumRebuilderProcessor& processor;
    SoloBar listen;
    WaveformView waveform;
    juce::TextButton zoomOut { "-" }, zoomIn { "+" }, fit { "Fit" };
    juce::Label hint;
};

// --- MIDI Editor ---------------------------------------------------------

class MidiEditorPage : public juce::Component
{
public:
    explicit MidiEditorPage (DrumRebuilderProcessor& p) : processor (p), view (p)
    {
        addAndMakeVisible (view);

        setupLabel (gridLabel, theme::textDim);
        gridLabel.setText ("Grid", juce::dontSendNotification);
        addAndMakeVisible (gridLabel);
        const auto& divisions = drb::gridDivisions();
        for (size_t i = 0; i < divisions.size(); ++i)
            gridBox.addItem (divisions[i].name, (int) i + 1);
        gridBox.setSelectedId (3, juce::dontSendNotification);   // 1/16
        gridBox.onChange = [this] { view.setDivision (division()); };
        gridBox.setWantsKeyboardFocus (false);
        addAndMakeVisible (gridBox);

        snapButton.setToggleState (true, juce::dontSendNotification);
        snapButton.setTooltip ("New and moved hits land on the grid (hold Alt for off the grid)");
        snapButton.onClick = [this] { view.setSnap (snapButton.getToggleState()); };
        addAndMakeVisible (snapButton);

        strength.setRange (0.0, 100.0, 1.0);
        strength.setValue (100.0, juce::dontSendNotification);
        strength.setTextValueSuffix (" %");
        strength.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 22);
        strength.setTooltip ("How far Quantize moves the hits: 100 % = onto the grid, 50 % = halfway (keeps some groove)");
        strength.setWantsKeyboardFocus (false);
        strength.setColour (juce::Slider::backgroundColourId, theme::panelLight);
        strength.setColour (juce::Slider::trackColourId, theme::accent.withAlpha (0.7f));
        addAndMakeVisible (strength);

        for (auto* b : { &quantizeButton, &undoButton, &redoButton, &zoomOut, &zoomIn, &fit })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        quantizeButton.setTooltip ("Move every hit towards the grid");
        quantizeButton.onClick = [this] { processor.quantizeHits (division(), strength.getValue() / 100.0); };
        undoButton.onClick = [this] { processor.undo(); };
        redoButton.onClick = [this] { processor.redo(); };
        zoomIn.onClick = [this] { view.zoomBy (2.0, view.zoomCentre()); };
        zoomOut.onClick = [this] { view.zoomBy (0.5, view.zoomCentre()); };
        fit.onClick = [this] { view.zoomToFit(); };

        setupLabel (hint, theme::textDim, 12.5f);
        hint.setText ("Click an empty spot: add a hit    Drag: move it (onto another row: change the instrument, Alt: off the grid)"
                      "    Double-click or Delete: remove    Right-click: how hard    Ctrl+Z: undo",
                      juce::dontSendNotification);
        addAndMakeVisible (hint);
        update();
    }

    void update()
    {
        view.refresh();
        undoButton.setEnabled (processor.canUndo());
        redoButton.setEnabled (processor.canRedo());
        quantizeButton.setEnabled (processor.getLoaded() != nullptr && processor.getAnalysis().grid.bpm > 0.0);
    }

    MidiEditorView& getView() { return view; }

    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto bar = r.removeFromTop (30);
        r.removeFromTop (6);
        hint.setBounds (r.removeFromBottom (22));
        r.removeFromBottom (4);
        view.setBounds (r);

        gridLabel.setBounds (bar.removeFromLeft (36));
        gridBox.setBounds (bar.removeFromLeft (80).reduced (0, 2));
        bar.removeFromLeft (8);
        snapButton.setBounds (bar.removeFromLeft (70));
        bar.removeFromLeft (16);
        quantizeButton.setBounds (bar.removeFromLeft (84).reduced (0, 1));
        bar.removeFromLeft (6);
        strength.setBounds (bar.removeFromLeft (190));
        bar.removeFromLeft (16);
        undoButton.setBounds (bar.removeFromLeft (60).reduced (0, 1));
        bar.removeFromLeft (4);
        redoButton.setBounds (bar.removeFromLeft (60).reduced (0, 1));

        fit.setBounds (bar.removeFromRight (52).reduced (0, 1));
        bar.removeFromRight (4);
        zoomIn.setBounds (bar.removeFromRight (34).reduced (0, 1));
        bar.removeFromRight (4);
        zoomOut.setBounds (bar.removeFromRight (34).reduced (0, 1));
    }

private:
    DrumRebuilderProcessor& processor;
    MidiEditorView view;
    juce::Label gridLabel, hint;
    juce::ComboBox gridBox;
    juce::ToggleButton snapButton { "Snap" };
    juce::Slider strength { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton quantizeButton { "Quantize" }, undoButton { "Undo" }, redoButton { "Redo" };
    juce::TextButton zoomOut { "-" }, zoomIn { "+" }, fit { "Fit" };

    double division() const
    {
        const auto& divisions = drb::gridDivisions();
        return divisions[(size_t) juce::jlimit (0, (int) divisions.size() - 1, gridBox.getSelectedId() - 1)].beats;
    }
};

// --- Export --------------------------------------------------------------

class ExportPage : public juce::Component
{
public:
    explicit ExportPage (DrumRebuilderProcessor& p) : processor (p)
    {
        midiButton.setWantsKeyboardFocus (false);
        midiButton.onClick = [this] { exportMidi(); };
        addAndMakeVisible (midiButton);

        setupLabel (title, theme::text, 18.0f);
        title.setText ("MIDI", juce::dontSendNotification);
        setupLabel (info, theme::text, 14.0f);
        info.setJustificationType (juce::Justification::topLeft);
        setupLabel (result, theme::accent, 14.0f);
        setupLabel (later, theme::textDim, 14.0f);
        later.setJustificationType (juce::Justification::topLeft);
        later.setText ("Stereo WAV of the rebuilt beat - stage 3\nStems, one WAV per instrument with the Reverb and Delay tails - stage 5",
                       juce::dontSendNotification);
        for (auto* l : { &title, &info, &result, &later })
            addAndMakeVisible (l);
        update();
    }

    void update()
    {
        const auto& a = processor.getAnalysis();
        juce::String text;
        if (processor.getLoaded() == nullptr)
            text = "Open a drums-only recording first.";
        else
        {
            text << "One track per instrument, General MIDI drum notes (kick 36, snare 38, hi-hats 42 / 46) on channel 10,\n"
                 << "at " << juce::String (a.grid.bpm, 1) << " BPM, starting at bar 1 (one bar earlier if hits come before it).\n"
                 << "The hits go out exactly as they are now: quantize them in the MIDI Editor first if you want them on the grid.\n\n";
            for (auto inst : drb::allInstruments)
            {
                int n = 0;
                for (const auto& h : a.hits)
                    n += h.instrument == inst ? 1 : 0;
                if (n > 0)
                    text << drb::instrumentName (inst) << ": " << n << "    ";
            }
        }
        info.setText (text, juce::dontSendNotification);
        midiButton.setEnabled (processor.getLoaded() != nullptr && ! a.hits.empty());
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (24);
        title.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        info.setBounds (r.removeFromTop (100));
        r.removeFromTop (8);
        auto row = r.removeFromTop (32);
        midiButton.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (12);
        result.setBounds (row);
        r.removeFromTop (40);
        later.setBounds (r.removeFromTop (60));
    }

private:
    DrumRebuilderProcessor& processor;
    juce::TextButton midiButton { "Export MIDI..." };
    juce::Label title, info, result, later;
    std::unique_ptr<juce::FileChooser> chooser;

    void exportMidi()
    {
        const auto* l = processor.getLoaded();
        if (l == nullptr)
            return;
        const auto suggested = l->file.getParentDirectory().getChildFile (l->file.getFileNameWithoutExtension() + " drums.mid");
        chooser = std::make_unique<juce::FileChooser> ("Save the drums as MIDI", suggested, "*.mid");
        juce::Component::SafePointer<ExportPage> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [self] (const juce::FileChooser& fc)
                              {
                                  if (self == nullptr || fc.getResult() == juce::File())
                                      return;
                                  auto file = fc.getResult().withFileExtension ("mid");
                                  const auto& a = self->processor.getAnalysis();
                                  const bool ok = drb::writeMidiFile (file, a.hits, a.grid);
                                  self->result.setText (ok ? "Saved " + file.getFileName() : "Could not save " + file.getFullPathName(),
                                                        juce::dontSendNotification);
                              });
    }
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
    stopButton.onClick = [this]
    {
        processor.pauseSolo();
        processor.getPlayer().stop();
    };
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
    instrumentsPage = new InstrumentsPage (processor);
    tabs.addTab ("Instruments", theme::panel, instrumentsPage, true);
    midiPage = new MidiEditorPage (processor);
    tabs.addTab ("MIDI Editor", theme::panel, midiPage, true);
    const std::pair<const char*, const char*> later[] {
        { "Sample Browser", "Your sample folders, one per instrument - stage 3" },
        { "Drum Rack",      "Volume, pan, pitch, mute, solo, Reverb and Delay per instrument - stages 3 and 5" },
        { "Master",         "Master Reverb and Delay, A/B with the original - stages 3 and 5" },
    };
    for (const auto& [name, stage] : later)
        tabs.addTab (name, theme::panel, new Placeholder (name, stage), true);
    exportPage = new ExportPage (processor);
    tabs.addTab ("Export", theme::panel, exportPage, true);
    tabs.setTabBarDepth (32);
    tabs.setOutline (0);
    addAndMakeVisible (tabs);

    processor.addChangeListener (this);
    setWantsKeyboardFocus (true);
    addMouseListener (this, true);   // any click inside: see mouseDown
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

// A click anywhere in the window gives it the keyboard back, if it lost it.
void DrumRebuilderEditor::mouseDown (const juce::MouseEvent&)
{
    focus::takeBack (this);
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
    const bool onMidi = tabs.getCurrentContentComponent() == midiPage;
    auto& waveform = tabs.getCurrentContentComponent() == instrumentsPage ? instrumentsPage->getWaveform()
                                                                          : analysisPage->getWaveform();

    // Ctrl / Cmd + Z: undo the last change to the hits; + Shift, or Y: redo.
    if (key.getModifiers().isCommandDown())
    {
        const auto k = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getKeyCode());
        if (k == 'z' && ! key.getModifiers().isShiftDown())
            processor.undo();
        else if (k == 'z' || k == 'y')
            processor.redo();
        else
            return false;
        return true;
    }

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
        waveform.scrollToShow (t);
        return true;
    }

    // Left / right: to the previous / next hit.
    if (! onImport && processor.getLoaded() != nullptr
        && (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey))
    {
        waveform.jumpToHit (key == juce::KeyPress::leftKey ? -1 : 1);
        return true;
    }

    // Up / down: zoom in / out, around the playhead when it is in view.
    if (! onImport && (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey))
    {
        const double factor = key == juce::KeyPress::upKey ? 2.0 : 0.5;
        if (onMidi)
            midiPage->getView().zoomBy (factor, midiPage->getView().zoomCentre());
        else
        {
            if (tabs.getCurrentContentComponent() != instrumentsPage)
                tabs.setCurrentTabIndex (1);
            waveform.zoomBy (factor, waveform.zoomCentre());
        }
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
    // On the Instruments tab with instruments on solo, only they play.
    auto& player = processor.getPlayer();
    if (processor.isSoloPlaying())
        processor.pauseSolo();
    else if (player.isPlaying())
        player.pause();
    else if (tabs.getCurrentContentComponent() == instrumentsPage && processor.anySolo())
        processor.playSolo();
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

    playButton.setButtonText (processor.getPlayer().isPlaying() || processor.isSoloPlaying() ? "Pause" : "Play");
    loopButton.setToggleState (processor.isLooping(), juce::dontSendNotification);
    clickButton.setToggleState (processor.isMetronomeOn(), juce::dontSendNotification);
}

void DrumRebuilderEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateHeader();
    importPage->update();
    analysisPage->update();
    instrumentsPage->update();
    midiPage->update();
    exportPage->update();

    if (processor.getLoadedId() != shownId)
    {
        shownId = processor.getLoadedId();
        tabs.setCurrentTabIndex (1);
    }
}

void DrumRebuilderEditor::timerCallback()
{
    // Space works as soon as the window is up, and again after a menu or a
    // dialog closed: the host doesn't give the editor keyboard focus by
    // itself, and a closed menu leaves nothing focused.
    if (isShowing() && getPeer() != nullptr && getPeer()->isFocused() && ! hasKeyboardFocus (true))
        grabKeyboardFocus();

    const auto& player = processor.getPlayer();
    const double position = processor.isSoloPlaying() ? processor.getAuditionPlayer().getPositionSeconds()
                                                      : player.getPositionSeconds();
    juce::String t = formatTime (position);

    // Where we are in the music: bar.beat
    const auto& grid = processor.getAnalysis().grid;
    if (processor.getLoaded() != nullptr && grid.bpm > 0.0)
    {
        const double beats = (position - grid.firstDownbeat) / grid.beatSeconds();
        if (beats >= 0.0)
        {
            const int perBar = grid.timeSignature.beatsPerBar;
            const int whole = (int) std::floor (beats);
            t << "\n" << (whole / perBar + 1) << "." << (whole % perBar + 1);
        }
    }
    timeLabel.setText (t, juce::dontSendNotification);

    const auto playText = player.isPlaying() || processor.isSoloPlaying() ? "Pause" : "Play";
    if (playButton.getButtonText() != playText)
        playButton.setButtonText (playText);
}
