#pragma once
#include <string>

// Folder holding WallpaperPlus.exe, WallpaperPlusSettings.exe and ui\.
std::wstring ExeDirectory();

// Where settings, library, thumbnails, caches, status and the log live. Both programs must land
// on the same answer, so it only depends on what's on disk:
//  - next to the exes (portable) when that folder already has a wallpaper.ini, or can be written;
//  - otherwise %LOCALAPPDATA%\WallpaperPlus. That covers folders the user can't write to (Program
//    Files, drive roots) and Windows Security's "Controlled folder access", which silently stops
//    unsigned programs writing into Documents and Desktop.
// Computed once per process; the folder exists on return.
const std::wstring& DataDirectory();

// True if Windows (or 7-Zip / WinRAR) is running the exe straight out of a zip, from a temporary
// copy it will delete. Settings would vanish and "start with Windows" would point at nothing.
bool RunningFromArchive();

// Tells the user to extract the zip first. Returns true if it did (the caller should exit).
bool RefuseToRunFromArchive();
