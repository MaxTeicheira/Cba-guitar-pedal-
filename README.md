# BadMoodEmu: a Chase Bliss BAD MOOD emulator

BadMoodEmu is an unofficial software emulation of Chase Bliss Audio's **BAD MOOD**: a wet
channel of spatial effects (Soup / Relay / Flip) next to an always-listening micro-looper
(Burst / Radio / Mask). The two run at a shared, variable sample rate set by the CLOCK knob
and are glued together at the end of the chain.

The MOOD MKII emulator (MoodEmu) that this repository used to contain is still available as
the [v0.1.0 release](../../releases/tag/v0.1.0).

It ships in two forms built from the same DSP:

| | Where | What |
|---|---|---|
| **Web** | `src/` | Runs in a browser (AudioWorklet). Demo phrase, audio file or live input. |
| **VST3 + standalone** | `native/` | JUCE plugin for any 64-bit DAW, plus a standalone app. Windows installer and zip, and a macOS zip, on the [Releases](../../releases) page. |

The C++ core (`native/dsp/badmood.h`) is a line-for-line port of the JS reference
(`src/badmood-worklet.js`). CI renders the same scenarios through both and requires them to
match (`tests/parity/`: max |difference| < 1e-3 and SNR > 80 dB). Measured on macOS: 6 of the
8 scenarios are bit-identical and the other two differ by one float ulp (libm rounding).

**Not affiliated with or endorsed by Chase Bliss.** BAD MOOD is their trademark. The
algorithms here are our own approximations, written from the public user manual and MIDI
manual; Chase Bliss has not published theirs. No Chase Bliss logos or artwork are used.

---

## Where the behaviour comes from: please check these

Source: the official **BAD MOOD manual** (BAD1.1) and **BAD MOOD MIDI manual**. Page
numbers below are the printed page numbers. Every row is tagged:

- **Documented**: stated in the manual or the MIDI manual (page cited).
- **Derived**: follows from a documented statement plus one documented fact about another product.
- **Assumed**: our interpretation where the manuals are silent. Please confirm or correct.
- **Estimated**: a number we picked so it sounds right. The real values are not published.

### Shared controls

| Control | Emulator behaviour | Status |
|---|---|---|
| MIX | Dry/wet for both channels: dry stays at unity up to noon then fades, effects rise to full at noon (the MoodEmu curve); unity dry when nothing is engaged | Function documented (p.8); curve assumed (kept from MoodEmu) |
| CLOCK | Sample rate of both channels in harmonized steps 2k / 3k / 4k / 6k / 8k / 12k / 16k / 24k / 32k / 48k / 64k, at the CC18 positions; 64k → 32k halves the speed of the loop and the wet effect | Documented (p.8, p.20; MIDI manual p.5). CC value 16 is listed for both 3k and 4k; we use 3k |
| CLOCK (cap) | Cannot run faster than the host, so steps above the host rate (usually 48 kHz) play at the host rate. The knob positions still match the pedal | Emulator limitation |
| CLOCK glide | 60 ms glide between steps | Estimated |
| ROUTING | What the wet channel processes when **both** channels are on: input only / input + micro-loop / micro-loop only | Documented (p.8, p.40) |
| Routed loop | A micro-loop sent through the wet channel is 100 % wet; BLEND mixes the clean loop back | Documented (p.41) |
| Recording the wet channel | While the looper is bypassed (always listening) it records the input plus the wet channel's output, whatever the routing | Documented (p.31, p.41); the wet is recorded at unity, before MIX: assumed |
| PRESETS | Preset 1 / Live / Preset 2. Every knob, toggle, dip and hidden option is stored. Leaving Live remembers the live controls; returning restores them | Documented (p.8, p.18) |
| Preset save | Footswitch gesture: hold one switch 3 s, then hold the other 3 s as well (right first saves the right slot, Preset 2). Also Save buttons in both UIs | Gesture documented (p.18); "keep holding the first" assumed |

### Wet channel (left footswitch)

