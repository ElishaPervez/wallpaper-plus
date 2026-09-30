#pragma once

// "WallpaperPlus.exe --uninstall-cleanup [--delete-data]": run by the uninstaller, elevated, before
// it removes the program files.
//
// The uninstaller runs as whichever account approved it. A standard user who typed an
// administrator's password gets an uninstaller running as that administrator, whose HKCU and
// %APPDATA% aren't the ones Wallpaper Plus used, and Inno Setup can't run anything as the original
// user during uninstall. So this looks up the user signed in to this Windows session instead:
//  - removes the start-at-sign-in entry pointing at this exe from every signed-in user's registry
//    (that includes the session's user and the approving administrator);
//  - with --delete-data, deletes that session user's %APPDATA%\WallpaperPlus and
//    %LOCALAPPDATA%\WallpaperPlus.
// Returns the exit code: 0 done, 1 some data couldn't be deleted, 2 the session's user or their
// folders couldn't be found (nothing deleted).
int UninstallCleanup(bool deleteData);
