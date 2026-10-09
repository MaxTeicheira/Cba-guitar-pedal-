# BadMoodEmu on Windows

BadMoodEmu is an unofficial software emulation of Chase Bliss Audio's BAD MOOD. It is not
affiliated with or endorsed by Chase Bliss.

## Easiest: run the installer

Download `BadMoodEmu-<version>-windows-x64-setup.exe` from the release page and run it.
It puts the VST3 in the shared VST3 folder and can also install the standalone app.
Windows SmartScreen may warn once because the installer is not code-signed: click
"More info", then "Run anyway".

Then rescan plug-ins in your DAW:

- Reaper: Options > Preferences > Plug-ins > VST > Re-scan (use "Clear cache/re-scan" if
  BadMoodEmu was installed before).
- Most other DAWs rescan on startup or have a Rescan button in their plug-in settings.

BadMoodEmu appears in the plug-in list as "BadMoodEmu" (VST3, cba-guitar-pedal). It installs
next to MoodEmu (the v0.1.0 MOOD emulator) and does not replace it.

## Manual install from the zip

`BadMoodEmu.vst3` is a **bundle**: a folder, not a single file.

1. Right-click the downloaded zip, choose Properties, tick **Unblock** if that box is
   there, then Extract All. Do not drag files out of the zip preview window; that copies
   an incomplete folder.
2. Copy the whole `BadMoodEmu.vst3` **folder** into `C:\Program Files\Common Files\VST3\`.
3. Rescan plug-ins in your DAW.

Dragging a .vst3 into a DAW window does not install a plugin. DAWs find plugins by
scanning the VST3 folder.

## If it does not show up

- 64-bit DAWs only; there is no 32-bit or ARM64 build.
- Check that this file exists:
  `C:\Program Files\Common Files\VST3\BadMoodEmu.vst3\Contents\x86_64-win\BadMoodEmu.vst3`.
- Read your DAW's plug-in scan log for the reason it rejected the file.
- The build links the Visual C++ runtime statically, so no redistributable is needed.

## Using it

- **Footswitches**: click and hold them, or press and hold **A** (left, wet channel) and
  **L** (right, micro-looper) while the plugin window has focus. Both are also host
  parameters, so you can map them to a MIDI pedal through your DAW.
- **Left switch (wet)**: tap for on/off, hold to freeze. With the LATCH dip on, a hold
  toggles freeze.
- **Right switch (micro-looper)**: the looper is always listening. Tap to play back what
  you just played, tap again to go back to listening (this replaces the loop as you play),
  hold to overdub.
- **Both switches**: hold both to use the knobs for the Hidden Options (the Hidden Options
  panel shows them too). Tap both three times quickly for true bypass; tap either to leave.
- **Presets**: the PRESET toggle between the footswitches recalls Preset 1 / Live /
  Preset 2. Save with the Save buttons, or the pedal's gesture: hold one switch for 3 s,
  then also hold the other for 3 s (right first saves Preset 2, left first Preset 1).
- **MIDI**: the plugin follows the BAD MOOD MIDI CC map (knobs, toggles, dips, Hidden
  Options) on any channel, and Program Change 0/1/2 for Live / Preset 1 / Preset 2.
- **Input**: *Live input* processes the track (or your interface in the standalone).
  *Demo guitar* plays a built-in fingerpicked phrase. *Load file...* loops any WAV, AIFF,
  FLAC, MP3 or OGG file.

### Standalone app: no sound from the guitar

JUCE's standalone mutes the input at first launch to avoid feedback through speakers.
Click **Settings...** in the yellow bar, pick your interface, and untick "Mute audio input"
(use headphones). If the input meter stays empty, set *Audio device type* to
**Windows Audio (Exclusive Mode)** at 48000 Hz; some interfaces deliver only silence in
shared mode.
