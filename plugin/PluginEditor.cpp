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
    // inDialog: the page inside the Open window, which closes once a beat opens.
    explicit ImportPage (DrumRebuilderProcessor& p, bool inDialogWindow = false)
        : processor (p),
          inDialog (inDialogWindow),
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
                open (processor.getPreviewFile());
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

        if (inDialog)
            setWantsKeyboardFocus (true);
        update();
        startTimerHz (30);
    }

    ~ImportPage() override
    {
        browser.removeListener (this);
        if (inDialog)
            processor.stopPreview();
    }

    std::function<void()> onOpened;   // a beat was opened from here

    // In the Open window Space is ours (in the editor, the editor's).
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (inDialog && key == juce::KeyPress::spaceKey)
        {
            togglePreview();
            return true;
        }
        return false;
    }

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
    const bool inDialog;
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
            open (f);
    }

    void open (const juce::File& f)
    {
        processor.loadFile (f);
        if (onOpened != nullptr)
            onOpened();
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
        if (inDialog && ++ticks % 6 == 0)   // the editor updates its own page
            update();
    }

    bool playheadWasMoving = false;
    int ticks = 0;
};

// The Open window: the Import page's browser over the whole editor, inside
// it (a separate window gets tiled by some window managers and hides behind
// plugin hosts). Click a file = hear it; opening a beat, Close or Escape
// shut it.
class OpenPanel : public juce::Component
{
public:
    explicit OpenPanel (DrumRebuilderProcessor& p) : page (p, true)
    {
        setupLabel (title, theme::text, 18.0f);
        title.setText ("Open a drums-only recording", juce::dontSendNotification);
        closeButton.setWantsKeyboardFocus (false);
        closeButton.onClick = [this] { close(); };
        page.onOpened = [this] { close(); };
        addAndMakeVisible (title);
        addAndMakeVisible (closeButton);
        addAndMakeVisible (page);
        setWantsKeyboardFocus (true);
    }

    std::function<void()> onClose;

    void close()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<OpenPanel> (this)]
                                         {
                                             if (safe != nullptr && safe->onClose != nullptr)
                                                 safe->onClose();
                                         });
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            close();
            return true;
        }
        if (key == juce::KeyPress::spaceKey)
        {
            page.togglePreview();
            return true;
        }
        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.55f));
        g.setColour (theme::background);
        g.fillRect (panelArea());
        g.setColour (theme::textDim.withAlpha (0.4f));
        g.drawRect (panelArea());
    }

    void resized() override
    {
        auto r = panelArea().reduced (1);
        auto top = r.removeFromTop (40).reduced (12, 6);
        closeButton.setBounds (top.removeFromRight (90));
        title.setBounds (top);
        page.setBounds (r);
    }

    void mouseDown (const juce::MouseEvent& e) override   // outside the panel
    {
        if (! panelArea().contains (e.getPosition()))
            close();
    }

private:
    ImportPage page;
    juce::Label title;
    juce::TextButton closeButton { "Close" };

    juce::Rectangle<int> panelArea() const
    {
        return getLocalBounds().withSizeKeepingCentre (juce::jmin (1000, getWidth() - 24),
                                                       juce::jmin (640, getHeight() - 24));
    }
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
        hint.setText ("Click a marker: what it really is   Click: play from there   Double-click: one hit   Drag: region   Right-click an empty spot: add a missed hit   Wheel, Up / Down: zoom   Left / Right: hits   Space: play   W / E: start / end",
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
        hint.setText ("Top: click a name: what it really is    Bottom: drag the ends of a bar: IN / OUT    Ctrl + click / Shift + drag: select several, click one: change all    Double-click an empty spot: add a hit, on a marker: delete it    Drag a marker: move it"
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

// "Save the drums as MIDI" (from the MIDI Editor and the Export tab): asks
// where, next to the beat by default; `done` gets what to show after.
static void saveDrumsMidi (DrumRebuilderProcessor& processor, std::unique_ptr<juce::FileChooser>& chooser,
                           std::function<void (const juce::String&)> done)
{
    const auto* l = processor.getLoaded();
    if (l == nullptr)
        return;
    const auto suggested = l->file.getParentDirectory().getChildFile (l->file.getFileNameWithoutExtension() + " drums.mid");
    chooser = std::make_unique<juce::FileChooser> ("Save the drums as MIDI", suggested, "*.mid");
    juce::WeakReference<DrumRebuilderProcessor> safe (&processor);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, done] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == juce::File())
                                  return;
                              const auto file = fc.getResult().withFileExtension ("mid");
                              const auto& a = safe->getAnalysis();
                              const bool ok = drb::writeMidiFile (file, a.hits, a.grid);
                              done (ok ? "Saved " + file.getFileName() : "Could not save " + file.getFullPathName());
                          });
}

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

        for (auto* b : { &quantizeButton, &undoButton, &redoButton, &saveMidiButton, &zoomOut, &zoomIn, &fit })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        saveMidiButton.setTooltip ("Save the drums as a MIDI file (one track per instrument, as they are now)");
        saveMidiButton.onClick = [this]
        {
            juce::Component::SafePointer<MidiEditorPage> self (this);
            saveDrumsMidi (processor, chooser, [self] (const juce::String& text)
            {
                if (self != nullptr)
                    self->savedLabel.setText (text, juce::dontSendNotification);
            });
        };
        setupLabel (savedLabel, theme::accent, 13.0f);
        addAndMakeVisible (savedLabel);
        quantizeButton.setTooltip ("Move every hit towards the grid");
        quantizeButton.onClick = [this] { processor.quantizeHits (division(), strength.getValue() / 100.0); };
        undoButton.onClick = [this] { processor.undo(); };
        redoButton.onClick = [this] { processor.redo(); };
        zoomIn.onClick = [this] { view.zoomBy (2.0, view.zoomCentre()); };
        zoomOut.onClick = [this] { view.zoomBy (0.5, view.zoomCentre()); };
        fit.onClick = [this] { view.zoomToFit(); };

        setupLabel (hint, theme::textDim, 12.5f);
        hint.setText ("Solo: hear only that row (Ctrl: one more)    Click an empty spot: add a hit    Drag: move it (onto another row: change the instrument)"
                      "    Drag its flag up / down: louder / softer (it loops meanwhile)    Double-click or Delete: remove    Ctrl+Z: undo"
                      "    Several: Ctrl + click, Shift + click (the row up to here), Shift + drag a box - then drag one flag: all of them",
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
        saveMidiButton.setEnabled (processor.getLoaded() != nullptr && ! processor.getAnalysis().hits.empty());
        if (processor.getLoadedId() != savedFor)
        {
            savedFor = processor.getLoadedId();
            savedLabel.setText ({}, juce::dontSendNotification);
        }
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
        bar.removeFromLeft (16);
        saveMidiButton.setBounds (bar.removeFromLeft (100).reduced (0, 1));
        bar.removeFromLeft (8);

        fit.setBounds (bar.removeFromRight (52).reduced (0, 1));
        bar.removeFromRight (4);
        zoomIn.setBounds (bar.removeFromRight (34).reduced (0, 1));
        bar.removeFromRight (4);
        zoomOut.setBounds (bar.removeFromRight (34).reduced (0, 1));
        bar.removeFromRight (8);
        savedLabel.setBounds (bar);
    }

