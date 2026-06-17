; Inno Setup script for YGO: Nova.
;
; Build with:  tools\build_installer.ps1   (stages the bundle, then runs ISCC)
; or directly: ISCC /DMyAppVersion=1.0.0 installer\YGONova.iss
;
; Per-user install (PrivilegesRequired=lowest) so the install directory is
; writable - the app caches on-demand card art into assets\cards\ next to the
; exe, which a Program Files install would block.

#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif
#define MyAppName "YGO Nova"
#define MyAppExeName "YGONova.exe"
#define MyAppPublisher "YGO Nova Project"
; Staged payload produced by package_release.ps1.
#define StageDir "..\dist\YGONova-v" + MyAppVersion

[Setup]
; A stable AppId keeps upgrades/uninstall consistent across versions.
AppId={{8F2C4B91-7A3E-4D6C-9B1F-YGONOVA00001}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#MyAppName} {#MyAppVersion}
UninstallDisplayIcon={app}\{#MyAppExeName}
OutputDir=..\dist
OutputBaseFilename=YGONova-v{#MyAppVersion}-Setup
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
LicenseFile={#StageDir}\THIRD_PARTY_NOTICES.md
; Branded installer + Add/Remove Programs icon (present once make_icon.py runs).
#if FileExists("YGONova.ico")
SetupIconFile=YGONova.ico
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
; Whole staged bundle (exe + assets without card images + tools + docs).
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\{#MyAppName}";            Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}";  Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}";      Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[Code]
// Warn (don't block) if the Visual C++ runtime is missing - the static-md
// build needs it. We can't easily detect every install, so we probe for the
// CRT DLL on the system path.
function InitializeSetup(): Boolean;
var
  Dummy: String;
begin
  Result := True;
  if not FileExists(ExpandConstant('{sys}\vcruntime140.dll')) then
  begin
    if MsgBox('YGO Nova needs the Microsoft Visual C++ 2015-2022 Redistributable (x64),'
      + #13#10 + 'which does not appear to be installed.' + #13#10#13#10
      + 'Install YGO Nova anyway? (Get the runtime from https://aka.ms/vs/17/release/vc_redist.x64.exe)',
      mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
    Dummy := '';
  end;
end;
