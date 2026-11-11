; MobCam installer (Inno Setup 6). Built by scripts\release-pc.ps1:
;   ISCC /DAppVersion=1.0.0 /DSourceDir=<publish folder> /O<output folder> mobcam.iss
;
; Installs the app and registers the virtual camera (mobcam-source.dll) for Windows Frame
; Server. The DLL must live where the LOCAL SERVICE account can read it, so the install folder
; is fixed to Program Files.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\publish"
#endif
; CLSID of the virtual camera media source (pc/common/mobcam_ids.h). "{{" escapes "{".
#define SourceClsid "{{6D3A9C1E-4B7F-4E2A-9F1C-2B8E5D7A4C31}"

[Setup]
AppId={{8E3B6F2A-5C1D-4A7E-9B2F-3D4C5E6F7A81}
AppName=MobCam
AppVersion={#AppVersion}
AppVerName=MobCam {#AppVersion}
AppPublisher=MobCam
DefaultDirName={autopf}\MobCam
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; MFCreateVirtualCamera needs Windows 11
MinVersion=10.0.22000
OutputBaseFilename=MobCam-Setup-{#AppVersion}
SetupIconFile=..\gui\mobcam.ico
UninstallDisplayIcon={app}\MobCam.exe
UninstallDisplayName=MobCam
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: desktopicon; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: startup; Description: "Start MobCam with &Windows (in the tray)"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
; Log folder for the camera DLL, which runs as LOCAL SERVICE
Name: "{commonappdata}\MobCam"; Permissions: everyone-modify

[Registry]
Root: HKLM; Subkey: "Software\Classes\CLSID\{#SourceClsid}"; ValueType: string; ValueData: "MobCam"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\CLSID\{#SourceClsid}\InprocServer32"; ValueType: string; ValueData: "{app}\mobcam-source.dll"
Root: HKLM; Subkey: "Software\Classes\CLSID\{#SourceClsid}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Both"
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "MobCam"; ValueData: """{app}\MobCam.exe"" --tray"; Tasks: startup; Flags: uninsdeletevalue

[Icons]
Name: "{autoprograms}\MobCam"; Filename: "{app}\MobCam.exe"
Name: "{autodesktop}\MobCam"; Filename: "{app}\MobCam.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\MobCam.exe"; Description: "Launch MobCam"; Flags: nowait postinstall skipifsilent

[Code]
// MobCam.exe may be running in the tray, and Frame Server keeps mobcam-source.dll loaded while
// an app uses the camera. Close both so files can be replaced; Windows restarts the camera
// services on demand.
procedure ReleaseFiles;
var
  rc: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM MobCam.exe', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM mobcam-cli.exe', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop FrameServerMonitor', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop FrameServer', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Sleep(1500);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  ReleaseFiles;
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    ReleaseFiles;
  if CurUninstallStep = usPostUninstall then
    // "Start with Windows" may also have been switched on from inside the app
    RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run', 'MobCam');
end;
