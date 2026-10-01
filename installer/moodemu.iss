; Inno Setup script for the MoodEmu Windows package.
; Built in CI: iscc /DMyAppVersion=x.y.z /DSrcDir=<artefacts Release dir> /DRepoDir=<repo root> installer\moodemu.iss
;
; Installs the VST3 bundle into the shared VST3 folder so nobody has to drag anything into
; a DAW (dragging a .vst3 into a DAW window does not install it, and dragging out of
; Windows' zip preview copies an incomplete bundle; both look like "import failed").
; The standalone app is an optional component for playing without a DAW.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef SrcDir
  #define SrcDir "..\native\build\plugin\MoodEmu_artefacts\Release"
#endif
#ifndef RepoDir
  #define RepoDir ".."
#endif

[Setup]
AppId={{433495E8-894D-47BE-AD1E-CA21390A03A3}
AppName=MoodEmu
AppVersion={#MyAppVersion}
AppPublisher=cba-guitar-pedal (GPLv3)
AppPublisherURL=https://github.com/maxteicheira/cba-guitar-pedal-
DefaultDirName={autopf}\MoodEmu
DefaultGroupName=MoodEmu
DisableProgramGroupPage=yes
LicenseFile={#RepoDir}\LICENSE
OutputBaseFilename=MoodEmu-{#MyAppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName=MoodEmu {#MyAppVersion}

[Types]
Name: "full"; Description: "VST3 plugin and standalone app"
Name: "plugin"; Description: "VST3 plugin only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (installs to the shared VST3 folder)"; Types: full plugin custom; Flags: fixed
Name: "standalone"; Description: "Standalone app (play the pedal without a DAW)"; Types: full

[Files]
; VST3 is a bundle (a folder), so the whole tree is copied.
Source: "{#SrcDir}\VST3\MoodEmu.vst3\*"; DestDir: "{commoncf64}\VST3\MoodEmu.vst3"; \
    Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcDir}\Standalone\MoodEmu.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\docs\WINDOWS-INSTALL.md"; DestDir: "{app}"; DestName: "README.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\MoodEmu"; Filename: "{app}\MoodEmu.exe"; Components: standalone
Name: "{group}\MoodEmu read me"; Filename: "{app}\README.txt"

[Run]
Filename: "{app}\MoodEmu.exe"; Description: "Start MoodEmu now"; \
    Components: standalone; Flags: nowait postinstall skipifsilent

[Messages]
FinishedLabel=MoodEmu is installed.%n%nThe VST3 went into the shared VST3 folder. In your DAW, rescan plug-ins (Reaper: Options > Preferences > Plug-ins > VST > Re-scan) and MoodEmu will appear in the list. Do not drag the .vst3 into the DAW window: it is already installed.
