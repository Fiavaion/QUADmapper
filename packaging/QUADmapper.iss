; QUADmapper installer (ADR-057). Built by build-installer.ps1, which stages the files
; and passes AppVersion, StageDir and OutputDir:
;   ISCC /DAppVersion=1.0.0 /DStageDir=...\packaging\stage /DOutputDir=...\packaging\out QUADmapper.iss

#ifndef AppVersion
  #error AppVersion is not defined; run build-installer.ps1
#endif
#ifndef StageDir
  #define StageDir AddBackslash(SourcePath) + "stage"
#endif
#ifndef OutputDir
  #define OutputDir AddBackslash(SourcePath) + "out"
#endif

[Setup]
AppId={{96F59930-7E96-43D0-890F-BB004EE06DE4}
AppName=QUADmapper
AppVersion={#AppVersion}
AppVerName=QUADmapper {#AppVersion}
AppPublisher=Fiavaion
DefaultDirName={autopf}\QUADmapper
DefaultGroupName=QUADmapper
DisableProgramGroupPage=yes
; Program Files by default; the user (or /CURRENTUSER) may install per-user instead.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#StageDir}\app\THIRD_PARTY_NOTICES.txt
UninstallDisplayIcon={app}\QUADmapper.exe
UninstallDisplayName=QUADmapper {#AppVersion}
OutputDir={#OutputDir}
OutputBaseFilename=QUADmapper-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
LZMAUseSeparateProcess=yes
LZMANumBlockThreads=4
WizardStyle=modern

[Types]
Name: "full"; Description: "Full installation (with AI features)"
Name: "compact"; Description: "QUADmapper only"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "main"; Description: "QUADmapper"; Types: full compact custom; Flags: fixed
Name: "ai"; Description: "AI features (Python runtime, PyTorch with CUDA, SAM 2 and Depth Anything models)"; Types: full

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\app\*"; DestDir: "{app}"; Components: main; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\ai\*"; DestDir: "{app}\ai"; Components: ai; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: ".qm-stage-stamp"

[Icons]
; The app finds resources\ relative to its working folder, so every shortcut starts in {app}.
Name: "{group}\QUADmapper"; Filename: "{app}\QUADmapper.exe"; WorkingDir: "{app}"
Name: "{group}\Third-party notices"; Filename: "{app}\THIRD_PARTY_NOTICES.txt"
Name: "{group}\{cm:UninstallProgram,QUADmapper}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\QUADmapper"; Filename: "{app}\QUADmapper.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\QUADmapper.exe"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,QUADmapper}"; Flags: nowait postinstall skipifsilent

[InstallDelete]
; Unticking AI features on a re-install removes the old runtime.
Type: filesandordirs; Name: "{app}\ai"; Components: not ai

[UninstallDelete]
; Files the app or the sidecar may create at run time.
Type: filesandordirs; Name: "{app}\ai"
Type: files; Name: "{app}\imgui.ini"