private:
    DrumRebuilderProcessor& processor;
    MidiEditorView view;
    juce::Label gridLabel, hint;
    juce::ComboBox gridBox;
    juce::ToggleButton snapButton { "Snap" };
    juce::Slider strength { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton quantizeButton { "Quantize" }, undoButton { "Undo" }, redoButton { "Redo" };
    juce::TextButton saveMidiButton { "Save MIDI..." };
    juce::Label savedLabel;
    int savedFor = -1;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TextButton zoomOut { "-" }, zoomIn { "+" }, fit { "Fit" };

    double division() const
    {
        const auto& divisions = drb::gridDivisions();
        return divisions[(size_t) juce::jlimit (0, (int) divisions.size() - 1, gridBox.getSelectedId() - 1)].beats;
    }
};

// --- Sample Browser ------------------------------------------------------

// The library: one folder per instrument. Save this beat's original hits
// into it, add your own sounds, listen to them, pick one for the rebuild.
class SampleBrowserPage : public juce::Component,
                          public juce::FileDragAndDropTarget,
                          public juce::DragAndDropContainer,
                          public juce::DragAndDropTarget,
                          private juce::ListBoxModel
{
public:
    explicit SampleBrowserPage (DrumRebuilderProcessor& p) : processor (p)
    {
        for (auto* b : { &saveButton, &addButton, &useButton, &trashButton, &openFolderButton, &changeFolderButton, &rescanButton })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        saveButton.setColour (juce::TextButton::buttonColourId, theme::accent.withAlpha (0.85f));
        saveButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        saveButton.onClick = [this]
        {
            const int n = processor.saveOriginals();
            result.setText (n > 0 ? juce::String (n) + " sounds saved from \"" + processor.getBeatName() + "\""
                                  : juce::String ("Nothing saved: open a beat first"),
                            juce::dontSendNotification);
        };
        addButton.onClick = [this] { addSounds(); };
        useButton.onClick = [this]
        {
            if (auto s = selectedSample())
                processor.setFixedSample (instrument(), s->file);
        };
        trashButton.onClick = [this]
        {
            if (auto s = selectedSample())
            {
                s->file.moveToTrash();
                processor.rescanLibrary();
            }
        };
        openFolderButton.onClick = [this]
        {
            const auto folder = processor.getLibrary().folderFor (instrument());
            folder.createDirectory();
            folder.startAsProcess();
        };
        changeFolderButton.onClick = [this] { changeFolder(); };
        rescanButton.onClick = [this] { processor.rescanLibrary(); };

        instruments.setModel (&instrumentModel);
        instrumentModel.owner = this;
        sounds.setModel (this);
        for (auto* l : { &instruments, &sounds })
        {
            l->setRowHeight (26);
            l->setColour (juce::ListBox::backgroundColourId, theme::panel);
            l->setWantsKeyboardFocus (false);   // the arrows / Space go to the page (they play)
            addAndMakeVisible (l);
        }
        instruments.selectRow (0);

        setupLabel (rootLabel, theme::textDim, 13.0f);
        setupLabel (help, theme::textDim, 13.0f);
        setupLabel (result, theme::accent, 14.0f);
        help.setJustificationType (juce::Justification::topLeft);
        help.setText ("The sounds in each instrument's folder. Click = listen (Space too). Up / Down: the next sound, it plays; "
                      "Left: to the folders (Up / Down between them), Right: into the folder. Drag sound files from your file manager onto an "
                      "instrument to add them; drag a sound up / down to put it in your order (right-click: put first). "
                      "The orange button saves every hit of this beat (cut between its IN and OUT) as \"<beat> (original) 01.wav\".",
                      juce::dontSendNotification);
        for (auto* l : { &rootLabel, &help, &result })
            addAndMakeVisible (l);
        update();
    }

    // The arrows, like a file manager: Up / Down go through the sounds of
    // the folder (each one plays) or, after Left, through the instrument
    // folders; Right goes into the folder (its first sound plays).
    bool arrowKey (const juce::KeyPress& key)
    {
        if (key == juce::KeyPress::leftKey)
        {
            if (! inFolders)
            {
                inFolders = true;
                processor.stopPreview();
                repaint();
            }
            return true;
        }
        if (key == juce::KeyPress::rightKey)
        {
            if (inFolders)
            {
                inFolders = false;
                repaint();
                if (sounds.getSelectedRow() < 0)
                    step (1);
                else if (auto s = selectedSample())
                    processor.previewSample (*s);
            }
            return true;
        }
        if (key != juce::KeyPress::upKey && key != juce::KeyPress::downKey)
            return false;
        const int delta = key == juce::KeyPress::upKey ? -1 : 1;
        if (inFolders)
        {
            const int row = juce::jlimit (0, drb::numInstruments - 1, instruments.getSelectedRow() + delta);
            instruments.selectRow (row);   // its sounds show on the right
        }
        else
            step (delta);
        return true;
    }

    // The sound above (-1) or below (+1) the selected one, played.
    void step (int delta)
    {
        const int n = getNumRows();
        if (n == 0)
            return;
        const int row = sounds.getSelectedRow() < 0 ? (delta > 0 ? 0 : n - 1)
                                                    : juce::jlimit (0, n - 1, sounds.getSelectedRow() + delta);
        sounds.selectRow (row);   // scrolls it into view
        processor.previewSample (*processor.getLibrary().samples (instrument())[(size_t) row]);
        update();
    }

    // Space on this page: the selected sound plays, again = stops (the beat
    // doesn't start here).
    void togglePreview()
    {
        auto& p = processor.getPreviewPlayer();
        if (p.isPlaying())
            processor.stopPreview();
        else if (auto s = selectedSample())
            processor.previewSample (*s);
    }

    void update()
    {
        rootLabel.setText ("Library: " + processor.getLibrary().getRoot().getFullPathName(), juce::dontSendNotification);
        saveButton.setEnabled (processor.getLoaded() != nullptr && ! processor.getAnalysis().hits.empty());
        instruments.updateContent();
        sounds.updateContent();
        instruments.repaint();
        sounds.repaint();
        const bool has = selectedSample() != nullptr;
        useButton.setEnabled (has);
        trashButton.setEnabled (has);
    }

    // Sound files dropped from outside: onto an instrument (or the list of
    // its sounds) = copied into its folder.
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (const auto& f : files)
            if (drb::isSupported (juce::File (f)))
                return true;
        return false;
    }
    void fileDragMove (const juce::StringArray&, int x, int y) override
    {
        if (const int t = dropInstrumentAt ({ x, y }); t != fileTarget)
        {
            fileTarget = t;
            repaint();
        }
    }
    void fileDragExit (const juce::StringArray&) override
    {
        fileTarget = -1;
        repaint();
    }
    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        const int row = dropInstrumentAt ({ x, y });
        fileTarget = -1;
        const auto inst = drb::allInstruments[(size_t) row];
        juce::Array<juce::File> list;
        for (const auto& f : files)
            list.add (juce::File (f));
        const int n = processor.getLibrary().addFiles (inst, list);
        processor.rescanLibrary();
        instruments.selectRow (row);
        result.setText (juce::String (n) + (n == 1 ? " sound added to " : " sounds added to ") + drb::instrumentName (inst),
                        juce::dontSendNotification);
        update();
        repaint();
    }

    // A sound dragged up / down in the list: the new order.
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.toString().startsWith ("sample:"); }
    void itemDragMove (const SourceDetails& d) override
    {
        if (const int i = insertionAt (d.localPosition); i != dropIndex)
        {
            dropIndex = i;
            repaint();
        }
    }
    void itemDragExit (const SourceDetails&) override
    {
        dropIndex = -1;
        repaint();
    }
    void itemDropped (const SourceDetails& d) override
    {
        const int from = d.description.toString().fromFirstOccurrenceOf (":", false, false).getIntValue();
        int to = insertionAt (d.localPosition);
        dropIndex = -1;
        if (to >= 0)
        {
            if (to > from)
                --to;
            moveSound (from, to);
        }
        repaint();
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // Which list the arrows move in.
        g.setColour (theme::accent.withAlpha (0.8f));
        g.drawRect ((inFolders ? instruments : sounds).getBounds().expanded (2), 2);

        // Where a dragged sound goes: a line between the rows.
        if (dropIndex >= 0)
        {
            const int n = sounds.getListBoxModel() != nullptr ? getNumRows() : 0;
            const auto row = sounds.getRowPosition (juce::jmin (dropIndex, juce::jmax (0, n - 1)), true);
            const int y = sounds.getY() + (dropIndex >= n && n > 0 ? row.getBottom() : row.getY());
            g.setColour (theme::accent);
            g.fillRect (sounds.getX(), y - 1, sounds.getWidth(), 3);
        }
        // Where dropped files go: that instrument, outlined.
        if (fileTarget >= 0)
        {
            const auto row = instruments.getRowPosition (fileTarget, true).translated (instruments.getX(), instruments.getY());
            g.setColour (theme::accent);
            g.drawRect (row, 2);
            g.drawRect (sounds.getBounds(), 2);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText (juce::String ("Drop: add to ") + drb::instrumentName (drb::allInstruments[(size_t) fileTarget]),
                        sounds.getBounds().reduced (10), juce::Justification::centredBottom);
        }
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto top = r.removeFromTop (30);
        saveButton.setBounds (top.removeFromLeft (250));
        top.removeFromLeft (12);
        result.setBounds (top);
        r.removeFromTop (4);
        help.setBounds (r.removeFromTop (36));
        r.removeFromTop (6);

        auto bottom = r.removeFromBottom (30);
        rootLabel.setBounds (bottom.removeFromLeft (bottom.getWidth() / 2));
        rescanButton.setBounds (bottom.removeFromRight (80).reduced (0, 2));
        bottom.removeFromRight (6);
        changeFolderButton.setBounds (bottom.removeFromRight (140).reduced (0, 2));
        r.removeFromBottom (6);

        instruments.setBounds (r.removeFromLeft (220));
        r.removeFromLeft (10);
        auto buttons = r.removeFromTop (30);
        for (auto* b : { &addButton, &useButton, &trashButton, &openFolderButton })
        {
            b->setBounds (buttons.removeFromLeft (b == &useButton ? 160 : 130).reduced (0, 1));
            buttons.removeFromLeft (6);
        }
        r.removeFromTop (6);
        sounds.setBounds (r);
    }

