<p align="center"><img src="res/icon-256.png" width="128" alt="Wallpaper Plus logo"></p>

# Wallpaper Plus

Video wallpapers for Windows 11. Pick a video, or a playlist of them, for each monitor, and it plays
behind your desktop icons. The player is small and stays out of the way: videos are decoded and
drawn by the graphics card, and playback stops while you can't see it.

## Features

- **A wallpaper per monitor.** The Monitors tab shows your screens as they're laid out on your
  desk. Each one gets its own video, fit (fill, fit with black bars, or stretch), brightness and
  playback speed.
- **Playlists.** Give a monitor several videos and it switches between them on a timer, in order
  or shuffled.
- **Library.** Add videos one by one, drop them onto the window, or add whole folders; new videos
  in those folders are picked up each time the settings window opens. Cards show a preview on
  hover and can be searched and sorted.
- **Browse tab.** Browse, search and preview wallpapers from [motionbgs.com](https://motionbgs.com)
  by topic, and download them in HD or 4K (4K is picked when a screen is bigger than 1080p). They
  are saved to `Videos\Wallpaper Plus` and added to your library.
- **Graphics card decoding, with a fallback.** Each video goes to the best decoder the PC has for
  it: first the graphics card the monitor is plugged into, then an NVIDIA card, then any other
  graphics card, then the processor. If a decoder can't handle a video, or fails partway through,
  the video restarts on the next one down, so something always plays. A *Power saving* option
  prefers the integrated graphics so a laptop's separate graphics card can stay off, and a
  *Processor* option decodes on the processor only. Settings shows which decoder each monitor is
  using.
- **Pauses when nobody can see it.** A monitor whose wallpaper is fully covered by windows pauses.
  Every monitor pauses while the screen is locked or off, and, if you choose, while a fullscreen
  app has focus or while specific apps are running.
- **Frame rate cap**, to save more power on high-frame-rate videos.
- **Survives the unexpected.** Explorer restarts, monitors being plugged in or rearranged, and the
  graphics driver resetting are all picked up without restarting the app.
- **Tray icon.** Click it to open the settings window; right-click to pause all wallpapers or quit.
- **Auto startup on sign in** (on by default).

The settings window is a separate program that only runs while it's open, so it uses no memory
once closed. When the player quits, your normal Windows wallpaper comes back.

**Video formats.** H.264 always plays. HEVC and AV1 videos need Microsoft's free "HEVC Video
Extensions" and "AV1 Video Extension" from the Microsoft Store. Wallpapers play without sound.

## Requirements

- Windows 11, 64-bit.
- Microsoft Edge WebView2 Runtime, for the settings window. Windows 11 already includes it.

## Install

1. Download `WallpaperPlus-Setup-x.y.z.exe` from the
   [Releases page](https://github.com/ElishaPervez/wallpaper-plus/releases).
2. Run it. Windows asks for admin permission, because Wallpaper Plus installs into Program Files.
3. The installer isn't code-signed, so Windows SmartScreen may say "Windows protected your PC".
   Click **More info**, then **Run anyway**.

When setup finishes, the player starts and, the first time, opens the settings window so you can
pick a wallpaper. Afterwards, open settings from the tray icon or the *Wallpaper Plus* Start menu
entry.

To update, run the newer installer over the old one. Your settings and library are kept.

## Where settings live

Everything is in `%APPDATA%\WallpaperPlus` (paste that into File Explorer's address bar):

| File | What it is |
| --- | --- |
| `wallpaper.ini` | Settings. Plain text, can be edited by hand; the player picks up changes immediately. |
| `library.json`, `thumbs\` | Your video library and its thumbnails. |
| `webcache\` | Thumbnails and preview clips from the Browse tab. |
| `wallpaper-plus.log` | What the player did. Look here if a video doesn't play. |
| `player-status.txt` | Which decoder each monitor is using, written by the player. |

The settings window keeps its browser cache in `%LOCALAPPDATA%\WallpaperPlus\WebView2`.

Coming from an older version that kept its settings next to the program, or in
`%LOCALAPPDATA%\WallpaperPlus`? The first time the new version runs, it copies your settings,
library and thumbnails over. It never overwrites anything already in `%APPDATA%\WallpaperPlus`.

## Uninstall

Use *Settings → Apps → Installed apps → Wallpaper Plus → Uninstall*. This closes Wallpaper Plus,
removes it from sign-in startup, then asks whether to delete your settings and library as well.
Videos you downloaded to `Videos\Wallpaper Plus` are never deleted.

## Build from source

You need:

- **Visual Studio 2022 Build Tools** (or Visual Studio 2022) with the *Desktop development with
  C++* workload, including the MSVC x64 build tools and a Windows 11 SDK (it provides the resource
  compiler and the shader compiler the build uses).
- **Inno Setup 6**, only to build the installer: `winget install JRSoftware.InnoSetup`.

Then:

- `build.bat` builds `build\WallpaperPlus.exe` (the player), `build\WallpaperPlusSettings.exe`
  (the settings window) and copies the settings UI to `build\ui\`. You can run them straight from
  `build\`. It finds the Build Tools in their default location, or uses an already open Developer
  Command Prompt.
- `tools/make-release.sh` (run from Git Bash) builds, then compiles `installer\WallpaperPlus.iss`
  into `release\WallpaperPlus-Setup-<version>.exe`. Pass a version such as `1.2.3`, or leave it out
  for a test build.

Useful while developing:

- `WallpaperPlus.exe --quit` stops the running player.
- `WALLPAPERPLUS_DEVTOOLS=1` enables the browser developer tools in the settings window.
- `WALLPAPERPLUS_STATS=1` logs the frames shown per monitor every 10 seconds.
- `WALLPAPERPLUS_NO_GPU=1` makes the player act as if the PC had no graphics card.

### Code layout

- `src/player/` - the always-running player (Direct3D 11 and Media Foundation).
- `src/settings/` - the settings window, which hosts the web UI in `ui/` with WebView2.
- `src/common/` - settings file, monitor list, paths and log, shared by both.
- `installer/` - the Inno Setup script.

## Releases

Every push to `main` and every pull request is built by GitHub Actions
(`.github/workflows/build.yml`); the installer is attached to the run as an artifact. To publish a
release, push a version tag:

```
git tag v1.2.3
git push origin v1.2.3
```

Actions then builds `WallpaperPlus-Setup-1.2.3.exe` and publishes it as the GitHub Release
"Wallpaper Plus 1.2.3".

## Credits and licenses

Wallpaper Plus is released under the [MIT License](LICENSE). It includes:

- [nlohmann/json](https://github.com/nlohmann/json), MIT License
  ([third_party/json-LICENSE.txt](third_party/json-LICENSE.txt)).
- The [Microsoft Edge WebView2 SDK](https://www.nuget.org/packages/Microsoft.Web.WebView2)
  headers and static loader, under Microsoft's license
  ([third_party/webview2/LICENSE.txt](third_party/webview2/LICENSE.txt),
  [NOTICE.txt](third_party/webview2/NOTICE.txt)).
- The [Big Shoulders](https://github.com/xotypeco/big_shoulders) typeface, SIL Open Font License
  1.1 ([ui/fonts/OFL.txt](ui/fonts/OFL.txt)).

The Browse tab shows wallpapers from [motionbgs.com](https://motionbgs.com). Wallpaper Plus is not
affiliated with or endorsed by motionbgs.com; the wallpapers belong to their respective owners.
