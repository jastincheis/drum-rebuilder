# Drum Rebuilder

*Working title: Drum Analyzer & Beat Rebuilder.*

A free, open-source audio plugin (VST3, AU, standalone) that listens to a
**drums-only** recording, finds the tempo, the bars and every hit (kick, snare,
hi-hats, clap, rimshot, toms, crash, ride) with velocity, swing and
microtiming, turns them into editable MIDI, and rebuilds the beat with your own
samples while keeping the groove.

**Status: preview (0.3.0), stage 3 done, stage 5 started.** It works and is
used, but things still change between versions.
[Download the latest release](https://github.com/jastincheis/drum-rebuilder/releases).
It is also built into [Adormis Audio](https://github.com/jastincheis/adormis-audio)
(Tools > Drum Rebuilder).

## What it does, tab by tab

- **Import**: open a drums-only recording. A file browser inside the window
  plays each file when you click it, with its waveform. Inside Adormis Audio
  the list also shows every file open there (stem tabs too).
- **Analysis**: tempo, bar 1 and the bar / beat grid, found by itself and
  fixable by hand.
- **Instruments**: the waveform with every hit named. Kick, snare and hi-hat
  (closed / open) are found by themselves; clap, rimshot, toms, crash and ride
  you name by hand. Click a name to change it, double-click to add or delete a
  marker, drag each hit's IN / OUT. Solo one or more instruments to hear only
  them; while they are on solo only their markers can be changed.
- **MIDI Editor**: every hit as a bar with its real level in dB. Change
  levels (up to +12 dB), quantize, select many at once, undo, and save MIDI.
- **Sample Browser**: your sound library, one folder per instrument, plus
  **Unsorted** and **Loops**. **Import a folder...** sorts a whole folder of
  sounds by name, or by how they sound when the name says nothing, and
  **links** them: your files stay where they are. Drag a sound onto another
  folder to move it, **Remove** to take it out. The beat's own hits are saved
  here too.
- **Drum Rack**: the beat rebuilt with your sounds, per instrument Original /
  Fixed / Random / Round robin / By velocity, Next sounds, keep the original
  pitch, another tempo, IN / OUT of each sound, and A/B against the original.
- **Master**: volume, Reverb and Delay (in time with the beat) for the
  rebuilt beat.
- **Export**: WAV of the rebuilt beat and MIDI of the hits (inside Adormis
  Audio, the rebuilt beat can go straight back into it).

Each beat remembers its fixes.

## Keys

Space play / pause, W / E start / end, Left / Right previous / next hit,
Up / Down zoom, Ctrl+Z / Ctrl+Shift+Z undo / redo, Delete removes the selected
hit in the MIDI Editor.

## Plan

| Stage | What |
|---|---|
| 0 | Skeleton, CI on macOS / Linux / Windows, synthetic test beats with known hits, scoring |
| 1 | Import, waveform, transport (play, stop, pause, loop, region, zoom), tempo and onset detection |
| 2 | Kick, snare, hi-hat (closed + open), MIDI editor, quantize, MIDI export |
| 3 | Sample folders (fixed / random / round robin / velocity), rebuild, Preserve Groove, A/B, WAV export |
| 4 | Clap, rimshot, toms, crash, ride |
| 5 | Mixer per instrument, Reverb + Delay per instrument and on Master, stems with tails |
| 6 | Presets with samples, time signature detection, plugin validation |

## Layout

- `core/` — `drb_core`: analysis, MIDI, samples, audio engine. No GUI and no
  plugin code, so other JUCE apps (Adormis Audio) can use it directly.
- `plugin/` — the plugin and standalone app.
- `tests/` — synthetic drum beats with known hits, and the scorer.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # add -DDRB_JUCE_DIR=/path/to/JUCE to skip the download
cmake --build build -j8
./build/drb_tests_artefacts/Release/drb_tests              # checks + detection scores
./build/drb_tests_artefacts/Release/drb_tests --write out  # also saves the test beats (WAV + CSV)
```

AU builds only on macOS.

`drb_tests --analyse <file>` prints what the analysis finds in any recording,
`drb_tests --sort <folder>` how a folder of sounds would be sorted.

## Licence

AGPLv3 (see `LICENSE`), built with [JUCE](https://juce.com) 9.0.4.