private:
    DrumRebuilderProcessor& processor;
    juce::ListBox instruments, sounds;
    juce::TextButton saveButton { "Save this beat's original sounds" }, addButton { "Add sounds..." },
                     useButton { "Use for the rebuild" }, trashButton { "Move to trash" }, openFolderButton { "Open folder" },
                     changeFolderButton { "Change library..." }, rescanButton { "Rescan" };
    juce::Label rootLabel, help, result;
    std::unique_ptr<juce::FileChooser> chooser;

    drb::Instrument instrument() const
    {
        const int row = juce::jlimit (0, drb::numInstruments - 1, instruments.getSelectedRow());
        return drb::allInstruments[(size_t) row];
    }

    drb::SamplePtr selectedSample() const
    {
        const auto& list = processor.getLibrary().samples (instrument());
        const int row = sounds.getSelectedRow();
        return juce::isPositiveAndBelow (row, (int) list.size()) ? list[(size_t) row] : nullptr;
    }

    bool inFolders = false;   // the arrows move in the instruments (left) list
    int dropIndex = -1;    // where a dragged sound would go
    int fileTarget = -1;   // the instrument dropped files would go to

    // The instrument row under a point (on the sounds: the shown one).
    int dropInstrumentAt (juce::Point<int> p) const
    {
        const auto q = instruments.getLocalPoint (this, p);
        if (instruments.getLocalBounds().contains (q))
            if (const int row = instruments.getRowContainingPosition (q.x, q.y); juce::isPositiveAndBelow (row, drb::numInstruments))
                return row;
        return juce::jlimit (0, drb::numInstruments - 1, instruments.getSelectedRow());
    }

    int insertionAt (juce::Point<int> p) const
    {
        const auto q = sounds.getLocalPoint (this, p);
        if (! sounds.getLocalBounds().contains (q))
            return -1;
        const int i = sounds.getInsertionIndexForPosition (q.x, q.y);
        return i < 0 ? (int) processor.getLibrary().samples (instrument()).size() : i;
    }

    void moveSound (int from, int to)
    {
        processor.moveSample (instrument(), from, to);
        sounds.selectRow (juce::jlimit (0, getNumRows() - 1, to));
        update();
    }

    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override
    {
        return rows.isEmpty() ? juce::var() : juce::var ("sample:" + juce::String (rows[0]));
    }

    // Left: the instruments, with how many sounds each folder has.
    struct InstrumentModel : public juce::ListBoxModel
    {
        SampleBrowserPage* owner = nullptr;
        int getNumRows() override { return drb::numInstruments; }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            const auto inst = drb::allInstruments[(size_t) row];
            if (selected)
                g.fillAll (theme::panelLight);
            g.setColour (theme::instrument (inst));
            g.fillRect (6, 6, 4, h - 12);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawText (drb::instrumentName (inst), 18, 0, w - 70, h, juce::Justification::centredLeft);
            g.setColour (theme::textDim);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (juce::String ((int) owner->processor.getLibrary().samples (inst).size()), w - 50, 0, 40, h,
                        juce::Justification::centredRight);
        }
        void selectedRowsChanged (int) override
        {
            owner->sounds.deselectAllRows();
            owner->update();
        }
        void listBoxItemClicked (int, const juce::MouseEvent&) override
        {
            owner->inFolders = true;
            owner->repaint();
        }
    } instrumentModel;

    // Right: the selected instrument's sounds. Click = listen.
    int getNumRows() override { return (int) processor.getLibrary().samples (instrument()).size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        const auto& list = processor.getLibrary().samples (instrument());
        if (! juce::isPositiveAndBelow (row, (int) list.size()))
            return;
        const auto& s = *list[(size_t) row];
        if (selected)
            g.fillAll (theme::panelLight);

        const bool inUse = processor.getFixedSample (instrument()) != nullptr && processor.getFixedSample (instrument())->file == s.file;
        auto r = juce::Rectangle<int> (0, 0, w, h).reduced (10, 0);
        if (s.original)
        {
            const auto tag = r.removeFromLeft (70).reduced (0, 5);
            g.setColour (theme::startFlag);
            g.fillRoundedRectangle (tag.toFloat(), 4.0f);
            g.setColour (juce::Colours::black);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("ORIGINAL", tag, juce::Justification::centred);
            r.removeFromLeft (8);
        }
        g.setColour (inUse ? theme::accent : theme::text);
        g.setFont (juce::FontOptions (13.5f, inUse ? juce::Font::bold : juce::Font::plain));
        const auto info = juce::String (s.audio.getNumSamples() / s.sampleRate, 2) + " s" + (inUse ? "   in use" : "");
        g.drawText (info, r.removeFromRight (120), juce::Justification::centredRight);
        g.drawText (s.file.getFileNameWithoutExtension(), r, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        const auto& list = processor.getLibrary().samples (instrument());
        if (! juce::isPositiveAndBelow (row, (int) list.size()))
            return;
        inFolders = false;
        repaint();
        if (e.mods.isPopupMenu())
        {
            showSoundMenu (row);
            return;
        }
        processor.previewSample (*list[(size_t) row]);
        update();
    }

    void showSoundMenu (int row)
    {
        const int n = getNumRows();
        juce::PopupMenu menu;
        juce::Component::SafePointer<SampleBrowserPage> self (this);
        menu.addItem ("Put first", row > 0, false, [self, row] { if (self != nullptr) self->moveSound (row, 0); });
        menu.addItem ("Move up", row > 0, false, [self, row] { if (self != nullptr) self->moveSound (row, row - 1); });
        menu.addItem ("Move down", row < n - 1, false, [self, row] { if (self != nullptr) self->moveSound (row, row + 1); });
        menu.addItem ("Put last", row < n - 1, false, [self, row, n] { if (self != nullptr) self->moveSound (row, n - 1); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sounds).withMousePosition(), focus::afterMenu (this));
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        const auto& list = processor.getLibrary().samples (instrument());
        if (juce::isPositiveAndBelow (row, (int) list.size()))
            processor.setFixedSample (instrument(), list[(size_t) row]->file);
    }

    void addSounds()
    {
        const auto inst = instrument();
        chooser = std::make_unique<juce::FileChooser> ("Add sounds to " + juce::String (drb::instrumentName (inst)),
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       drb::supportedWildcard());
        juce::Component::SafePointer<SampleBrowserPage> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [self, inst] (const juce::FileChooser& fc)
                              {
                                  if (self == nullptr || fc.getResults().isEmpty())
                                      return;
                                  const int n = self->processor.getLibrary().addFiles (inst, fc.getResults());
                                  self->processor.rescanLibrary();
                                  self->result.setText (juce::String (n) + " sounds added to " + drb::instrumentName (inst),
                                                        juce::dontSendNotification);
                              });
    }

    void changeFolder()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose the library folder", processor.getLibrary().getRoot());
        juce::Component::SafePointer<SampleBrowserPage> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [self] (const juce::FileChooser& fc)
                              {
                                  if (self == nullptr || ! fc.getResult().isDirectory())
                                      return;
                                  self->processor.getLibrary().setRoot (fc.getResult());
                                  self->processor.rescanLibrary();
                              });
    }
};

