; QUADmapper installer (ADR-057, without the AI engine for now: TD-44).
; Built by build-installer.ps1, which stages `cmake --install` and passes the defines:
;   ISCC /DAppVersion=1.0.0 /DStageDir=<staged app folder> /DOutputDir=<folder> QUADmapper.iss

#ifndef AppVersion
  #error AppVersion is not defined; run build-installer.ps1
#endif
#ifndef StageDir
  #error StageDir is not defined; run build-installer.ps1
#endif
#ifndef OutputDir
  #define OutputDir AddBackslash(SourcePath) + "out"
#endif

[Setup]
; The AppId identifies the app to Windows for upgrades and uninstall: never change it.
AppId={{96F59930-7E96-43D0-890F-BB004EE06DE4}
AppName=QUADmapper
AppVersion={#AppVersion}
AppVerName=QUADmapper {#AppVersion}
AppPublisher=Fiavaion
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\QUADmapper
DefaultGroupName=QUADmapper
DisableProgramGroupPage=yes
; Program Files by default; the user may choose a per-user install instead (no admin).
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#StageDir}\LICENSE.txt
SetupIconFile={#SourcePath}\..\src\app\QUADmapper.ico
UninstallDisplayIcon={app}\QUADmapper.exe
UninstallDisplayName=QUADmapper {#AppVersion}
; A running copy is asked to close before files are replaced.
CloseApplications=yes
OutputDir={#OutputDir}
OutputBaseFilename=QUADmapper-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

; FFmpeg's DLL names carry a version (avcodec-61.dll), so an upgrade to a newer FFmpeg
; would leave the old ones behind. They are removed first; [Files] installs the current set.
[InstallDelete]
Type: files; Name: "{app}\avcodec-*.dll"
Type: files; Name: "{app}\avdevice-*.dll"
Type: files; Name: "{app}\avfilter-*.dll"
Type: files; Name: "{app}\avformat-*.dll"
Type: files; Name: "{app}\avutil-*.dll"
Type: files; Name: "{app}\swresample-*.dll"
Type: files; Name: "{app}\swscale-*.dll"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\QUADmapper"; Filename: "{app}\QUADmapper.exe"; WorkingDir: "{app}"
Name: "{group}\Third-party notices"; Filename: "{app}\THIRD-PARTY-NOTICES.txt"
Name: "{group}\{cm:UninstallProgram,QUADmapper}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\QUADmapper"; Filename: "{app}\QUADmapper.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\QUADmapper.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,QUADmapper}"; Flags: nowait postinstall skipifsilent
