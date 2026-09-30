; Wallpaper Plus installer (Inno Setup 6). Built by tools\make-release.sh and the GitHub workflow:
;   ISCC /DAppVersion=1.2.3 installer\WallpaperPlus.iss
; Optional: /DBuildDir=<folder with the built exes and ui\> (default ..\build),
;           /DOutputDir=<where the setup exe goes> (default ..\release).
;
; Installs for all users into Program Files, so it needs admin. Settings, library and thumbnails
; live in the user's %APPDATA%\WallpaperPlus (see src\common\paths.cpp), so upgrades keep them.

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif
#ifndef BuildDir
  #define BuildDir "..\build"
#endif
#ifndef OutputDir
  #define OutputDir "..\release"
#endif

[Setup]
; Never change AppId: it's how Windows and later installers recognise an existing install.
AppId={{7E573877-05AE-41C6-B5B1-C8F61F9644DF}
AppName=Wallpaper Plus
AppVersion={#AppVersion}
AppVerName=Wallpaper Plus {#AppVersion}
AppPublisher=ElishaPervez
AppPublisherURL=https://github.com/ElishaPervez/wallpaper-plus
AppSupportURL=https://github.com/ElishaPervez/wallpaper-plus/issues
AppUpdatesURL=https://github.com/ElishaPervez/wallpaper-plus/releases
DefaultDirName={autopf}\Wallpaper Plus
DefaultGroupName=Wallpaper Plus
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1809: the oldest release the WebView2 runtime (the settings window) supports.
MinVersion=10.0.17763
SetupIconFile=..\res\icon.ico
UninstallDisplayIcon={app}\WallpaperPlus.exe
UninstallDisplayName=Wallpaper Plus
OutputDir={#OutputDir}
OutputBaseFilename=WallpaperPlus-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The running copies are closed in [Code]; this only mops up if that didn't work. The player is
; started again by [Run], not by Windows' Restart Manager.
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[InstallDelete]
; The settings UI is replaced wholesale, so files dropped by a newer version don't linger.
Type: filesandordirs; Name: "{app}\ui"

[Files]
Source: "{#BuildDir}\WallpaperPlus.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\WallpaperPlusSettings.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\third_party\webview2\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "WebView2-SDK-LICENSE.txt"; Flags: ignoreversion
Source: "..\third_party\webview2\NOTICE.txt"; DestDir: "{app}\licenses"; DestName: "WebView2-SDK-NOTICE.txt"; Flags: ignoreversion
Source: "..\third_party\json-LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "nlohmann-json-LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Wallpaper Plus"; Filename: "{app}\WallpaperPlusSettings.exe"
Name: "{autodesktop}\Wallpaper Plus"; Filename: "{app}\WallpaperPlusSettings.exe"; Tasks: desktopicon

[Run]
; Starts the player as the signed-in user, not elevated (so it writes the right %APPDATA% and
; sign-in entry). On a first install it has no wallpaper yet and opens the settings window itself;
; on an upgrade it just puts the wallpapers back.
Filename: "{app}\WallpaperPlus.exe"; Description: "Start Wallpaper Plus"; Flags: nowait postinstall runasoriginaluser

[Code]
const
  WM_CLOSE = $0010;
  PlayerClass = 'WallpaperPlus.Controller';      // src\player\main.cpp kControllerClass
  SettingsClass = 'WallpaperPlus.Settings';      // src\settings\main.cpp kClass
  PlayerMutex = 'Local\WallpaperPlus.SingleInstance';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';
  RunValue = 'WallpaperPlus';                    // src\player\main.cpp kRunValue

// Asks the window of that class to close (what "WallpaperPlus.exe --quit" does for the player) and
// waits up to TimeoutMs for it to go.
procedure CloseWindowOfClass(const ClassName: String; TimeoutMs: Integer);
var
  Wnd: HWND;
  Waited: Integer;
begin
  Wnd := FindWindowByClassName(ClassName);
  if Wnd = 0 then Exit;
  PostMessage(Wnd, WM_CLOSE, 0, 0);
  Waited := 0;
  while (FindWindowByClassName(ClassName) <> 0) and (Waited < TimeoutMs) do begin
    Sleep(100);
    Waited := Waited + 100;
  end;
end;

// Closes any running player and settings window, wherever they were started from, so their exes
// can be replaced or removed. The player's window goes first, then it stops its videos and puts
// the normal wallpaper back before it exits and lets go of its single-instance mutex.
procedure CloseRunningCopies();
var
  Waited: Integer;
begin
  CloseWindowOfClass(SettingsClass, 5000);
  CloseWindowOfClass(PlayerClass, 10000);
  Waited := 0;
  while CheckForMutexes(PlayerMutex) and (Waited < 10000) do begin
    Sleep(100);
    Waited := Waited + 100;
  end;
  Sleep(500);  // the settings window's process exits a moment after its window
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningCopies();
  Result := '';
end;

// The uninstaller runs elevated. With a standard account that elevated with another (admin)
// account's password, HKCU and {userappdata} / {localappdata} here are that admin account's, not
// the user's who ran Wallpaper Plus, and Inno Setup can't run anything as the original user while
// uninstalling (ExecAsOriginalUser and runasoriginaluser are install-only). So the per-user
// cleanup is done by WallpaperPlus.exe --uninstall-cleanup (src\player\uninstall.h), which works
// out who is signed in to this session and cleans up theirs. It needs the exe, so it runs before
// the files are removed, and the question comes first.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DeleteData: Boolean;
  Params: String;
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    CloseRunningCopies();
    DeleteData := SuppressibleMsgBox('Also delete your settings and library?' + #13#10 + #13#10 +
                                     'Your video files are not deleted either way.',
                                     mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES;
    Params := '--uninstall-cleanup';
    if DeleteData then Params := Params + ' --delete-data';
    if not Exec(ExpandConstant('{app}\WallpaperPlus.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      ResultCode := 2;
    // The approving account's own entry, even if the exe couldn't run. Removing it is always right:
    // it would point at a program that's about to be gone.
    RegDeleteValue(HKEY_CURRENT_USER, RunKey, RunValue);
    if DeleteData and (ResultCode <> 0) then
      SuppressibleMsgBox('Some of your Wallpaper Plus settings couldn''t be deleted.' + #13#10 + #13#10 +
                         'You can delete them yourself: the WallpaperPlus folders in %APPDATA% and %LOCALAPPDATA%.',
                         mbInformation, MB_OK, IDOK);
  end;
end;