// --- Drum Rack (rebuild) ---------------------------------------------------

// One row per instrument: how its hits pick their sound, which sound.
// A Drum Rack row's sound: its waveform and how long it plays (the orange
// handle; what is after it is cut, with a short fade). Drag the handle (or
// anywhere on the waveform); double-click = all of it again; click = hear it
// as it plays now.
class SoundLengthView : public juce::Component,
                        public juce::SettableTooltipClient
{
public:
    SoundLengthView (DrumRebuilderProcessor& p, drb::Instrument i) : processor (p), instrument (i)
    {
        setTooltip ("How long this sound plays at every hit: drag the orange line. Double-click: all of it");
    }

    void update()
    {
        auto s = processor.getRowSound (instrument);
        const bool changed = (s == nullptr) != (sound == nullptr)
                             || (s != nullptr && (s->file != sound->file || s->audio.getNumSamples() != sound->audio.getNumSamples()));
        sound = std::move (s);
        if (changed)
            buildPeaks();
        if (! dragging)
            length = processor.getSoundLength (instrument);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (theme::background);
        g.fillRoundedRectangle (r, 4.0f);
        if (sound == nullptr || peaks.empty())
        {
            g.setColour (theme::textDim);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText ("no sound", r, juce::Justification::centred);
            return;
        }
        const float x0 = r.getX() + 2.0f, w = r.getWidth() - 4.0f, mid = r.getCentreY(), half = r.getHeight() * 0.45f;
        const float cutX = lengthX();
        const auto colour = theme::instrument (instrument);
        for (size_t k = 0; k < peaks.size(); ++k)
        {
            const float x = x0 + w * (float) k / (float) peaks.size();
            g.setColour (x > cutX ? colour.withAlpha (0.18f) : colour.withAlpha (0.9f));
            g.drawVerticalLine ((int) x, mid - peaks[k] * half, mid + peaks[k] * half + 1.0f);
        }
        if (length > 0.0)
        {
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (cutX, r.getY(), r.getRight() - cutX, r.getHeight()));
        }
        // The handle.
        g.setColour (theme::accent);
        g.fillRect (juce::Rectangle<float> (cutX - 1.5f, r.getY(), 3.0f, r.getHeight()));
        g.fillRoundedRectangle (juce::Rectangle<float> (cutX - 5.0f, r.getY(), 10.0f, 8.0f), 2.0f);

        const double full = soundSeconds();
        const auto text = length > 0.0 && length < full ? juce::String (length, 2) + " s  of  " + juce::String (full, 2) + " s"
                                                        : juce::String (full, 2) + " s  (all)";
        g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        const float tw = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 10.0f;
        const auto label = juce::Rectangle<float> (r.getRight() - tw - 3.0f, r.getY() + 2.0f, tw, 15.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRoundedRectangle (label, 3.0f);
        g.setColour (juce::Colours::white);
        g.drawText (text, label, juce::Justification::centred);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (std::abs ((float) e.x - lengthX()) < 8.0f ? juce::MouseCursor::LeftRightResizeCursor
                                                                  : juce::MouseCursor::PointingHandCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = sound != nullptr && std::abs ((float) e.x - lengthX()) < 8.0f;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (sound == nullptr || (! dragging && e.getDistanceFromDragStart() < 3))
            return;
        dragging = true;
        const double full = soundSeconds();
        const double t = juce::jlimit (0.01, full, full * (e.x - 2.0) / std::max (1.0, getWidth() - 4.0));
        length = t >= full * 0.995 ? 0.0 : t;
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging)
        {
            dragging = false;
            processor.setSoundLength (instrument, length);
        }
        else
            listen();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        length = 0.0;
        processor.setSoundLength (instrument, 0.0);
    }

    // Plays the sound as long as it is set to.
    void listen()
    {
        if (sound == nullptr)
            return;
        if (length <= 0.0 || length >= soundSeconds())
        {
            processor.previewSample (*sound);
            return;
        }
        drb::Sample cut;
        cut.sampleRate = sound->sampleRate;
        const int end = std::max (1, (int) std::ceil (length * sound->sampleRate));
        cut.audio.makeCopyOf (sound->audio);
        cut.audio.setSize (sound->audio.getNumChannels(), end, true);
        const int n = std::min (end, (int) (0.015 * sound->sampleRate));
        cut.audio.applyGainRamp (end - n, n, 1.0f, 0.0f);
        processor.previewSample (cut);
    }

private:
    DrumRebuilderProcessor& processor;
    drb::Instrument instrument;
    drb::SamplePtr sound;
    std::vector<float> peaks;
    double length = 0.0;
    bool dragging = false;

    double soundSeconds() const { return sound != nullptr ? sound->audio.getNumSamples() / sound->sampleRate : 0.0; }

    float lengthX() const
    {
        const double full = soundSeconds();
        const double l = length > 0.0 && full > 0.0 ? std::min (length, full) : full;
        return 2.0f + (float) (getWidth() - 4) * (float) (full > 0.0 ? l / full : 1.0);
    }

    void buildPeaks()
    {
        peaks.clear();
        if (sound == nullptr)
            return;
        const int n = 300, total = sound->audio.getNumSamples();
        float top = 0.0f;
        for (int k = 0; k < n; ++k)
        {
            const int a = (int) ((juce::int64) total * k / n), b = std::max (a + 1, (int) ((juce::int64) total * (k + 1) / n));
            float m = 0.0f;
            for (int ch = 0; ch < sound->audio.getNumChannels(); ++ch)
                m = std::max (m, sound->audio.getMagnitude (ch, a, std::min (b, total) - a));
            peaks.push_back (m);
            top = std::max (top, m);
        }
        if (top > 0.0f)
            for (auto& p : peaks)
                p /= top;
    }
};

