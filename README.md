# MoodEmu: a Chase Bliss MOOD emulator

An unofficial software emulation of the Chase Bliss Audio **MOOD** (MKII layout): a wet
channel (reverb / delay / slip) next to an always-listening micro-looper (env / tape / stretch),
both running at a variable sample rate set by the CLOCK knob.

It ships in two forms built from the same DSP:

| | Where | What |
|---|---|---|
| **Web** | `src/` | Runs in a browser (AudioWorklet). Demo phrase, audio file or live input. |
| **VST3 + standalone** | `native/` | JUCE plugin for any 64-bit DAW, plus a standalone app. Windows installer and zip on the [Releases](../../releases) page. |

The C++ core (`native/dsp/mood.h`) is a line-for-line port of the JS reference
(`src/mood-worklet.js`). CI renders the same scenarios through both and requires them to
match (`tests/parity/`); on Linux they are bit-identical.

Not affiliated with or endorsed by Chase Bliss. The algorithms are my own approximations;
Chase Bliss has not published theirs.

---

## Where the parameters come from: please check these

I could not open the manuals directly: this build environment's network policy blocks the
PDF hosts (squarespace, manualslib). What I have comes from **search-engine excerpts** of the
official manuals and reviews, plus general knowledge of the pedal. Every row below is tagged:

- **Documented**: stated in Chase Bliss material (manual, fact sheet or product page) as quoted in search excerpts.
- **Assumed**: my interpretation where the sources I could see were silent. Please confirm or correct.
- **Estimated**: a number I picked so it sounds right. The real values are not published in anything I could reach.

### Knobs

