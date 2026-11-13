; Thruma - Windows installer (Inno Setup 6)
; Build: iscc /DAppVersion=0.1.0 /DSourceDir=<dir containing Thruma.vst3> /DOutputDir=<out> packaging\windows\Thruma.iss

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\Source\Plugin\Thruma_artefacts\Release\VST3"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
AppId={{6B0C1E2A-7F4D-4B8E-9C61-5A3E2D7F9B10}
AppName=Thruma
AppVersion={#AppVersion}
AppPublisher=SacredHills
AppPublisherURL=https://github.com/SacredHills/thruma
DefaultDirName={autopf}\Thruma
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=Thruma-{#AppVersion}-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0
WizardStyle=modern
UninstallDisplayName=Thruma (VST3)

[Files]
; The VST3 bundle goes to the standard system-wide location: C:\Program Files\Common Files\VST3
Source: "{#SourceDir}\Thruma.vst3\*"; DestDir: "{commoncf64}\VST3\Thruma.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\docs\USER_GUIDE.md"; DestDir: "{app}"; Flags: ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Thruma.vst3"

[Messages]
FinishedLabel=Thruma has been installed to Common Files\VST3. Rescan plug-ins in your DAW to load it.