| Control | Mode | Emulator behaviour | Status |
|---|---|---|---|
| TIME | Soup | Decay: T60 0.4 s → 40 s, infinite above 98.5 % | Function documented (p.10, p.24); range estimated |
| MODIFY | Soup | Character. CCW: diffuse (random phases, smoothed spectrum, highs decay up to 2.5x faster). CW: synthetic (phase-coherent sustained partials, spectral peaks emphasised, flat decay) | "Clockwise = more synthetic" documented (p.24); what character means is assumed |
| Soup engine | | STFT spectral reverb at the CLOCK rate: 1024-point hand-written radix-2 FFT, hop 256, Hann windows; each bin keeps a magnitude that decays (peak-hold) and a phasor advanced by the measured phase-vocoder rotation plus random jitter. Low CLOCK = long smeared frames; high CLOCK = short frames that sparkle | "Spectral reverb that resynthesizes" and the CLOCK character documented (p.24); the design is ours |
| TIME | Relay | Delay time 30 ms → 1.5 s | Function documented (p.10, p.25); range estimated |
| MODIFY | Relay | Exactly N equal-volume repeats, N = 1 … 8; above 94 % the repeats never stop and pile up like a looper | Documented (p.25); N max 8 and the 94 % point estimated |
| TIME | Flip | Lag between successive notes: 0 → 0.8 s (quadratic, so the "vintage" small lags are easy to set) | Function documented (p.10, p.26); range estimated |
| MODIFY | Flip | The 48 harmonies of the CC17 table (−oct, −5th, −4th, +4th, +5th, +oct; 1–4 notes, more notes higher up), at the CC17 positions. Note k is delayed by k × lag. Output is the harmony notes only (your dry note comes from MIX) | Table documented (p.26–27; MIDI manual p.7–8); just intervals (3/2, 4/3) and "harmony only" assumed |
| Flip engine | | One SOLA-style delay-line pitch shifter per note (correlation-aligned crossfades, pitch accurate to < 1 %) | Ours |
| Tap / hold | all | Tap: on/off. Hold: freeze (Soup: ambient pad; Relay: looping echo of the last period; Flip: the chord pattern repeats with period max(notes × lag, 0.25 s)) | Documented (p.10, p.22); freeze details assumed |
| Hold threshold | | 350 ms | Estimated |

### Micro-looper (right footswitch)

| Behaviour | Emulator | Status |
|---|---|---|
| Always listening | A circular buffer records continuously while the looper is bypassed; tap to play back what you just played | Documented (p.12, p.30) |
| Loop length | Fixed sample count, so CLOCK sets the duration: **128 000 samples** (2 s at 64 kHz … 64 s at 2 kHz; 2.67 s at a 48 kHz host). HALF: 64 000 samples | Derived: HALF "cuts the loop length in half, matching the original MOOD" (p.45), and MoodEmu's MOOD MKII buffer is 64 000 samples |
| LED | Blinks once per loop length while recording; solid while playing; fast blink while overdubbing | Blink while recording documented (p.30); the rest assumed |
| Tap | Alternates recording and playback | Documented (p.12) |
| Hold | Overdub (momentary; with LATCH, a hold toggles it). Hold from recording engages playback and overdubs | Documented (p.12, p.44); "hold from recording" assumed |
| Overdub | Records only the clean input, never the wet channel; recorded at the current CLOCK, so notes stay where you played them | Documented (p.32–33) |
| REPLACE | Tapping back to recording starts overwriting the loop with the input as the write head passes | Documented (p.33) |
| Mode switching | Never erases the loop | Documented (p.31) |
| FADE (hidden) | Overdubs keep 25 % … 100 % of the old loop per pass (max = no fade, the default) | Documented (p.15); range estimated |
| Seam | 4 ms fades hide the jump where recording stopped | Ours |

