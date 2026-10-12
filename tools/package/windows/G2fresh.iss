; G2fresh's Windows installer (Inno Setup 6.3 or later). Compiled by tools/package/windows.ps1:
;
;   ISCC /DAppVersion=0.10.0 /DStage=<folder> /DOutDir=<folder> /DOutName=G2fresh-0.10.0-Windows-x64-setup
;        [/DSigned "/Sg2sign=<command> $f"] G2fresh.iss
;
; Stage holds what is installed: G2fresh.exe, g2bridge.exe, G2fresh.vst3 (the VST3 bundle, a folder, with its own
; g2bridge.exe in Contents\Resources), LICENSE.txt and NOTICE.txt. With /DSigned, Setup and the uninstaller are
; signed by the "g2sign" sign tool given on the command line (windows\sign.ps1); the files in Stage are signed
; before, by windows.ps1.

#ifndef AppVersion
  #error AppVersion is not defined (see the top of this file)
#endif
#ifndef Stage
  #error Stage is not defined (see the top of this file)
#endif
#ifndef OutDir
  #define OutDir "."
#endif
#ifndef OutName
  #define OutName "G2fresh-" + AppVersion + "-Windows-x64-setup"
#endif

[Setup]
; Identifies G2fresh to Windows for upgrades and the uninstaller: never change it.
AppId={{D6076324-6951-4147-86DF-4477F0E1B033}
AppName=G2fresh
AppVersion={#AppVersion}
AppVerName=G2fresh {#AppVersion}
AppPublisher=G2fresh
AppPublisherURL=https://github.com/r0main-f/G2fresh
AppSupportURL=https://github.com/r0main-f/G2fresh/issues
AppUpdatesURL=https://github.com/r0main-f/G2fresh/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\G2fresh
DisableProgramGroupPage=yes
LicenseFile={#Stage}\LICENSE.txt
InfoAfterFile=after-install.txt
OutputDir={#OutDir}
OutputBaseFilename={#OutName}
; 64-bit only, also on Arm64 Windows, which runs x64 programs
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=G2fresh
UninstallDisplayIcon={app}\G2fresh.exe
#ifdef Signed
SignTool=g2sign
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "The app and the VST3 plugin"
Name: "custom"; Description: "Choose"; Flags: iscustom

[Components]
Name: "app"; Description: "G2fresh (stand-alone app)"; Types: full custom
Name: "vst3"; Description: "VST3 plugin, in Common Files\VST3"; Types: full custom

[Files]
Source: "{#Stage}\G2fresh.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion
; g2bridge owns the G2's USB connection; the app looks for it next to its executable
Source: "{#Stage}\g2bridge.exe"; DestDir: "{app}"; Components: app; Flags: ignoreversion
Source: "{#Stage}\LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Stage}\G2fresh.vst3\*"; DestDir: "{commoncf64}\VST3\G2fresh.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; the whole bundle anew: a file an older version had and this one has not must not stay
Type: filesandordirs; Name: "{commoncf64}\VST3\G2fresh.vst3"; Components: vst3

[Icons]
Name: "{autoprograms}\G2fresh"; Filename: "{app}\G2fresh.exe"; Components: app
