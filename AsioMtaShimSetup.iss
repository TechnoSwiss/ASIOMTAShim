; AsioMtaShimSetup.iss
; Inno Setup 6.3+ script for the ASIO MTA Shim (TASCAM US-16x08 / vMix shutdown fix).
; Put this file next to AsioMtaShim64.dll and AsioMtaShim.cpp, then compile with ISCC.exe.
;
; Installs to C:\Program Files\AsioMtaShim, registers the shim as an ASIO device
; named "US-16x08 (MTA Shim)", points it at the installed TASCAM driver, and adds
; an entry to Settings > Apps so it can be uninstalled cleanly.

#define ShimClsid "6F1D2C84-3A57-4E9B-B0C2-9D41E7A5F318"
#define AsioName  "US-16x08 (MTA Shim)"

[Setup]
AppId={{B3E7A2D1-5C84-4F6A-9E21-7D0C38F4A9B6}
AppName=ASIO MTA Shim for TASCAM US-16x08
AppVersion=1.0.0
UninstallDisplayName=ASIO MTA Shim for TASCAM US-16x08
DefaultDirName={autopf}\AsioMtaShim
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
OutputDir=.
OutputBaseFilename=AsioMtaShimSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "AsioMtaShim64.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "AsioMtaShim.cpp";   DestDir: "{app}"; Flags: ignoreversion

[Registry]
; COM registration for the shim
Root: HKLM64; Subkey: "SOFTWARE\Classes\CLSID\{{{#ShimClsid}}"; ValueType: string; ValueName: ""; ValueData: "ASIO MTA Shim"; Flags: uninsdeletekey
Root: HKLM64; Subkey: "SOFTWARE\Classes\CLSID\{{{#ShimClsid}}\InprocServer32"; ValueType: string; ValueName: ""; ValueData: "{app}\AsioMtaShim64.dll"
Root: HKLM64; Subkey: "SOFTWARE\Classes\CLSID\{{{#ShimClsid}}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"

; ASIO device entry (what vMix shows in its device list)
Root: HKLM64; Subkey: "SOFTWARE\ASIO\{#AsioName}"; ValueType: string; ValueName: "CLSID"; ValueData: "{{{#ShimClsid}}"; Flags: uninsdeletekey
Root: HKLM64; Subkey: "SOFTWARE\ASIO\{#AsioName}"; ValueType: string; ValueName: "Description"; ValueData: "{#AsioName}"

; Which real driver the shim loads (detected at install time)
Root: HKLM64; Subkey: "SOFTWARE\AsioMtaShim"; ValueType: string; ValueName: "TargetCLSID"; ValueData: "{code:GetTargetClsid}"; Flags: uninsdeletekey

[Code]
var
  TargetClsid: String;

{ Find the TASCAM ASIO driver's CLSID: first key under SOFTWARE\ASIO whose
  name contains "16x08" and isn't this shim. }
function FindTascamClsid(): String;
var
  Names: TArrayOfString;
  I: Integer;
  Clsid: String;
begin
  Result := '';
  if RegGetSubkeyNames(HKLM64, 'SOFTWARE\ASIO', Names) then
  begin
    for I := 0 to GetArrayLength(Names) - 1 do
    begin
      if (Pos('16X08', Uppercase(Names[I])) > 0) and (Pos('SHIM', Uppercase(Names[I])) = 0) then
      begin
        if RegQueryStringValue(HKLM64, 'SOFTWARE\ASIO\' + Names[I], 'CLSID', Clsid) and (Clsid <> '') then
        begin
          Result := Clsid;
          Exit;
        end;
      end;
    end;
  end;
end;

function InitializeSetup(): Boolean;
begin
  TargetClsid := FindTascamClsid();
  Result := TargetClsid <> '';
  if not Result then
    SuppressibleMsgBox('The TASCAM US-16x08 ASIO driver was not found.' + #13#10 +
      'Install the TASCAM driver first, then run this installer again.',
      mbCriticalError, MB_OK, IDOK);
end;

function GetTargetClsid(Param: String): String;
begin
  Result := TargetClsid;
end;