class RackRow : public juce::Component
{
public:
    RackRow (DrumRebuilderProcessor& p, drb::Instrument i) : processor (p), instrument (i), length (p, i)
    {
        addAndMakeVisible (length);
        for (auto m : drb::allPickModes)
            mode.addItem (drb::pickModeName (m), (int) m + 1);
        mode.onChange = [this]
        {
            processor.setPickMode (instrument, (drb::PickMode) (mode.getSelectedId() - 1));
            keyboardBack();
        };
        sound.onChange = [this]
        {
            const auto pool = processor.getPool (instrument);
            const int k = sound.getSelectedId() - 1;
            if (juce::isPositiveAndBelow (k, (int) pool.size()))
                processor.setFixedSample (instrument, pool[(size_t) k]->file);
            keyboardBack();
        };
        listen.onClick = [this] { length.listen(); };
        change.onClick = [this] { processor.setChanging (instrument, change.getToggleState()); };
        change.setTooltip ("Ticked: Next sounds / Right arrow gives this instrument another sound from its folder");
        for (auto* c : std::initializer_list<juce::Component*> { &change, &mode, &sound, &listen })
        {
            c->setWantsKeyboardFocus (false);
            addAndMakeVisible (c);
        }
        setupLabel (info, theme::textDim, 12.5f);
        addAndMakeVisible (info);
        update();
    }