| Control | Mode | Emulator behavior | Status |
|---|---|---|---|
| TIME | Reverb | Decay/size: T60 0.4 s → 25 s, ∞ at full CW | Function documented; range estimated |
| TIME | Delay | Delay time 20 ms → 2 s (exponential) | Function documented; range estimated |
| TIME | Slip | Refresh rate (slice length) 50 ms → 1.5 s | Function documented; range estimated |
| MIX | — | Balance of input vs MOOD. Dry stays at unity up to noon then fades; effect rises to full at noon | Function documented; curve assumed |
| LENGTH | Env | Slice size: 10 ms (grains) → 1 s (short phrases) | Function documented; range estimated |
| LENGTH | Tape | Loop length: 2 % → 100 % of the captured loop, keeping the most recent part | Function documented; mapping assumed |
| LENGTH | Stretch | Slice (grain) size 10 → 500 ms | Function documented; range estimated |
| MODIFY (wet) | Reverb | "Smear": more input diffusion, deeper delay-line modulation, softer highs | Name documented; what smear does is assumed |
| MODIFY (wet) | Delay | Feedback 0 → 110 % (self-oscillates past ~3 o'clock, soft-limited) | Function documented; range assumed |
| MODIFY (wet) | Slip | Playback speed and direction: reverse 2x → 0.5x, forward 0.5x → 1x (noon) → 2x, snapping to octaves | Function documented; map estimated |
| CLOCK | — | Sample rate of both channels, 2 kHz → 64 kHz. Lower = longer delays/loops, darker, grittier, and existing buffer contents drop in pitch | Function documented; 2–64 kHz from a search excerpt of the MKII manual |
| MODIFY (loop) | Env | Envelope detector sensitivity (CW = more sensitive) | Documented |
| MODIFY (loop) | Tape | Speed and direction (same map as Slip, tighter octave snap, tape-style glide) | Function documented; map estimated |
| MODIFY (loop) | Stretch | Stretch amount and direction: noon freezes, CW forward 1/8x → 2x, CCW the same in reverse, pitch unchanged | Function documented; map assumed |

The emulator cannot run faster than the host, so CLOCK settings above the host rate (usually
44.1/48 kHz) are capped there. The knob positions still match the pedal.

### Toggles and routing

| Control | Emulator behavior | Status |
|---|---|---|
| Wet toggle | Reverb / Delay / Slip | Documented |
| Loop toggle | Env / Tape / Stretch | Documented |
| Routing toggle | What feeds the wet channel: **In** (input only), **In + Loop** (both), **Loop** (looper only). Only matters when both channels are on | Documented |
| Routing (detail) | When the loop is routed into an active wet channel, it is heard only through the wet channel, not also directly | Assumed |

### Micro-looper

| Behavior | Emulator | Status |
|---|---|---|
| Always listening | Records continuously, even when off. Engaging it plays back what you just played | Documented |
| LED while off | Blinks each time the buffer completes a new loop | Documented |
| Loop length | The buffer is a fixed 64 000 samples, so CLOCK sets the length: 1 s at 64 kHz → 32 s at 2 kHz | Derived: the MKI manual gives 0.5 s (CW) → 16 s (CCW), and MKII "doubles loop length" |
| Env mode | Playing interrupts the loop. The slice under the playhead repeats while you play; when you stop, the loop resumes in time | Documented |
| Overdub | Present on MKII | Documented that it exists; gesture assumed (below) |

### Footswitches

| Gesture | Emulator | Status |
|---|---|---|
| Loop: tap when off | Start looping what you just played | Documented |
| Loop: tap when playing | Stop | Assumed |
| Loop: hold when off | Record a fresh loop for as long as you hold | Assumed |
| Loop: hold when playing | Overdub (old layers decay to 97 % per pass) | Assumed |
| Wet: tap | Wet channel on/off | Assumed |
| Wet: hold | Freeze the reverb tail / delay repeats / slip slice while held | Assumed |
| Hold threshold | 350 ms | Estimated |

### Dip switches

| Dip | Emulator | Status |
|---|---|---|
| Trails | Wet tails ring out after the wet channel is switched off | Name documented; behavior assumed (standard meaning) |
| Dry Kill | Removes the dry signal (for parallel loops) | Name documented; behavior assumed (standard meaning) |
| Latch | Wet-switch hold toggles freeze instead of being momentary | Name documented; behavior assumed |
| Spread | Wider stereo: decorrelated delay/slip, widened reverb | Name documented; behavior assumed |
| Smooth | Smooths CLOCK changes (1.5 s glide instead of 80 ms) | Documented ("smooth clock") |
| Balance (slider) | Level of wet vs loop channel | My addition. Not verified as a MOOD feature |

**Not emulated:** Classic (MKI) mode, MISO, Bounce, Sweep, Polarity, No Dub, ramping and
expression, MIDI, and presets. The source
excerpts named these, but I didn't have enough detail to model them.

### Sources

Search excerpts of these documents are the basis for the "Documented" rows:

- [MOOD MKII manual (Chase Bliss, PDF)](https://static1.squarespace.com/static/622176a9b8d15d57ffbf5700/t/64111ea7c3f6d569218566b4/1678843563430/MOOD+MKII_Manual_Pedal_Chase+Bliss.pdf)
- [MOOD (MKI) manual (Chase Bliss, PDF)](https://static1.squarespace.com/static/622176a9b8d15d57ffbf5700/t/622cbf0fad4c9b4e4b077fb5/1647099667314/MOOD_Manual_Pedal_Chase+Bliss.pdf)
- [MOOD MKII fact sheet (Chase Bliss, PDF)](https://static1.squarespace.com/static/622176a9b8d15d57ffbf5700/t/64111e44b7e4db6a404ad61f/1678843461129/MOOD+MKII_Whats+New_Pedal_Chase+Bliss.pdf)
- [MOOD MKII product page](https://www.chasebliss.com/mood-mkii)
- [Effects Database: MOOD MkII](https://www.effectsdatabase.com/model/chasebliss/mood/mk2)
- [Guitar Pedal X: MOOD MKII announcement](https://www.guitarpedalx.com/news/gpx-blog/chase-bliss-reboots-its-mood-micro-looper--ambience-generator-with-significant-enhancements-including-stereo-and-overdubbing)
- [Guitar.com: MOOD MkII review](https://guitar.com/reviews/effects-pedal/the-big-review-chase-bliss-mood-mkii/)
- [Pedal of the Day: MOOD](https://www.pedal-of-the-day.com/2019/06/24/chase-bliss-audio-m-o-o-d-granular-micro-looper-delay/)

If you have the manual handy, the rows tagged **Assumed** and **Estimated** are the ones to
check. Mappings live in one place per implementation (`speedMap`, `stretchMap`, `expMap`
calls in `src/mood-worklet.js` and `native/dsp/mood.h`), and the parity test keeps both in sync.

---

## How it works

```
input ─┬────────────────────────────────────────────── dry ─────┐
       └─ anti-alias LPF ─► [ CORE at CLOCK rate ] ─► reconstruction LPF ─► MIX ─► out L/R
                               │
                 always-recording history (64 000 samples)
                    └─► micro-looper (env / tape / stretch) ──┬──────────► loop out
                                                              │ routing
                 input / input+loop / loop ─► wet (reverb / delay / slip) ─► wet out
```

- **Clock domain.** Everything inside the core ticks at `fs × c` (c = CLOCK / host rate).
  Times are counted in core samples, so a lower clock automatically lengthens delays and
  loops, lowers bandwidth (4th-order Butterworth anti-alias and reconstruction filters at
  0.45 × clock) and pitches down what is already in the buffers, as on the pedal.
- **Reverb**: 8-line feedback delay network (Householder matrix), 4 input allpass diffusers,
  modulated line lengths, per-line damping.
- **Delay**: modulatable tape-style delay (time changes glide and bend pitch), low/high-pass in
  the feedback path, tanh limiting.
- **Slip**: two crossfading read heads replaying the last slice at another speed or direction.
- **Stretch**: two Hann-windowed grains at 1x pitch, with the grain position moving at the
  stretch speed.

## Run the web version

Serve `src/` with any static server (AudioWorklets need http, not `file://`):

```sh
cd src && python3 -m http.server 8000   # then open http://localhost:8000
```

Press **Start audio**. Keyboard: hold **A** for the wet switch and **L** for the loop switch.

## Build the plugin

Requires CMake ≥ 3.22 and a C++17 compiler. JUCE 8.0.9 is fetched automatically.

```sh
cd native
cmake -B build -DMOOD_BUILD_PLUGIN=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target MoodEmu_Standalone MoodEmu_VST3
```

Artefacts land in `native/build/plugin/MoodEmu_artefacts/Release/`. On Linux, install JUCE's
dependencies first (`libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev
libfreetype-dev libfontconfig1-dev`).

## Tests

```sh
node --test test/*.test.js                               # JS reference DSP
cmake -S native -B build && cmake --build build          # C++ core + renderer
./build/dsp_tests && node tests/parity/compare.js build/mood_render   # C++ tests, JS↔C++ parity
```

CI (`.github/workflows/build.yml`) runs these on Windows and macOS, then on Windows:
checks the binaries have no dynamic MSVC runtime dependency, validates the VST3 with
pluginval (strictness 8), launches the standalone and screenshots it, builds the Inno Setup
installer and zip, and re-validates the plugin extracted from that zip.

## Releasing

Push a tag `vX.Y.Z`, or run the **build** workflow manually (Actions → build → Run workflow)
with `release_version` set to `X.Y.Z`, which creates the tag for you. CI attaches `MoodEmu-X.Y.Z-windows-x64-setup.exe`,
`MoodEmu-windows-x64.zip` and `MoodEmu-macos.zip` to a GitHub release. See
[docs/WINDOWS-INSTALL.md](docs/WINDOWS-INSTALL.md) for install help.

## License

GPLv3 (see `LICENSE`). The plugin links JUCE 8, which is available under AGPLv3 or a
commercial license; distributing binaries under the open-source option means complying
with AGPLv3.
