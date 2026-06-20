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
; Stable GUID keeps upgrade/uninstall registration consistent across releases.
; MyAppId doubles the leading brace ({{) so AppId={#MyAppId} in [Setup] resolves to
; a single-brace GUID. MyAppGuid is the raw GUID reused in [Code] to locate the
; previously-installed version's uninstaller (HKLM ...\Uninstall\{GUID}_is1) so a
; new installer can cleanly remove the old version before installing.
#define MyAppGuid       "E4B2C9A1-7F3D-4E8B-9C2A-1D5E6F708192"
#define MyAppId         "{{" + MyAppGuid + "}"

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
; Create the preset directory the user chose. uninsneveruninstall: Inno must NOT
; touch this folder on uninstall - removal is OPT-IN, handled entirely in [Code]
; (see InitializeUninstall / CurUninstallStepChanged), gated on the uninstall-time
; "also remove my presets" checkbox. Without this flag Inno would auto-delete even
; when the user leaves the box unticked.
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
; Factory presets - inflated onto the machine on install.
;   onlyifdoesntexist: never overwrite a user's modified copy on reinstall/upgrade.
;   skipifsourcedoesntexist: don't fail the compile when the staging folder is empty
;     (lets the installer build before any factory presets exist).
;   uninsneveruninstall: Inno must NOT auto-delete presets. Removal is OPT-IN at
;     uninstall time: InitializeUninstall shows an "also remove my presets" checkbox,
;     and CurUninstallStepChanged DelTree's the WHOLE preset folder (factory + anything
;     the user created) only if it was ticked. The preset folder path is recovered at
;     uninstall from the [Registry] PresetsDir value written below. Leaving the box
;     unticked keeps every preset, which is why Inno's own tracking stays disabled here.
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

[Registry]
; Record the chosen preset folder so the uninstaller (which has no wizard pages) can
; offer to delete it. HKA = HKLM here (PrivilegesRequired=admin). uninsdeletekey cleans
; the key on uninstall; InitializeUninstall reads it BEFORE that happens.
Root: HKA; Subkey: "Software\{#MyAppName}"; ValueType: string; ValueName: "PresetsDir"; ValueData: "{code:GetPresetsDir}"; Flags: uninsdeletekey

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
  (* Uninstall-time state: whether the user asked to also wipe the preset folder,
     and the folder path recovered from the registry (the uninstaller has no wizard). *)
  UninstallRemovePresets: Boolean;
  UninstallPresetsDir: string;

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

(* ================= UPGRADE: remove the previous version =================== *)

(* Returns the previously-installed version's uninstaller command (or '' if none).
   Inno records every install under HKLM/HKCU ...\Uninstall\{AppId}_is1; because
   AppId is a stable GUID, every release shares this key, so a new installer can
   always find the old one. {#MyAppGuid} is emitted by ISPP as the raw GUID; the
   surrounding '{' .. '}' literals rebuild the single-brace key Inno actually wrote. *)
function PreviousUninstaller(): string;
var
  Subkey, S: string;
begin
  Result := '';
  Subkey := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{' + '{#MyAppGuid}' + '}_is1';
  if RegQueryStringValue(HKLM, Subkey, 'UninstallString', S) then
    Result := S
  else if RegQueryStringValue(HKCU, Subkey, 'UninstallString', S) then
    Result := S;
end;

(* Inno calls this after the wizard, just before files are copied. We run the old
   version's uninstaller SILENTLY so shipping a new installer is a clean replace
   (no stale leftovers from renamed/removed files) instead of an overlay. /VERYSILENT
   suppresses the old uninstaller's preset-removal dialog AND keeps its "remove
   presets" checkbox unticked (see the UninstallSilent guard in InitializeUninstall),
   so an UPDATE never deletes the user's presets. Combined with onlyifdoesntexist on
   the [Files] presets, every factory/user preset survives an update untouched.
   Non-fatal: if the old uninstaller is missing or won't launch, Inno just installs
   over the top. ewWaitUntilTerminated blocks until the uninstall finishes so the
   removal and the fresh install never race. *)
function PrepareToInstall(var NeedsRestart: Boolean): string;
var
  Uninstaller: string;
  ResultCode: Integer;
begin
  Result := '';
  Uninstaller := Trim(PreviousUninstaller());
  if Uninstaller = '' then
    Exit;   (* fresh install - nothing to remove *)
  Uninstaller := RemoveQuotes(Uninstaller);
  if FileExists(Uninstaller) then
    Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCANCEL',
         '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

(* =============================== UNINSTALL ================================= *)

(* Runs at the very start of uninstall - before [Registry]/[UninstallDelete] are
   processed, so the PresetsDir key is still readable. Recovers the preset folder
   and, if it exists, shows an opt-in checkbox to also delete the WHOLE folder
   (factory presets AND anything the user created). Default: unticked = keep all.
   Returning False here cancels the uninstall entirely (user backed out). *)
function InitializeUninstall(): Boolean;
var
  Form: TSetupForm;
  Lbl, Lbl2: TNewStaticText;
  Chk: TNewCheckBox;
  OKBtn, CancelBtn: TNewButton;
begin
  Result := True;
  UninstallRemovePresets := False;
  UninstallPresetsDir := '';

  if not RegQueryStringValue(HKA, 'Software\{#MyAppName}', 'PresetsDir', UninstallPresetsDir) then
    UninstallPresetsDir := '';
  UninstallPresetsDir := Trim(UninstallPresetsDir);

  (* Nothing to offer if we don't know the folder or it's already gone. *)
  if (UninstallPresetsDir = '') or (not DirExists(UninstallPresetsDir)) then
    Exit;

  (* Silent uninstall = driven by a NEWER installer's upgrade step (PrepareToInstall
     runs us with /VERYSILENT) or any command-line /SILENT. In that case never show
     the prompt and leave UninstallRemovePresets = False, so an UPDATE always keeps
     the user's presets. Manual uninstalls (no /SILENT) still get the opt-in dialog. *)
  if UninstallSilent then
    Exit;

  (* (width, height, autosizeWidth=False, fixedHeight=True). No WizardForm exists in
     the uninstaller, so we let it auto-center on screen rather than on a wizard. *)
  Form := CreateCustomForm(ScaleX(470), ScaleY(210), False, True);
  try
    Form.Caption := 'Uninstall {#MyAppName}';

    Lbl := TNewStaticText.Create(Form);
    Lbl.Parent := Form;
    Lbl.Left := ScaleX(18);
    Lbl.Top := ScaleY(18);
    Lbl.Width := Form.ClientWidth - ScaleX(36);
    Lbl.Caption := '{#MyAppName} is about to be uninstalled.';

    Chk := TNewCheckBox.Create(Form);
    Chk.Parent := Form;
    Chk.Left := ScaleX(18);
    Chk.Top := ScaleY(48);
    Chk.Width := Form.ClientWidth - ScaleX(36);
    Chk.Height := ScaleY(20);
    Chk.Checked := False;
    Chk.Caption := 'Also remove my Space Dust presets folder from this computer';

    (* TNewCheckBox has no WordWrap; the wrapping detail/warning lives in its own
       static text indented under the checkbox. *)
    Lbl2 := TNewStaticText.Create(Form);
    Lbl2.Parent := Form;
    Lbl2.Left := ScaleX(36);
    Lbl2.Top := ScaleY(72);
    Lbl2.Width := Form.ClientWidth - ScaleX(54);
    Lbl2.Height := ScaleY(88);
    Lbl2.AutoSize := False;
    Lbl2.WordWrap := True;
    Lbl2.Caption :=
      UninstallPresetsDir + #13#10 + #13#10 +
      'WARNING: this permanently deletes the entire folder and EVERY preset in it, ' +
      'including any presets you created yourself. Leave the box unchecked to keep them.';

    OKBtn := TNewButton.Create(Form);
    OKBtn.Parent := Form;
    OKBtn.Caption := 'Uninstall';
    OKBtn.ModalResult := mrOk;
    OKBtn.Default := True;
    OKBtn.Width := ScaleX(95);
    OKBtn.Height := ScaleY(28);
    OKBtn.Top := Form.ClientHeight - ScaleY(28 + 16);
    OKBtn.Left := Form.ClientWidth - ScaleX(95 + 12 + 95 + 18);

    CancelBtn := TNewButton.Create(Form);
    CancelBtn.Parent := Form;
    CancelBtn.Caption := 'Cancel';
    CancelBtn.ModalResult := mrCancel;
    CancelBtn.Cancel := True;
    CancelBtn.Width := ScaleX(95);
    CancelBtn.Height := ScaleY(28);
    CancelBtn.Top := Form.ClientHeight - ScaleY(28 + 16);
    CancelBtn.Left := Form.ClientWidth - ScaleX(95 + 18);

    if Form.ShowModal() = mrOk then
      UninstallRemovePresets := Chk.Checked
    else
      Result := False;   (* user cancelled the whole uninstall *)
  finally
    Form.Free();
  end;
end;

(* After Inno's own uninstall work has finished, honour the checkbox: nuke the
   entire preset folder (files + subfolders + the folder itself) if it was ticked. *)
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    if UninstallRemovePresets and (UninstallPresetsDir <> '') and DirExists(UninstallPresetsDir) then
      DelTree(UninstallPresetsDir, True, True, True);
end;
