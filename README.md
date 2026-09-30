# Wallpaper Plus

A lightweight video wallpaper for Windows 11, in two programs:

- **`build\WallpaperPlus.exe`** — the player. Always running, lives in the tray. Videos are decoded
  by the GPU's video decoder and drawn by the GPU, so the processor barely does anything. It
  pauses a monitor whenever that monitor is covered, and every monitor when the screen is locked
  or off.
- **`build\WallpaperPlusSettings.exe`** — the settings window. Opens from the tray icon and exits
  completely when you close it (about 100 MB of memory while open, nothing when closed).

## Use

1. Run `build\WallpaperPlus.exe`. A tray icon appears; if no wallpaper is set yet, the settings
   window opens.
2. In the settings window: **Library** → *Add videos* / *Add folder* (or drop files onto the
   window). Click a video to put it on the selected monitor, or use the small monitor map on each
   card to pick a specific monitor.
3. **Monitors** shows your desk as it's really laid out. Select a monitor to change its wallpaper,
   build a rotation (several videos, switching on a timer, optional shuffle), and adjust fit,
   brightness, and speed.
4. **Settings**: frame-rate cap, video decoding, pause rules (covered monitors, fullscreen apps, specific apps),
   library folders, start with Windows.

Tray icon: click to open settings; right-click for *Pause wallpapers* and *Quit*.
Double-clicking `WallpaperPlus.exe` while it runs also opens settings.
`WallpaperPlus.exe --quit` stops it from a script.

Decoding: each video goes to the best decoder the PC has for it. *Automatic* tries the GPU the
monitor is plugged into first, then NVIDIA's decoder, then any other GPU (Intel/AMD), then the
processor; *Power saving* tries the integrated GPU first so a laptop's discrete GPU can sleep;
*Processor* always decodes on the processor. If a decoder can't take a video, or fails partway
through, the video restarts on the next one down; a video that ended up on the processor because
a GPU decoder failed partway through tries the best GPU decoder again each time it loops. On a PC
with no working GPU (VMs, remote desktop, broken drivers), Windows' software renderer draws, so
wallpapers still play, at a high processor cost. Settings shows which decoder each monitor is using.

Formats: H.264 always works. HEVC and AV1 need Microsoft's "HEVC Video Extensions" /
"AV1 Video Extension" from the Store. Wallpapers play without sound.

Data folder: `wallpaper.ini` (settings, hand-editable), `library.json`, `thumbs\`, `webcache\`
(Browse tab downloads), `player-status.txt` (which decoder each monitor uses, written by the
player), and `wallpaper-plus.log` (read this if something doesn't play). Both programs pick the
same folder:

1. next to the exe, if a `wallpaper.ini` is already there and the folder can be written;
2. otherwise `%LOCALAPPDATA%\WallpaperPlus`, if a `wallpaper.ini` is already there;
3. otherwise next to the exe, if it can be written and isn't inside the temp folder;
4. otherwise `%LOCALAPPDATA%\WallpaperPlus` (Program Files, drive roots, folders blocked by
   Windows Security's "Controlled folder access"). The first time, the settings, library and
   thumbnails already next to the exe are copied there; nothing already there is overwritten.

The settings window always keeps its browser cache in `%LOCALAPPDATA%\WallpaperPlus\WebView2`.
Started straight from inside a zip, the player refuses to run and asks for the zip to be extracted.

## Build

`build.bat` (needs Visual Studio 2022 Build Tools with the C++ workload). Third-party pieces live in
`third_party\` (Microsoft WebView2 SDK, nlohmann/json) and `ui\fonts\` (Big Shoulders Display, SIL
Open Font License).

Dev loop: `tools\dev-restart.sh` closes both programs, rebuilds, and relaunches them with the
DevTools port open; `node tools\cdp.mjs eval|evalfile|shot|logs` drives the settings page.
Set `WALLPAPERPLUS_STATS=1` to log frames shown per monitor every 10 seconds, and
`WALLPAPERPLUS_NO_GPU=1` to make the player act as if the PC had no GPU (software renderer only).
