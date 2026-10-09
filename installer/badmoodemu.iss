; Inno Setup script for the BadMoodEmu Windows package.
; Built in CI: iscc /DMyAppVersion=x.y.z /DSrcDir=<artefacts Release dir> /DRepoDir=<repo root> installer\badmoodemu.iss
;
; Installs the VST3 bundle into the shared VST3 folder so nobody has to drag anything into
; a DAW (dragging a .vst3 into a DAW window does not install it, and dragging out of
; Windows' zip preview copies an incomplete bundle; both look like "import failed").
; The standalone app is an optional component for playing without a DAW.

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#ifndef SrcDir
  #define SrcDir "..\native\build\plugin\BadMoodEmu_artefacts\Release"
#endif
#ifndef RepoDir
  #define RepoDir ".."
#endif

[Setup]
AppId={{06A8B10B-6230-4FF5-8CF1-ACCB30375773}
AppName=BadMoodEmu
AppVersion={#MyAppVersion}
AppPublisher=cba-guitar-pedal (GPLv3)
AppPublisherURL=https://github.com/maxteicheira/cba-guitar-pedal-
DefaultDirName={autopf}\BadMoodEmu
DefaultGroupName=BadMoodEmu
DisableProgramGroupPage=yes
LicenseFile={#RepoDir}\LICENSE
OutputBaseFilename=BadMoodEmu-{#MyAppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName=BadMoodEmu {#MyAppVersion}

[Types]
Name: "full"; Description: "VST3 plugin and standalone app"
Name: "plugin"; Description: "VST3 plugin only"
Name: "custom"; Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (installs to the shared VST3 folder)"; Types: full plugin custom; Flags: fixed
Name: "standalone"; Description: "Standalone app (play the pedal without a DAW)"; Types: full

[Files]
; VST3 is a bundle (a folder), so the whole tree is copied.
Source: "{#SrcDir}\VST3\BadMoodEmu.vst3\*"; DestDir: "{commoncf64}\VST3\BadMoodEmu.vst3"; \
    Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcDir}\Standalone\BadMoodEmu.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "{#RepoDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepoDir}\docs\WINDOWS-INSTALL.md"; DestDir: "{app}"; DestName: "README.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\BadMoodEmu"; Filename: "{app}\BadMoodEmu.exe"; Components: standalone
Name: "{group}\BadMoodEmu read me"; Filename: "{app}\README.txt"

[Run]
Filename: "{app}\BadMoodEmu.exe"; Description: "Start BadMoodEmu now"; \
    Components: standalone; Flags: nowait postinstall skipifsilent

[Messages]
FinishedLabel=BadMoodEmu is installed.%n%nThe VST3 went into the shared VST3 folder. In your DAW, rescan plug-ins (Reaper: Options > Preferences > Plug-ins > VST > Re-scan) and BadMoodEmu will appear in the list. Do not drag the .vst3 into the DAW window: it is already installed.
