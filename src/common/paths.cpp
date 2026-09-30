#include "paths.h"

#include <windows.h>
#include <shlobj.h>
#include <cwctype>

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L'\\'));
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::wstring LongPath(const std::wstring& path) {  // TEMP is often an 8.3 short path
    wchar_t buf[MAX_PATH];
    DWORD n = GetLongPathNameW(path.c_str(), buf, MAX_PATH);
    return n > 0 && n < MAX_PATH ? std::wstring(buf, n) : path;
}

static bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) s = path;
    CoTaskMemFree(path);
    return s;
}

static bool UnderTemp(const std::wstring& dir) {
    wchar_t temp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (!n || n >= MAX_PATH) return false;
    std::wstring t = Lower(LongPath(temp)), d = Lower(LongPath(dir)) + L"\\";
    if (!t.empty() && t.back() != L'\\') t += L'\\';
    return d.rfind(t, 0) == 0;
}

bool RunningFromArchive() {
    const std::wstring dir = LongPath(ExeDirectory());
    if (!UnderTemp(dir)) return false;
    // Explorer: %TEMP%\Temp1_name.zip\...   7-Zip: %TEMP%\7zO1234\   WinRAR: %TEMP%\Rar$EXa1.234\ .
    const std::wstring d = Lower(dir) + L"\\";
    for (size_t start = 0; start < d.size();) {
        size_t end = d.find(L'\\', start);
        const std::wstring part = d.substr(start, end - start);
        const bool explorerZip = part.size() > 5 && part.rfind(L"temp", 0) == 0 && iswdigit(part[4]) &&
                                 part.find(L'_') != std::wstring::npos && part.ends_with(L".zip");
        if (explorerZip || part.rfind(L"7zo", 0) == 0 || part.rfind(L"rar$", 0) == 0) return true;
        start = end + 1;
    }
    return false;
}

bool RefuseToRunFromArchive() {
    if (!RunningFromArchive()) return false;
    MessageBoxW(nullptr,
                L"Wallpaper Plus is running from inside a zip file, from a temporary copy Windows deletes "
                L"again, so it would stop working (and wouldn't start when you sign in).\n\n"
                L"Right-click the zip, choose \"Extract All...\", then open WallpaperPlus.exe in the extracted folder.",
                L"Wallpaper Plus", MB_ICONINFORMATION);
    return true;
}

// Copies src to dst unless dst exists, through a temporary name: the other program starting at the
// same moment never reads a half-copied file, and whichever copy lands first wins. Failure is harmless.
static void CopyIfMissing(const std::wstring& src, const std::wstring& dst) {
    if (!FileExists(src) || FileExists(dst)) return;
    const std::wstring tmp = dst + L".copy-" + std::to_wstring(GetCurrentProcessId());
    if (CopyFileW(src.c_str(), tmp.c_str(), FALSE) && MoveFileExW(tmp.c_str(), dst.c_str(), 0)) return;
    DeleteFileW(tmp.c_str());
}

// Folder of the exe that starts at sign in (the player writes this value, see main.cpp): where a
// copy from before the installer, e.g. an extracted zip, kept its settings.
static std::wstring AutostartFolder() {
    wchar_t cmd[MAX_PATH + 3] = {};
    DWORD size = sizeof(cmd);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"WallpaperPlus",
                     RRF_RT_REG_SZ, nullptr, cmd, &size) != ERROR_SUCCESS)
        return {};
    std::wstring exe = cmd;
    if (!exe.empty() && exe.front() == L'"') exe = exe.substr(1, exe.find(L'"', 1) - 1);
    const size_t slash = exe.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash);
}

// Where older versions kept settings: next to the exes (portable), %LOCALAPPDATA%\WallpaperPlus
// (when that wasn't writable), or next to the exe that starts at sign in. Several can hold a
// wallpaper.ini; the most recently saved one is the one that was in use. Empty if none.
static std::wstring OldDataDirectory() {
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    const std::wstring candidates[] = {ExeDirectory(), local.empty() ? L"" : local + L"\\WallpaperPlus",
                                       AutostartFolder()};
    std::wstring best;
    FILETIME newest{};
    for (const auto& dir : candidates) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (dir.empty() || !GetFileAttributesExW((dir + L"\\wallpaper.ini").c_str(), GetFileExInfoStandard, &a) ||
            (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (best.empty() || CompareFileTime(&a.ftLastWriteTime, &newest) > 0) {
            best = dir;
            newest = a.ftLastWriteTime;
        }
    }
    return best;
}

// First run in %APPDATA%: brings along the settings and library from where an older version kept
// them, so they don't seem to vanish. Nothing already there is overwritten. wallpaper.ini goes
// last: once it's there, this never runs again. If both programs start at once, both copy, and
// CopyIfMissing makes that safe.
static void CopyInSettings(const std::wstring& to) {
    if (FileExists(to + L"\\wallpaper.ini")) return;
    const std::wstring from = OldDataDirectory();
    if (from.empty() || _wcsicmp(from.c_str(), to.c_str()) == 0) return;
    CopyIfMissing(from + L"\\library.json", to + L"\\library.json");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((from + L"\\thumbs\\*.jpg").c_str(), &fd);  // the library's thumbnails
    if (find != INVALID_HANDLE_VALUE) {
        CreateDirectoryW((to + L"\\thumbs").c_str(), nullptr);
        do CopyIfMissing(from + L"\\thumbs\\" + fd.cFileName, to + L"\\thumbs\\" + fd.cFileName);
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    CopyIfMissing(from + L"\\wallpaper.ini", to + L"\\wallpaper.ini");
}

const std::wstring& DataDirectory() {
    static const std::wstring dir = [] {
        const std::wstring roaming = KnownFolder(FOLDERID_RoamingAppData);
        if (roaming.empty()) return ExeDirectory();  // no user profile at all; nothing better to do
        std::wstring d = roaming + L"\\WallpaperPlus";
        CreateDirectoryW(d.c_str(), nullptr);  // no-op if it exists; the parent always does
        CopyInSettings(d);
        return d;
    }();
    return dir;
}

std::wstring WebViewDataDirectory() {
    return KnownFolder(FOLDERID_LocalAppData) + L"\\WallpaperPlus\\WebView2";
}
