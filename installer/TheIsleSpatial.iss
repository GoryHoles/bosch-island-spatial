#define MyAppName "Bosch Island - Spatial Audio"
#define MyAppPublisher "GoryHoles"
#define MyAppExeName "Mumble.exe"
#ifndef MyAppVersion
  #define MyAppVersion Trim(FileRead(FileOpen("..\VERSION")))
#endif
#define MyAppVersionSafe StringChange(StringChange(StringChange(StringChange(StringChange(MyAppVersion, " ", "_"), "/", "_"), "\\", "_"), ":", "_"), "+", "_")

#define PluginsDir "{userappdata}\Mumble\Mumble\Plugins"
#define LegacyServerHost "141.98.157.235"
#define LegacyServerPort "8890"
#define ActiveServer "server1"
#define Server1Host "141.98.157.235"
#define Server1Port "8890"
#define Server2Host "83.147.29.99"
#define Server2Port "8890"
#define ApiKey "change_me"

[Setup]
AppId={{C76A4F49-0F8B-4E8B-9D9E-2A58F9C3A9B5}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={#PluginsDir}
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=BoschIslandSpatialSetup-{#MyAppVersionSafe}
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64compatible
CreateUninstallRegKey=yes
Uninstallable=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "..\build\theisle_spatial.dll"; DestDir: "{userappdata}\Mumble\Mumble\Plugins"; Flags: ignoreversion

[Run]
Filename: "{commonpf}\Mumble\Mumble.exe"; Description: "Open Mumble (Plugins settings)"; Flags: postinstall skipifsilent unchecked; Check: MumbleExe64Exists
Filename: "{commonpf32}\Mumble\Mumble.exe"; Description: "Open Mumble (Plugins settings)"; Flags: postinstall skipifsilent unchecked; Check: MumbleExe32Exists

[UninstallDelete]
Type: files; Name: "{userappdata}\Mumble\Mumble\Plugins\theisle_spatial.dll"

[Code]
function PluginDir: string;
begin
  Result := ExpandConstant('{userappdata}\Mumble\Mumble\Plugins');
end;

function IniPath: string;
begin
  Result := PluginDir + '\theisle_spatial.ini';
end;

function MumbleExe64Exists: Boolean;
begin
  Result := FileExists(ExpandConstant('{commonpf}\Mumble\Mumble.exe'));
end;

function MumbleExe32Exists: Boolean;
begin
  Result := FileExists(ExpandConstant('{commonpf32}\Mumble\Mumble.exe'));
end;

procedure WriteIniFile;
var
  P, Lines: string;
begin
  Lines :=
    'server_host={#LegacyServerHost}' + #13#10 +
    'server_port={#LegacyServerPort}' + #13#10 +
    'active_server={#ActiveServer}'   + #13#10 +
    'server1_host={#Server1Host}'     + #13#10 +
    'server1_port={#Server1Port}'     + #13#10 +
    'server2_host={#Server2Host}'     + #13#10 +
    'server2_port={#Server2Port}'     + #13#10 +
    'api_key={#ApiKey}'               + #13#10 +
    'enabled=1'                       + #13#10 +
    'debug_log=0'                     + #13#10 +
    'smoothing=0.80'                  + #13#10 +
    'pan_smoothing=0.95'              + #13#10;

  P := IniPath;
  if FileExists(P) then DeleteFile(P);
  SaveStringToFile(P, Lines, False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Dir, DllPath, Msg: string;
  DllOk, IniOk: Boolean;
begin
  if CurStep <> ssPostInstall then exit;

  Dir := PluginDir;
  ForceDirectories(Dir);
  WriteIniFile;

  DllPath := Dir + '\theisle_spatial.dll';
  DllOk   := FileExists(DllPath);
  IniOk   := FileExists(IniPath);

  if not DllOk or not IniOk then begin
    Msg := 'Warning: expected files missing in:' + #13#10 + Dir + #13#10 + #13#10;
    if not DllOk then Msg := Msg + '  theisle_spatial.dll  NOT found' + #13#10;
    if not IniOk then Msg := Msg + '  theisle_spatial.ini  NOT found' + #13#10;
    Msg := Msg + #13#10 + 'Check that Mumble uses: ' + Dir;
    MsgBox(Msg, mbError, MB_OK);
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
    WizardForm.FinishedLabel.Caption :=
      WizardForm.FinishedLabel.Caption + #13#10 + #13#10 +
      'Installed to:' + #13#10 +
      '  ' + PluginDir + #13#10 + #13#10 +
      'Next steps:' + #13#10 +
      '1. Open Mumble -> Settings -> Plugins.' + #13#10 +
      '2. Enable "Bosch Island - Spatial Audio".' + #13#10 +
      '3. Switch servers by editing active_server in theisle_spatial.ini.' + #13#10 + #13#10 +
      'Developed by GoryHoles';
end;