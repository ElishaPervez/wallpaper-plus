#pragma once
#include <string>

// Folder holding WallpaperPlus.exe, WallpaperPlusSettings.exe and ui\.
std::wstring ExeDirectory();

// Where settings, library, thumbnails, caches, status and the log live: always
// %APPDATA%\WallpaperPlus, wherever the exes are, so both programs agree and an installed copy in
// Program Files (which the user can't write to) works like any other.
// The first time, settings, library and thumbnails from where older versions kept them are copied
// in (see paths.cpp). Computed once per process; the folder exists on return.
const std::wstring& DataDirectory();

// %LOCALAPPDATA%\WallpaperPlus\WebView2: the settings window's browser cache. Kept out of
// DataDirectory() because it's cache, and a roaming profile shouldn't carry it around.
std::wstring WebViewDataDirectory();

// True if Windows (or 7-Zip / WinRAR) is running the exe straight out of a zip, from a temporary
// copy it will delete: "start at sign in" would point at a file that's about to vanish.
bool RunningFromArchive();

// Tells the user to extract the zip first. Returns true if it did (the caller should exit).
bool RefuseToRunFromArchive();