    drb::Instrument getInstrument() const { return instrument; }

    void update()
    {
        const auto pool = processor.getPool (instrument);
        const auto m = processor.getPickMode (instrument);
        mode.setSelectedId ((int) m + 1, juce::dontSendNotification);
        mode.setEnabled (! pool.empty());
        change.setToggleState (processor.getChanging (instrument), juce::dontSendNotification);
        change.setEnabled (! pool.empty());

        sound.clear (juce::dontSendNotification);
        const auto fixed = processor.getFixedSample (instrument);
        for (size_t k = 0; k < pool.size(); ++k)
        {
            sound.addItem (pool[k]->file.getFileNameWithoutExtension(), (int) k + 1);
            if (fixed != nullptr && pool[k]->file == fixed->file)
                sound.setSelectedId ((int) k + 1, juce::dontSendNotification);
        }
        sound.setEnabled (m == drb::PickMode::Fixed && ! pool.empty());
        length.update();
        listen.setEnabled (processor.getRowSound (instrument) != nullptr);

        int hits = 0;
        for (const auto& h : processor.getAnalysis().hits)
            hits += h.instrument == instrument ? 1 : 0;
        juce::String text;
        text << hits << (hits == 1 ? " hit    " : " hits    ");
        if (pool.empty())
            text << "folder empty: plays its original sounds";
        else if (m == drb::PickMode::Original)
            text << "its original sounds (" << (int) pool.size() << " others in the folder)";
        else
            text << (int) pool.size() << " sounds in the folder";
        info.setText (text, juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        g.setColour (theme::instrument (instrument));
        g.fillRect (8, 8, 5, getHeight() - 16);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (drb::instrumentName (instrument), 46, 8, 120, 30, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 7);
        // Right: the sound and its length; left: two lines of controls.
        length.setBounds (r.removeFromRight (juce::jlimit (160, 420, r.getWidth() * 2 / 5)));
        r.removeFromRight (10);
        auto top = r.removeFromTop (30);
        change.setBounds (top.getX() + 12, top.getY(), 26, top.getHeight());
        top.removeFromLeft (170);
        mode.setBounds (top.removeFromLeft (130));
        top.removeFromLeft (8);
        listen.setBounds (top.removeFromRight (70));
        top.removeFromRight (6);
        sound.setBounds (top);
        r.removeFromTop (4);
        r.removeFromLeft (170);
        info.setBounds (r);
    }

private:
    // After a list closes the keyboard can be left nowhere: the arrows
    // must still reach the page.
    void keyboardBack()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
                                         { focus::takeBack (safe.getComponent()); });
    }

    DrumRebuilderProcessor& processor;
    drb::Instrument instrument;
    juce::ToggleButton change;
    juce::ComboBox mode, sound;
    juce::TextButton listen { "Listen" };
    juce::Label info;
    SoundLengthView length;
};

