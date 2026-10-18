# Drum Rebuilder

*Working title: Drum Analyzer & Beat Rebuilder.*

A free, open-source audio plugin (VST3, AU, standalone) that listens to a
**drums-only** recording, finds the tempo, the bars and every hit (kick, snare,
hi-hats, clap, rimshot, toms, crash, ride) with velocity, swing and
microtiming, turns them into editable MIDI, and rebuilds the beat with your own
samples while keeping the groove.

**Status: stage 3 (in progress)** — browse and preview files, open a recording,
see its waveform with the bar and beat grid and every hit named (kick, snare,
hi-hat closed / open), fix tempo, bar 1, hits, instruments and each hit's IN /
OUT by hand, solo instruments, MIDI Editor with quantize and undo, MIDI
export. Each beat remembers its fixes. Sample library with one folder per
instrument (the beat's original hits are saved into it), and the beat rebuilt
from it: per instrument Original / Fixed / Random / Round robin / By velocity,
Next sounds, keep the original pitch, another tempo, A/B, WAV export.

Keys: Space play / pause, W / E start / end, Left / Right previous / next hit,
Up / Down zoom, Ctrl+Z / Ctrl+Shift+Z undo / redo, Delete removes the selected
hit in the MIDI Editor.

`drb_tests --analyse <file>` prints what the analysis finds in any recording.

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

## Licence

AGPLv3 (see `LICENSE`), built with [JUCE](https://juce.com) 9.0.4.
