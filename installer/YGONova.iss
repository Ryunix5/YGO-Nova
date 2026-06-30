; Inno Setup script for YGO: Nova.
;
; Build with:  tools\build_installer.ps1   (stages the bundle, then runs ISCC)
; or directly: ISCC /DMyAppVersion=1.0.0 installer\YGONova.iss
;
; Per-user install (PrivilegesRequired=lowest) so the install directory is
; writable - the app caches on-demand card art into assets\cards\ next to the
; exe, which a Program Files install would block.

#ifndef MyAppVersion
  #define MyAppVersion "1.0.3"
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
; Non-solid, normal compression: the payload is thousands of tiny Lua scripts,
; and solid/max made install crawl (the whole archive must be streamed to reach
; each small file). Non-solid trades a slightly larger download for a much
; faster install + extraction.
Compression=lzma2/normal
SolidCompression=no
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
; WorkingDir={app} so the app launches with its install folder as the working
; directory - the on-demand card-art cache and other assets are resolved
; relative to it.
Name: "{group}\{#MyAppName}";            Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"
Name: "{group}\Uninstall {#MyAppName}";  Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}";      Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

// The VC++ 2015-2022 runtime DLLs (msvcp140 / vcruntime140 / vcruntime140_1)
// are bundled app-local next to the exe by package_release.ps1, so no system
// redistributable is required and no pre-install check is needed.