class DrumRackPage : public juce::Component
{
public:
    explicit DrumRackPage (DrumRebuilderProcessor& p) : processor (p)
    {
        addMouseListener (this, true);   // a click anywhere here: the arrows work again
        for (auto* b : { &aButton, &bButton })
        {
            b->setClickingTogglesState (false);
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        aButton.onClick = [this] { processor.setListeningRebuilt (false); };
        bButton.onClick = [this] { processor.setListeningRebuilt (true); };
        aButton.setTooltip ("Hear the recording as it is");
        bButton.setTooltip ("Hear the beat rebuilt with the sounds below");

        nextButton.setWantsKeyboardFocus (false);
        nextButton.onClick = [this] { processor.nextSounds(); };
        nextButton.setTooltip ("Right arrow: other sounds from the folders, at random, for the ticked instruments");
        addAndMakeVisible (nextButton);
        prevButton.setWantsKeyboardFocus (false);
        prevButton.onClick = [this] { processor.previousSounds(); };
        prevButton.setTooltip ("Left arrow: back to the sounds before");
        addAndMakeVisible (prevButton);

        keepPitch.setWantsKeyboardFocus (false);
        keepPitch.onClick = [this] { processor.setKeepPitch (keepPitch.getToggleState()); };
        keepPitch.setTooltip ("Tune each new sound to the original instrument (kick, toms: their note; hi-hats: their brightness)");
        addAndMakeVisible (keepPitch);

        setupLabel (tempoLabel, theme::textDim);
        tempoLabel.setText ("Tempo", juce::dontSendNotification);
        addAndMakeVisible (tempoLabel);
        tempo.setTextValueSuffix (" BPM");
        tempo.setNumDecimalPlacesToDisplay (1);
        tempo.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
        tempo.setWantsKeyboardFocus (false);
        tempo.setColour (juce::Slider::backgroundColourId, theme::panelLight);
        tempo.setColour (juce::Slider::trackColourId, theme::accent.withAlpha (0.7f));
        tempo.onValueChange = [this] { processor.setRebuildBpm (tempo.getValue()); };
        tempo.onDragEnd = [this] { processor.setRebuildBpm (tempo.getValue()); };
        addAndMakeVisible (tempo);
        resetTempo.setWantsKeyboardFocus (false);
        resetTempo.onClick = [this] { processor.setRebuildBpm (processor.getAnalysis().grid.bpm); };
        addAndMakeVisible (resetTempo);

        setupLabel (status, theme::textDim, 13.0f);
        setupLabel (hint, theme::textDim, 12.5f);
        hint.setText ("Space / Play plays A or B in a loop over its bars. Left / Right: the sounds before / next sounds for the ticked instruments. "
                      "Drag the orange line on a sound: how long it plays (double-click: all of it). "
                      "Save the originals and add your own sounds on the Sample Browser tab.",
                      juce::dontSendNotification);
        addAndMakeVisible (status);
        addAndMakeVisible (hint);
        update();
    }

    void update()
    {
        // A row for every instrument the beat has.
        std::vector<drb::Instrument> wanted;
        for (auto inst : drb::allInstruments)
            for (const auto& h : processor.getAnalysis().hits)
                if (h.instrument == inst)
                {
                    wanted.push_back (inst);
                    break;
                }
        bool same = wanted.size() == rows.size();
        for (size_t k = 0; same && k < rows.size(); ++k)
            same = rows[k]->getInstrument() == wanted[k];
        if (! same)
        {
            rows.clear();
            for (auto inst : wanted)
                addAndMakeVisible (*rows.emplace_back (std::make_unique<RackRow> (processor, inst)));
            resized();
        }
        for (auto& r : rows)
            r->update();

        const bool b = processor.isListeningRebuilt();
        aButton.setToggleState (! b, juce::dontSendNotification);
        bButton.setToggleState (b, juce::dontSendNotification);
        keepPitch.setToggleState (processor.getKeepPitch(), juce::dontSendNotification);
        prevButton.setEnabled (processor.canGoBack());

        const double bpm = processor.getAnalysis().grid.bpm;
        const bool hasTempo = bpm > 0.0 && processor.getLoaded() != nullptr;
        tempo.setEnabled (hasTempo);
        resetTempo.setEnabled (hasTempo);
        if (hasTempo)
        {
            tempo.setRange (bpm * 0.5, bpm * 2.0, 0.1);
            tempo.setValue (processor.getRebuildBpm(), juce::dontSendNotification);
        }
        status.setText (processor.getLoaded() == nullptr ? juce::String ("Open a beat first")
                        : processor.isRendering()        ? juce::String ("Rebuilding...")
                        : b                              ? juce::String ("Listening: B, the rebuilt beat")
                                                         : juce::String ("Listening: A, the recording"),
                        juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::background); }

    void mouseUp (const juce::MouseEvent&) override { keyboardBack(); }
    void visibilityChanged() override
    {
        if (isShowing())
            keyboardBack();
    }

    void keyboardBack()
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)]
                                         { focus::takeBack (safe.getComponent()); });
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        auto bar = r.removeFromTop (34);
        aButton.setBounds (bar.removeFromLeft (110));
        bar.removeFromLeft (4);
        bButton.setBounds (bar.removeFromLeft (110));
        bar.removeFromLeft (16);
        prevButton.setBounds (bar.removeFromLeft (90));
        bar.removeFromLeft (4);
        nextButton.setBounds (bar.removeFromLeft (120));
        bar.removeFromLeft (16);
        keepPitch.setBounds (bar.removeFromLeft (170));
        bar.removeFromLeft (10);
        tempoLabel.setBounds (bar.removeFromLeft (48));
        tempo.setBounds (bar.removeFromLeft (260));
        bar.removeFromLeft (6);
        resetTempo.setBounds (bar.removeFromLeft (80).reduced (0, 3));
        bar.removeFromLeft (12);
        status.setBounds (bar);

        hint.setBounds (r.removeFromBottom (22));
        r.removeFromTop (10);
        for (auto& row : rows)
        {
            row->setBounds (r.removeFromTop (70));
            r.removeFromTop (6);
        }
    }

