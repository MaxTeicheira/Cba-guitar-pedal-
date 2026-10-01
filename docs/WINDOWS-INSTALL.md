# MoodEmu on Windows

## Easiest: run the installer

Download `MoodEmu-<version>-windows-x64-setup.exe` from the release page and run it.
It puts the VST3 in the shared VST3 folder and can also install the standalone app.
Windows SmartScreen may warn once because the installer is not code-signed: click
"More info", then "Run anyway".

Then rescan plug-ins in your DAW:

- Reaper: Options > Preferences > Plug-ins > VST > Re-scan (use "Clear cache/re-scan" if
  MoodEmu was installed before).
- Most other DAWs rescan on startup or have a Rescan button in their plug-in settings.

MoodEmu appears in the plug-in list as "MoodEmu" (VST3, cba-guitar-pedal).

## Manual install from the zip

`MoodEmu.vst3` is a **bundle**: a folder, not a single file.

1. Right-click the downloaded zip, choose Properties, tick **Unblock** if that box is
   there, then Extract All. Do not drag files out of the zip preview window; that copies
   an incomplete folder.
2. Copy the whole `MoodEmu.vst3` **folder** into `C:\Program Files\Common Files\VST3\`.
3. Rescan plug-ins in your DAW.

Dragging a .vst3 into a DAW window does not install a plugin. DAWs find plugins by
scanning the VST3 folder.

## If it does not show up

- 64-bit DAWs only; there is no 32-bit or ARM64 build.
- Check that this file exists:
  `C:\Program Files\Common Files\VST3\MoodEmu.vst3\Contents\x86_64-win\MoodEmu.vst3`.
- Read your DAW's plug-in scan log for the reason it rejected the file.
- The build links the Visual C++ runtime statically, so no redistributable is needed.

## Using it

- **Footswitches**: click and hold them, or press and hold **A** (wet) and **L** (loop)
  while the plugin window has focus. Both are also host parameters ("Wet footswitch",
  "Loop footswitch"), so you can map them to a MIDI pedal through your DAW.
- **Loop switch**: the looper is always listening. Tap to loop what you just played,
  hold from off to record a new loop, hold while playing to overdub, tap while playing
  to stop.
- **Wet switch**: tap for on/off, hold to freeze. With the LATCH dip on, a hold toggles freeze.
- **Input**: *Live input* processes the track (or your interface in the standalone).
  *Demo guitar* plays a built-in fingerpicked phrase. *Load file...* loops any WAV, AIFF,
  FLAC, MP3 or OGG file.

### Standalone app: no sound from the guitar

JUCE's standalone mutes the input at first launch to avoid feedback through speakers.
Click **Settings...** in the yellow bar, pick your interface, and untick "Mute audio input"
(use headphones). If the input meter stays empty, set *Audio device type* to
**Windows Audio (Exclusive Mode)** at 48000 Hz; some interfaces deliver only silence in
shared mode.
