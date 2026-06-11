; =============================================================================
; Space Dust Synthesizer — Windows installer (Inno Setup 6)
; =============================================================================
; Staging layout (next to this .iss file):
;   Files\VST3\Space Dust.vst3\   ← full VST3 bundle (folder with .vst3 extension)
;   Files\Standalone\Space Dust.exe ← standalone desktop app
;   Files\Presets\*.sdpreset      ← factory presets
;   License-GPLv3.txt             ← GPLv3 text for the license wizard page
;   THIRD-PARTY-NOTICES.txt       ← JUCE (GPL) + Glitch Goblin font (OFL) notices
;   Support\README-Presets.txt    ← copied into the preset folder as README.txt
;
; Components: the user ticks VST3 and/or Standalone (see [Components] + the
; SelectComponentsLabel2 message). Presets/config install if EITHER is chosen.
; The plug-in reads presetFolder from config.xml (see PresetManager.cpp).
; =============================================================================

#define MyAppName       "Space Dust Synthesizer"
#define MyAppVersion    "1.0"
#define MyPublisher     "63C"
#define MyAppCopyright  "Copyright (c) 2026 63C"
; Stable AppId keeps upgrade/uninstall registration consistent across releases.
#define MyAppId         "{{E4B2C9A1-7F3D-4E8B-9C2A-1D5E6F708192}"

