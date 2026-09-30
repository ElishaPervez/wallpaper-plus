Wallpaper Plus — video wallpapers for Windows 10 and 11 (64-bit)

START
  1. Right-click the zip and choose "Extract All...". It won't run from inside the zip.
  2. Open the extracted folder and double-click WallpaperPlus.exe. The settings window opens; pick a video and it plays
     on your desktop.
  Windows may say "Windows protected your PC" because the program isn't signed.
  Click "More info", then "Run anyway".

USE
  - The tray icon (bottom-right, by the clock) opens settings; right-click it to pause or quit.
  - It starts with Windows by default; turn that off in Settings > General.
  - It pauses by itself when windows cover a monitor, and while the screen is locked or off.

VIDEOS
  - MP4 (H.264) always plays. HEVC and AV1 videos need Microsoft's free "HEVC Video Extensions
    from Device Manufacturer" / "AV1 Video Extension" from the Microsoft Store.
  - Videos are decoded by your graphics card when it can, otherwise by the processor, so they
    play on any PC. Settings > Video decoding shows what each monitor is using. On a laptop,
    "Power saving" keeps the NVIDIA/AMD chip asleep.

WHERE SETTINGS GO
  Next to the programs, in this folder. If Windows won't let the programs write here (for
  example Windows Security's "Controlled folder access", or Program Files), they go to
  %LOCALAPPDATA%\WallpaperPlus instead (paste that into File Explorer's address bar), and the
  settings and library you already had here come along.

TROUBLE
  - Nothing plays: open wallpaper-plus.log (in one of the two places above); it says why.
  - Settings window won't open: install "Microsoft Edge WebView2 Runtime" from Microsoft
    (already built into Windows 11).
  - To remove it:
      1. In the settings window, turn off "Start with Windows" (Settings > General).
      2. Right-click the tray icon > Quit.
      3. Delete this folder.
      4. Also delete the folder %LOCALAPPDATA%\WallpaperPlus (paste that into File Explorer's
         address bar). The settings window always keeps some files there, and your settings
         may be there too.
