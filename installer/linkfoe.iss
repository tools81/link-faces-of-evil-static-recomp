; Inno Setup script: single-file installer for the Link: The Faces of Evil recomp.
; Built by tools\package.ps1, which passes /DAppVersion=x.y.z and /DStageDir=<files>.
;
; The game is portable: it keeps its saves, settings and linkfoe.ini in the
; folder it is installed to, so the default location is a user-writable
; folder and the installer needs no administrator rights.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\dist\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#define AppName "Link - The Faces of Evil (Recomp)"

[Setup]
AppId={{7C1B6E0A-4F53-4C6B-9A0E-5D1F0B6E2A11}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
DefaultDirName={sd}\Games\Link - The Faces of Evil
DefaultGroupName=Link - The Faces of Evil
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutputDir}
OutputBaseFilename=LinkFacesOfEvil-v{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
UninstallDisplayIcon={app}\linkfoe_hd.exe

[Tasks]
Name: "desktopicon"; Description: "Create desktop shortcuts"; GroupDescription: "Shortcuts:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
Name: "{app}\saves"

[Icons]
Name: "{group}\Link - The Faces of Evil (HD)"; Filename: "{app}\linkfoe_hd.exe"; Parameters: "--portable"; WorkingDir: "{app}"
Name: "{group}\Link - The Faces of Evil (Original)"; Filename: "{app}\linkfoe.exe"; Parameters: "--portable"; WorkingDir: "{app}"
Name: "{group}\Read me"; Filename: "{app}\README.md"
Name: "{group}\Uninstall"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Link - The Faces of Evil (HD)"; Filename: "{app}\linkfoe_hd.exe"; Parameters: "--portable"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\Link - The Faces of Evil (Original)"; Filename: "{app}\linkfoe.exe"; Parameters: "--portable"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\linkfoe_hd.exe"; Parameters: "--portable"; WorkingDir: "{app}"; Description: "Play now (HD edition)"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; program-made files; saves\ is left in place
Type: filesandordirs; Name: "{app}\update"

[Code]
var
  RomPage: TInputFileWizardPage;

{ the .bin a .cue refers to (or the file itself when it is not a .cue) }
function DiscBinPath(const Path: String): String;
var
  Lines: TArrayOfString;
  I, Q1, Q2: Integer;
  S: String;
begin
  Result := Path;
  if CompareText(ExtractFileExt(Path), '.cue') <> 0 then Exit;
  if not LoadStringsFromFile(Path, Lines) then Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do begin
    S := Trim(Lines[I]);
    if CompareText(Copy(S, 1, 4), 'FILE') = 0 then begin
      Q1 := Pos('"', S);
      S := Copy(S, Q1 + 1, Length(S));
      Q2 := Pos('"', S);
      if (Q1 > 0) and (Q2 > 0) then
        Result := AddBackslash(ExtractFilePath(Path)) + Copy(S, 1, Q2 - 1);
      Exit;
    end;
  end;
end;

{ a raw 2352-byte-sector CD-i image has "CD-I " in the disc label (sector 16) }
function IsCdiImage(const Path: String): Boolean;
var
  Stream: TFileStream;
  Buf: AnsiString;
begin
  Result := False;
  if not FileExists(Path) then Exit;
  try
    Stream := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone);
    try
      if Stream.Size < 16 * 2352 + 30 then Exit;
      Stream.Seek(16 * 2352 + 25, soFromBeginning);
      SetLength(Buf, 5);
      Stream.ReadBuffer(Buf, 5);
      Result := Buf = 'CD-I ';
    finally
      Stream.Free;
    end;
  except
    Result := False;
  end;
end;

procedure InitializeWizard;
begin
  RomPage := CreateInputFilePage(wpSelectDir,
    'Game disc image',
    'Where is your copy of Link: The Faces of Evil?',
    'The game runs from your own CD-i disc image, which is not included. ' +
    'Select its .cue file (or the .bin file). It stays where it is; ' +
    'the location is saved in linkfoe.ini and can be changed later.');
  RomPage.Add('Disc image (.cue or .bin):',
    'CD-i disc image (*.cue;*.bin)|*.cue;*.bin|All files|*.*', '.cue');
  { unattended installs: setup.exe /VERYSILENT /ROM="D:\path\game.cue" }
  RomPage.Values[0] := ExpandConstant('{param:ROM|}');
end;

{ reinstall/upgrade: offer the disc image chosen last time }
procedure CurPageChanged(CurPageID: Integer);
var
  Prev: String;
begin
  if (CurPageID <> RomPage.ID) or (RomPage.Values[0] <> '') then Exit;
  Prev := GetIniString('game', 'rom', '', ExpandConstant('{app}\linkfoe.ini'));
  if (Prev <> '') and (Pos(':', Prev) = 0) then
    Prev := ExpandConstant('{app}\') + Prev;
  RomPage.Values[0] := Prev;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Path: String;
begin
  Result := True;
  if CurPageID <> RomPage.ID then Exit;
  Path := Trim(RomPage.Values[0]);
  if Path = '' then begin
    MsgBox('Please select the game''s disc image (.cue or .bin).', mbError, MB_OK);
    Result := False;
  end else if not FileExists(Path) then begin
    MsgBox('The file was not found:' #13#10 + Path, mbError, MB_OK);
    Result := False;
  end else if not IsCdiImage(DiscBinPath(Path)) then begin
    Result := MsgBox('This does not look like a raw CD-i disc image:' #13#10 + DiscBinPath(Path) + #13#10#13#10 +
      'Use it anyway?', mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    SetIniString('game', 'rom', Trim(RomPage.Values[0]), ExpandConstant('{app}\linkfoe.ini'));
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo,
  MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine +
    'Disc image:' + NewLine + Space + Trim(RomPage.Values[0]);
  if MemoTasksInfo <> '' then Result := Result + NewLine + NewLine + MemoTasksInfo;
end;