private:
    DrumRebuilderProcessor& processor;
    std::vector<std::unique_ptr<RackRow>> rows;
    juce::TextButton aButton { "A  Original" }, bButton { "B  Rebuilt" }, nextButton { "Next sounds" }, prevButton { "Previous" }, resetTempo { "Original" };
    juce::ToggleButton keepPitch { "Keep original pitch" };
    juce::Label tempoLabel, status, hint;
    juce::Slider tempo { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
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
        wavButton.setWantsKeyboardFocus (false);
        wavButton.onClick = [this] { exportWav(); };
        addAndMakeVisible (wavButton);
        setupLabel (wavTitle, theme::text, 18.0f);
        wavTitle.setText ("Audio", juce::dontSendNotification);
        setupLabel (wavInfo, theme::text, 14.0f);
        wavInfo.setJustificationType (juce::Justification::topLeft);
        setupLabel (wavResult, theme::accent, 14.0f);
        for (auto* l : { &wavTitle, &wavInfo, &wavResult })
            addAndMakeVisible (l);

        setupLabel (title, theme::text, 18.0f);
        title.setText ("MIDI", juce::dontSendNotification);
        setupLabel (info, theme::text, 14.0f);
        info.setJustificationType (juce::Justification::topLeft);
        setupLabel (result, theme::accent, 14.0f);
        setupLabel (later, theme::textDim, 14.0f);
        later.setJustificationType (juce::Justification::topLeft);
        later.setText ("Stems, one WAV per instrument with the Reverb and Delay tails - stage 5",
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
        wavButton.setEnabled (processor.getLoaded() != nullptr && ! a.hits.empty());
        wavInfo.setText (processor.getLoaded() == nullptr ? juce::String()
                         : "The rebuilt beat (the Drum Rack's sounds" + juce::String (processor.getKeepPitch() ? ", tuned" : "")
                               + ", at " + juce::String (processor.getRebuildBpm(), 1) + " BPM), stereo 24-bit WAV at the "
                               "recording's sample rate, with the last sounds ringing out.",
                         juce::dontSendNotification);
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
        r.removeFromTop (30);
        wavTitle.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        wavInfo.setBounds (r.removeFromTop (40));
        auto wavRow = r.removeFromTop (32);
        wavButton.setBounds (wavRow.removeFromLeft (150));
        wavRow.removeFromLeft (12);
        wavResult.setBounds (wavRow);
        r.removeFromTop (30);
        later.setBounds (r.removeFromTop (60));
    }

private:
    DrumRebuilderProcessor& processor;
    juce::TextButton midiButton { "Export MIDI..." }, wavButton { "Export WAV..." };
    juce::Label title, info, result, later, wavTitle, wavInfo, wavResult;

    void exportWav()
    {
        const auto* l = processor.getLoaded();
        if (l == nullptr)
            return;
        const auto suggested = l->file.getParentDirectory().getChildFile (l->file.getFileNameWithoutExtension() + " rebuilt.wav");
        chooser = std::make_unique<juce::FileChooser> ("Save the rebuilt beat as WAV", suggested, "*.wav");
        juce::Component::SafePointer<ExportPage> self (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [self] (const juce::FileChooser& fc)
                              {
                                  if (self == nullptr || fc.getResult() == juce::File())
                                      return;
                                  const auto file = fc.getResult().withFileExtension ("wav");
                                  self->wavResult.setText ("Saving...", juce::dontSendNotification);
                                  self->processor.exportRebuiltWav (file, [self, file] (bool ok)
                                  {
                                      if (self != nullptr)
                                          self->wavResult.setText (ok ? "Saved " + file.getFileName() : "Could not save " + file.getFullPathName(),
                                                                   juce::dontSendNotification);
                                  });
                              });
    }
    std::unique_ptr<juce::FileChooser> chooser;

    void exportMidi()
    {
        juce::Component::SafePointer<ExportPage> self (this);
        saveDrumsMidi (processor, chooser, [self] (const juce::String& text)
        {
            if (self != nullptr)
                self->result.setText (text, juce::dontSendNotification);
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
        processor.setPlayheadSeconds (processor.setFirstDownbeat (processor.getPlayheadSeconds()));
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
    samplePage = new SampleBrowserPage (processor);
    tabs.addTab ("Sample Browser", theme::panel, samplePage, true);
    rackPage = new DrumRackPage (processor);
    tabs.addTab ("Drum Rack", theme::panel, rackPage, true);
    tabs.addTab ("Master", theme::panel, new Placeholder ("Master", "Master Reverb and Delay - stage 5"), true);
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
    openPanel.reset();
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
    if (openPanel != nullptr)
        openPanel->setBounds (getLocalBounds());

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
    if (openPanel != nullptr)   // the Open panel's keys stay there
        return false;

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

    // Escape: the selected labels on the Instruments tab are let go.
    if (key == juce::KeyPress::escapeKey && tabs.getCurrentContentComponent() == instrumentsPage)
        return instrumentsPage->getWaveform().clearSelection();

    if (key == juce::KeyPress::spaceKey)
    {
        if (tabs.getCurrentContentComponent() == samplePage)
            samplePage->togglePreview();
        else if (! (onImport && importPage->togglePreview()))
            togglePlay();
        return true;
    }

    // W / E: to the start / end of the beat (playing goes on from there).
    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
    if ((c == 'w' || c == 'e') && processor.getLoaded() != nullptr)
    {
        const auto* l = processor.getLoaded();
        const double t = c == 'w' ? 0.0 : std::max (0.0, l->audio.getNumSamples() / l->sampleRate - 0.001);
        processor.setPlayheadSeconds (t);
        waveform.scrollToShow (t);
        return true;
    }

    // Sample Browser: the arrows go through the folders and their sounds.
    if (tabs.getCurrentContentComponent() == samplePage && samplePage->arrowKey (key))
        return true;

    // Drum Rack: left / right = the sounds before / next sounds.
    if (tabs.getCurrentContentComponent() == rackPage
        && (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey))
    {
        if (key == juce::KeyPress::leftKey)
            processor.previousSounds();
        else
            processor.nextSounds();
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

// Our own Open panel instead of the system dialog, which can't play a file
// before opening it.
void DrumRebuilderEditor::openFileChooser()
{
    if (openPanel == nullptr)
    {
        openPanel = std::make_unique<OpenPanel> (processor);
        openPanel->onClose = [this]
        {
            openPanel.reset();   // stops the preview
            focus::takeBack (this);
        };
        addAndMakeVisible (*openPanel);
        openPanel->setBounds (getLocalBounds());
    }
    openPanel->grabKeyboardFocus();
}

void DrumRebuilderEditor::togglePlay()
{
    // On the Instruments tab with instruments on solo, only they play.
    auto& player = processor.getPlayer();
    if (processor.isSoloPlaying())
        processor.pauseSolo();
    else if (player.isPlaying())
        player.pause();
    else if ((tabs.getCurrentContentComponent() == instrumentsPage || tabs.getCurrentContentComponent() == midiPage)
             && processor.anySolo())
        processor.playSolo();
    else
    {
        processor.stopPreview();
        processor.setBarLoop (tabs.getCurrentContentComponent() == rackPage);
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
    samplePage->update();
    rackPage->update();
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

    // The MIDI editor plays the hits as changed there, the other tabs the
    // recording as it was. Opening the MIDI editor: everything plays, no
    // solo left over.
    if (auto* page = tabs.getCurrentContentComponent(); page != lastPage)
    {
        lastPage = page;
        processor.setEditsAudible (page == midiPage);
        if (page == midiPage)
            for (auto inst : processor.getSolo())
                processor.setSolo (inst, false);
    }

    const auto& player = processor.getPlayer();
    const double position = processor.isSoloPlaying() ? processor.getAuditionPlayer().getPositionSeconds()
                                                      : processor.getPlayheadSeconds();
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