[Setup]
; --- Identity & version -------------------------------------------------------
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyPublisher}
AppCopyright={#MyAppCopyright}
VersionInfoVersion=1.0.0.0
VersionInfoCompany={#MyPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion=1.0.0.0
VersionInfoDescription={#MyAppName} Setup
VersionInfoCopyright={#MyAppCopyright}
DefaultDirName={autopf64}\{#MyAppName}
; We only use {app} for Inno's uninstall records; VST3 path is chosen separately.
DisableDirPage=yes
DisableProgramGroupPage=yes
; --- Modern look (Inno Setup 6+) ------------------------------------------------
WizardStyle=modern
WizardSizePercent=120,150
; --- 64-bit VST3 --------------------------------------------------------------
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; --- Install scope -----------------------------------------------------------
; Admin-required, no per-user override. The "install for me only" option was
; removed because the resulting per-user VST3 path (%LOCALAPPDATA%\Programs\Common\VST3)
; isn't scanned by older DAWs (FL Studio 20 and earlier, older Cubase/Studio One),
; so users on those hosts saw a successful install but no plugin. Forcing admin
; means the VST3 lands in C:\Program Files\Common Files\VST3 — the universal path
; every Windows DAW scans by default.
PrivilegesRequired=admin
; --- Wizard copy --------------------------------------------------------------
LicenseFile=License-GPLv3.txt
InfoBeforeFile=
InfoAfterFile=
OutputDir=Output
OutputBaseFilename=SpaceDust-Synthesizer-{#MyAppVersion}-Setup
SetupIconFile=AppIcon.ico
UninstallDisplayIcon={app}\AppIcon.ico
Compression=lzma2/max
SolidCompression=yes
LZMAUseSeparateProcess=yes
; --- Code signing -------------------------------------------------------------
; When compiled with /DSIGN (see package-installer.ps1 -Sign), Inno signs the
; Setup .exe AND the embedded uninstaller (unins000.exe) on the fly using the
; "spacedust" sign tool, whose command is supplied on the ISCC command line via
; /Sspacedust=...  SignedUninstaller defaults to "yes if a SignTool is set", so
; merely defining this directive is what gets the uninstaller signed - otherwise
; unins000.exe is extracted unsigned at install time and shows "unknown publisher".
#ifdef SIGN
SignTool=spacedust
#endif
; --- Shell --------------------------------------------------------------------
MinVersion=10.0
CloseApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";       Description: "Both (VST3 plug-in + Standalone app)"
Name: "vst3";       Description: "VST3 plug-in only"
Name: "standalone"; Description: "Standalone app only"
Name: "custom";     Description: "Custom"; Flags: iscustom

[Components]
Name: "vst3";       Description: "VST3 plug-in - for use inside a DAW (Ableton, FL Studio, Cubase, ...)"; Types: full vst3 custom
Name: "standalone"; Description: "Standalone app - a desktop synthesizer, no DAW required";              Types: full standalone custom

[Messages]
; The instructional blurb shown above the component checkboxes.
SelectComponentsLabel2=Choose what to install. To use Space Dust inside a Digital Audio Workstation (DAW) such as Ableton, FL Studio, or Cubase, select the VST3 plug-in. If you just want a fun standalone desktop synthesizer, select the Standalone app. Tick both boxes to install both.

[Tasks]
; Desktop shortcut for the Standalone app (only offered when Standalone is selected).
Name: "desktopicon"; Description: "Create a desktop shortcut for the Standalone app"; Components: standalone; Flags: unchecked

[Dirs]
; Create the preset directory the user chose (user data — keep on uninstall).
Name: "{code:GetPresetsDir}"; Flags: uninsneveruninstall

[Files]
; Entire VST3 bundle: recursive copy preserving inner layout. (VST3 component only.)
Source: "Files\VST3\Space Dust.vst3\*"; DestDir: "{code:GetVST3Dir}\Space Dust.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: vst3
; Standalone desktop app -> {app} (Program Files\Space Dust Synthesizer). (Standalone component only.)
Source: "Files\Standalone\Space Dust.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: standalone
; Third-party notices (JUCE GPL + Glitch Goblin OFL font) — always installed, satisfies OFL.
Source: "THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion
; App icon — installed so UninstallDisplayIcon has a stable target regardless of components.
Source: "AppIcon.ico"; DestDir: "{app}"; Flags: ignoreversion
; Factory presets — copied into the user's chosen preset folder.
;   onlyifdoesntexist: never overwrite a user's modified copy on reinstall/upgrade.
;   skipifsourcedoesntexist: don't fail the compile when the staging folder is empty
;     (lets the installer build before any factory presets exist).
;   uninsneveruninstall: CRITICAL — do NOT delete presets on uninstall. The preset
;     folder is the user's sound library (often inside Documents) and is frequently the
;     SAME folder the user curates. Without this flag Inno records each installed preset
;     and removes it on uninstall, gutting the user's library. The [Dirs] entry alone
;     does not protect the files. Keep this flag.
; Extension must match PresetManager::presetExtension (".sdpreset"). DO NOT change
; without also updating PresetManager.h.
Source: "Files\Presets\*.sdpreset"; DestDir: "{code:GetPresetsDir}"; Flags: ignoreversion onlyifdoesntexist skipifsourcedoesntexist uninsneveruninstall; Components: vst3 standalone
; Documentation placed inside the preset folder.
Source: "Support\README-Presets.txt"; DestDir: "{code:GetPresetsDir}"; DestName: "README.txt"; Flags: ignoreversion confirmoverwrite; Components: vst3 standalone

[Icons]
; Standalone app shortcuts (only created when the Standalone component is selected).
Name: "{autoprograms}\Space Dust"; Filename: "{app}\Space Dust.exe"; Components: standalone
Name: "{autodesktop}\Space Dust";  Filename: "{app}\Space Dust.exe"; Components: standalone; Tasks: desktopicon
; NOTE: We deliberately do NOT create a Start Menu "Uninstall Space Dust" shortcut.
; Windows 10/11 filter uninstall-target shortcuts out of the Start Menu "All apps"
; list (by design — they route uninstalls through Settings > Apps), so such a shortcut
; is created as a file but never shown. The discoverable, working uninstall path is the
; auto-registered entry in Settings > Apps / Programs and Features ("Space Dust
; Synthesizer version 1.0"), which Inno creates automatically.

[Run]
; Silent install — no post-install programs required for a VST3.

[UninstallDelete]
; config.xml is written from script — remove on uninstall.
; {autoappdata} matches whichever mode was used at install time.
Type: files; Name: "{autoappdata}\Space Dust\config.xml"
; Remove empty config directory if nothing else remains (ignore errors).
Type: dirifempty; Name: "{autoappdata}\Space Dust"

[Code]
var
  VST3DirPage: TInputDirWizardPage;
  PresetsDirPage: TInputDirWizardPage;
  PresetsAllUsersCheck: TNewCheckBox;

(* Recursively create a directory tree (single-level CreateDir is not enough). *)
function ForceDirectories(Dir: string): Boolean;
var
  Parent: string;
begin
  Result := True;
  if Dir = '' then Exit;
  if DirExists(Dir) then Exit;
  Parent := ExtractFileDir(Dir);
  if (CompareText(Parent, Dir) <> 0) and (Parent <> '') then
    Result := ForceDirectories(Parent);
  if not Result then Exit;
  Result := CreateDir(Dir);
end;

(* Escape attribute text for a minimal XML file (preset paths are unlikely to contain markup). *)
function XmlAttrEscape(const S: string): string;
var
  R: string;
begin
  R := S;
  StringChangeEx(R, '&', '&amp;', True);
  StringChangeEx(R, '"', '&quot;', True);
  StringChangeEx(R, '<', '&lt;', True);
  Result := R;
end;

(* Wizard page: VST3 destination (standard default = Common Files x64\VST3). *)
procedure InitVST3Page;
begin
  VST3DirPage := CreateInputDirPage(wpSelectComponents,
    'VST3 Plug-in Location',
    'Where should the installer place the Space Dust VST3 bundle?',
    'Select the folder that should contain Space Dust.vst3 (your DAW''s VST3 scan path).',
    False, '');
  VST3DirPage.Add('VST3 folder:');
  (* {autocf64} resolves to {commoncf64} in all-users mode, {localappdata}\Programs\Common
     in per-user mode. The latter is the standard per-user VST3 location DAWs scan. *)
  VST3DirPage.Values[0] := ExpandConstant('{autocf64}\VST3');
end;

(* Wizard page: presets + “all users” checkbox (default under Documents). *)
procedure InitPresetsPage;
var
  EditTop: Integer;
begin
  PresetsDirPage := CreateInputDirPage(VST3DirPage.ID,
    'Space Dust Presets Location',
    'Choose where preset files will be stored on disk.',
    'Factory and user presets are loaded from this folder. You can change it later by reinstalling or editing config.xml.',
    False, '');
  PresetsDirPage.Add('Presets folder:');
  PresetsDirPage.Values[0] := ExpandConstant('{userdocs}\Space Dust\Presets');

  PresetsAllUsersCheck := TNewCheckBox.Create(PresetsDirPage);
  PresetsAllUsersCheck.Parent := PresetsDirPage.Surface;
  PresetsAllUsersCheck.Caption :=
    'Use this folder and make it the default for all users of this PC';
  PresetsAllUsersCheck.Checked := False;

  (* Position the checkbox under the directory edit supplied by the dir page.
     Edits[0] always exists because InitPresetsPage always calls .Add() once. *)
  EditTop := PresetsDirPage.Edits[0].Top;
  PresetsAllUsersCheck.Top := EditTop + ScaleY(44);
  PresetsAllUsersCheck.Left := PresetsDirPage.Edits[0].Left;
  PresetsAllUsersCheck.Width := PresetsDirPage.SurfaceWidth - PresetsDirPage.Edits[0].Left * 2;
  PresetsAllUsersCheck.Height := ScaleY(36);
  PresetsAllUsersCheck.Anchors := [akLeft, akRight, akTop];
end;

procedure InitializeWizard;
begin
  InitVST3Page;
  InitPresetsPage;
end;

(* Inno calls these when expanding {code:...} constants in [Files] / [Dirs]. *)
function GetVST3Dir(Param: string): string;
begin
  Result := Trim(VST3DirPage.Values[0]);
end;

function GetPresetsDir(Param: string): string;
begin
  Result := Trim(PresetsDirPage.Values[0]);
end;

(* Skip the VST3 folder page when the user did not tick the VST3 component. *)
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = VST3DirPage.ID then
    Result := not WizardIsComponentSelected('vst3');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  D: string;
begin
  Result := True;

  if CurPageID = VST3DirPage.ID then
  begin
    D := Trim(VST3DirPage.Values[0]);
    if D = '' then
    begin
      MsgBox('Please choose a folder for the VST3 plug-in.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
  end;

  if CurPageID = PresetsDirPage.ID then
  begin
    D := Trim(PresetsDirPage.Values[0]);
    if D = '' then
    begin
      MsgBox('Please choose a folder for Space Dust presets.', mbError, MB_OK);
      Result := False;
      Exit;
    end;
  end;
end;

(* Appended when “all users” checkbox is checked — runs after README.txt is installed.
   Uses SaveStringsToFile (line-array, native String type) with Append=True so we
   don't have to deal with AnsiString/UnicodeString conversion of LoadStringFromFile. *)
procedure AppendPresetsReadmeAllUsersNote(const PresetFolder: string);
var
  ReadmePath: string;
  Lines: TArrayOfString;
begin
  ReadmePath := PresetFolder + '\README.txt';
  if not FileExists(ReadmePath) then
    Exit;
  SetArrayLength(Lines, 4);
  Lines[0] := '';
  Lines[1] := '---';
  Lines[2] := 'Installer: This folder was set as the default preset location for all users of this PC.';
  Lines[3] := '';
  SaveStringsToFile(ReadmePath, Lines, True);
end;

(* After files are in place: write plug-in config matching JUCE XmlElement output. *)
procedure CurStepChanged(CurStep: TSetupStep);
var
  ConfigDir: string;
  ConfigPath: string;
  PresetPath: string;
  Xml: string;
begin
  if CurStep <> ssPostInstall then
    Exit;

  (* Folder selected on the “Space Dust Presets Location” page *)
  PresetPath := Trim(PresetsDirPage.Values[0]);

  (* {autoappdata} = %ProgramData%\Space Dust\config.xml in all-users mode,
     %APPDATA%\Space Dust\config.xml in per-user mode. The plugin's PresetManager
     looks in BOTH locations (user APPDATA preferred, ProgramData fallback) so
     either install mode is discoverable at runtime. *)
  ConfigPath := ExpandConstant('{autoappdata}\Space Dust\config.xml');

  ConfigDir := ExtractFileDir(ConfigPath);
  if not ForceDirectories(ConfigDir) then
    RaiseException('Could not create directory: ' + ConfigDir);

  (* Same shape as PresetManager::savePresetFolderConfig — root tag + attribute. *)
  Xml :=
    '<?xml version="1.0" encoding="UTF-8"?>' + #13#10 + #13#10 +
    '<SpaceDustConfig presetFolder="' + XmlAttrEscape(PresetPath) + '"/>' + #13#10;

  if not SaveStringToFile(ConfigPath, Xml, True) then
    RaiseException('Could not write configuration file: ' + ConfigPath);

  (* Optional note in README when the user chose a machine-wide default *)
  if PresetsAllUsersCheck.Checked then
    AppendPresetsReadmeAllUsersNote(PresetPath);
end;
