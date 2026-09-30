Wallpaper Plus — video wallpapers for Windows 10 and 11 (64-bit)

START
  1. Extract this whole folder somewhere you can write to, e.g. Documents or your Desktop
     (not Program Files: settings are saved next to the programs).
  2. Double-click WallpaperPlus.exe. The settings window opens; pick a video and it plays
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

TROUBLE
  - Nothing plays: open wallpaper-plus.log in this folder; it says why.
  - Settings window won't open: install "Microsoft Edge WebView2 Runtime" from Microsoft
    (already built into Windows 11).
  - To remove it: right-click the tray icon > Quit, turn off "Start with Windows" first if you
    want, then delete this folder.