| Control | Mode | Emulator behaviour | Status |
|---|---|---|---|
| LENGTH | Burst | Step size, tied to the loop: L/64, L/48, L/32, L/24, L/16, L/12, L/8, L/6, L/4 | Function documented (p.12, p.34); values estimated |
| MODIFY | Burst | Envelope sensitivity: live input above the threshold (0.5 → 0.01 envelope) scrambles the pattern: random steps, 35 % of them half-length ratchets | Documented (p.34); values estimated |
| Burst slices | | Up to 8 steps at the strongest energy jumps (≥ 3x per 256-sample block, minimum spacing L/16), in loop order; if nothing stands out, four equal steps. Overdubs go into the loop, not the sequence | "Up to 8 steps at unique sounds" and overdub behaviour documented (p.34–35); detector ours |
| MODIFY | Radio | Scans five stations at the CC19 positions (Tape 0, Ambient 32, Orchestral 63, Shoegaze 97, Dance 127). Clean spot ±0.035 around each; between them cross-talk (equal-power blend), static, crackle, a heterodyne whistle and a band-limited "radio" tone | Stations and the clean spots documented (p.36–37; MIDI manual p.5); widths and the interference design assumed |
| LENGTH | Radio: Tape | Speed and direction: reverse 2x → 0.5x, forward 0.5x → 1x (noon) → 2x, snapping to octaves | Documented (p.37); map estimated (from MoodEmu's TAPE) |
| LENGTH | Radio: Ambient | Pitch-preserving slow-down 1/16x → 1x (4 overlapping 200 ms grains, jitter grows as it slows) | Documented (p.37); numbers estimated |
| LENGTH | Radio: Orchestral | 1 … 6 voices: playheads at 1x, ½x, 2x, 1.5x, ¾x, 4/3x with their own offsets and pans, fading in and out at random (1–4 s) | Documented (p.37); voice set estimated |
| LENGTH | Radio: Shoegaze | Moment selector over 8 frozen moments (granular freeze), with an octave-up layer and the opposite moment stacked on top | Documented (p.37); layering assumed |
| LENGTH | Radio: Dance | Rotation speed: normal → half → double speed, every L, L/2, L/4, L/8 or L/16, re-synced to the loop each step | Documented (p.37); periods estimated |
| MODIFY | Mask | Threshold on the loop's own envelope: 0.25 (1 − MODIFY)²; fully down = the pure loop (bit-for-bit) | Documented (p.12, p.38–39); curve estimated |
| LENGTH | Mask | Character of the disguise, crossfading through: octave down, ring mod, bit crush, envelope band-pass, reverse grains, octave up | Function documented (p.38); treatments assumed |

### Hidden options (both footswitches held; p.14–17)

On the pedal the knobs and toggles take these jobs while both switches are held. Both UIs
do the same (the knobs turn green) and also show every option in a Hidden Options panel.

| Option | Position | Emulator | Status |
|---|---|---|---|
| CROSS | TIME | Intensity of the modulation (default off) | Documented (p.14, p.43) |
| INPUT MOD | loop toggle | Source: wet modulates loop / input modulates both (default) / loop modulates wet | Documented (p.16, p.43) |
| Cross engine | | Envelope of the source bends pitch (a 0–12 ms modulated delay) and sputters the level (10–80 Hz) of the target channel(s) | Design and numbers estimated |
| LEVEL BALANCE | MIX | Relative level of the two channels (default even) | Documented (p.15) |
| FADE | LENGTH | See micro-looper | Documented (p.15) |
| EQ | wet MODIFY | Tilt on both channels: CCW low-pass 20 kHz → 500 Hz, CW high-pass 20 Hz → 2.5 kHz, flat at noon | Documented (p.15); ranges estimated |
| GLUE | CLOCK | End-of-chain saturator on both channels: biased tanh (warm), above 55 % folding and bit-crushing (destroy). Drive 0 → 34 dB, level-compensated. Default 20 % | Documented (p.16, p.42); default read from the p.14 drawing; curve estimated |
| BLEND | loop MODIFY | Clean micro-loop mixed back when the loop is routed through the wet channel (default none) | Documented (p.15, p.41) |
| SYNC | wet toggle | Left: loop length = 4 Relay periods (set by TIME). Right: TIME steps through 1/32 … 1 of the loop length (Relay delay and Flip lag). Middle: unsynced (default) | Function documented (p.16); "4 periods" and the fractions assumed |
| SPREAD | routing toggle | Which channel the SPREAD dip affects: wet only / both (default) / loop only | Documented (p.16) |
| EFFECT VOLUME | right switch held + MIX | Both channels −12 → +12 dB, unity at noon; saved per preset | Documented (p.17); range estimated |
| Reset | | A "Reset hidden options" button | The pedal's gesture (flip the preset toggle three times, p.17) is not emulated |

### Dip switches (p.44–45) and ramping (p.46–47)

| Dip | Emulator | Status |
|---|---|---|
| MISO | The left input feeds both sides (core and dry) | Documented (p.4, p.44) |
| SPREAD | Stereo per mode: Soup decorrelated phases; Relay ping-pong; Flip notes panned; Burst steps alternate; Orchestral voices panned; others decorrelated | Documented that each mode has its own approach (p.44); approaches assumed |
| DRY KILL | No dry signal, even bypassed | Documented (p.44) |
| TRAILS | The wet channel rings out after it is switched off | Documented (p.44) |
| LATCH | Holds toggle instead of being momentary | Documented (p.44) |
| SMOOTH | CLOCK is continuous (log-interpolated through the step positions) | Documented (p.45) |
| DRY GLUE | GLUE also processes the dry signal (while a channel is engaged) | Documented (p.42, p.45); "while engaged" assumed |
| HALF | Loop buffer 64 000 samples | Documented (p.45) |
| Ramp dips TIME / MODIFY / CLOCK / MODIFY / LENGTH, BOUNCE, SWEEP | Bounce: the selected knobs move continuously between the knob position and the bottom (SWEEP off) or top (SWEEP on). Ramp (BOUNCE off): one movement up to (or down to) the knob when the pedal is switched on. MIX becomes the ramp speed (period 20 s → 0.25 s) unless the left switch is held | Documented (p.46–47; dip order from MIDI manual p.7); period range estimated |
| POLARITY | Stored, but only meaningful for CV/expression | Documented as a CV setting (p.48) |

### Footswitches

| Gesture | Emulator | Status |
|---|---|---|
| Tap | Decided on release (< 350 ms) | Assumed |
| Hold | Fires at 350 ms | Estimated |
| Both held | Hidden options; no tap or hold action happens | Documented (p.14) |
| Both tapped three times (within 1.5 s) | True bypass: output is the untouched input, the looper stops listening, the three LEDs blink red; tap either to leave | Documented (p.17); window estimated |
| Knob moved while a switch is held | That switch does nothing on release (as with EFFECT VOLUME and the ramp MIX) | Assumed |
| Defaults | Knobs at noon except CLOCK (max); Soup, routing In, Radio | Assumed |

### MIDI (plugin)

The plugin reads the BAD MOOD CC map (MIDI manual p.3–8) on any channel: knobs 14–20, toggles
21–23 (0/1, 2, >2), hidden options 24–33, dips 61–68 and 71–78, footswitch functions 102
(wet), 103 (looper), 105 (freeze), 106 (overdub), 55 (true bypass). Program Change 0 = Live,
1 = the right slot (Preset 2), 2 = the left slot (Preset 1), following the MIDI manual
(p.2), which numbers the slots right to left. CC102/103 are both labelled BYPASS in the MIDI
manual; we assume 102 is the left (wet) switch.

### Not emulated

- MIDI clock sync, clock divisions (CC51/53/54), tap tempo (CC93) and their effect on the sample rate.
- Synth Mode (MIDI notes, ADSR, portamento, pitch bend, mod wheel; CC57–59, 80–84).
- More than two preset slots (the pedal has 122 over MIDI), preset save over CC111, factory reset (CC56), MIDI reset (CC110), stop ramping (CC52), expression over MIDI (CC100), hidden-option access over MIDI (CC104), per-channel EQ (CC85), MIDI channel selection (the plugin listens to every channel).
- CV, expression pedal and external footswitch input (not applicable to a plugin).
- The preset-toggle gesture that resets the hidden options (a button instead).
- The analog buffered bypass and anything about the hardware's converters and noise.

---

## How it works

```
in L/R ─┬──────────────────────────────────────────────────── dry L/R ─────────────────┐
        └─ mono ─ anti-alias LPF ─► [ CORE at CLOCK rate ] ─► reconstruction LPF ─► EQ ─► GLUE ─► volume ─► MIX ─► out
                                      │
          1. micro-looper read: 128 000-sample buffer, Burst / Radio / Mask
          2. wet channel (ROUTING: input / input + loop / loop): Soup / Relay / Flip
          3. CROSS: the source's envelope bends and sputters the target channel(s)
          4. micro-looper write: always listening (input + wet) or overdub (clean input)
          5. LEVEL BALANCE, BLEND
```

- **Clock domain.** Everything inside the core ticks at `fs × c` (c = CLOCK / host rate).
  Times are counted in core samples, so a lower clock lengthens delays and loops, lowers the
  bandwidth (4th-order Butterworth filters at 0.45 × clock) and pitches down what is already
  in the buffers.
- **No allocation on the audio thread.** Every buffer is sized in `prepare()` (largest:
  Relay at 12 s × host rate); suitable for an embedded port.
- **Determinism.** One 32-bit LCG per random source, no sorting library calls, the same
  hand-written FFT in both languages, and the same evaluation order, so the JS and C++
  outputs match.

## Run the web version

Serve `src/` with any static server (AudioWorklets need http, not `file://`):

```sh
cd src && python3 -m http.server 8000   # then open http://localhost:8000
```

Press **Start audio**. Keyboard: hold **A** for the left (wet) switch and **L** for the right
(looper) switch; hold both for the hidden options.

## Build the plugin

Requires CMake ≥ 3.22 and a C++17 compiler. JUCE 8.0.9 is fetched automatically.

```sh
cmake -S native -B build-plugin -DBADMOOD_BUILD_PLUGIN=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-plugin --config Release --target BadMoodEmu_Standalone BadMoodEmu_VST3
```

Artefacts land in `build-plugin/plugin/BadMoodEmu_artefacts/Release/`. On Linux, install
JUCE's dependencies first (`libasound2-dev libx11-dev libxrandr-dev libxinerama-dev
libxcursor-dev libfreetype-dev libfontconfig1-dev`).

## Tests

```sh
node --test test/*.test.js                                   # JS reference DSP (behaviour)
cmake -S native -B build && cmake --build build              # C++ core + renderer
./build/dsp_tests && node tests/parity/compare.js build/badmood_render   # C++ tests, JS↔C++ parity
```

The behavioural tests check, among others: loop length against CLOCK and HALF; Relay's
exactly-N equal repeats; Flip's intervals (Goertzel on a sine) and lag; freeze in every wet
mode; Burst's ≤ 8 slices and scrambling; five distinct Radio stations with static between
them; Mask at MODIFY 0 equal to the pure loop sample for sample; Glue adding harmonics;
presets round-tripping; and the footswitch gestures. The parity scenarios cover every wet
and loop mode, all five stations, Glue, Cross, routing, stereo/MISO and the gestures.

CI (`.github/workflows/build.yml`) runs these on Windows and macOS, then on Windows:
checks the binaries have no dynamic MSVC runtime dependency, validates the VST3 with
pluginval (strictness 8), launches the standalone and screenshots it, builds the Inno Setup
installer and zip, and re-validates the plugin extracted from that zip.

## Releasing

Bump `VERSION` (e.g. to `0.2.0`) and push to `main`: when tag `v<VERSION>` does not exist
yet, CI creates it and publishes the release. Pushing a `vX.Y.Z` tag, or running the workflow
manually with `release_version`, works too. CI attaches
`BadMoodEmu-X.Y.Z-windows-x64-setup.exe`, `BadMoodEmu-windows-x64.zip` and
`BadMoodEmu-macos.zip` to a GitHub release. See
[docs/WINDOWS-INSTALL.md](docs/WINDOWS-INSTALL.md) for install help. The Windows installer
has its own AppId, so BadMoodEmu installs next to MoodEmu.

## License

GPLv3 (see `LICENSE`). The plugin links JUCE 8, which is available under AGPLv3 or a
commercial license; distributing binaries under the open-source option means complying
with AGPLv3.
