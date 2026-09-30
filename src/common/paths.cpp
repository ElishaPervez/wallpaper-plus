#include "paths.h"

#include <windows.h>
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

// Creates and immediately deletes a scratch file.
static bool CanWriteTo(const std::wstring& dir) {
    std::wstring probe = dir + L"\\.write-test-" + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
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
                L"again, so it can't keep your settings.\n\n"
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

// First run in %LOCALAPPDATA%: brings along the settings and library already next to the exes, so
// they don't seem to vanish. Nothing there is overwritten. wallpaper.ini goes last: once it's
// there, this never runs again (and Pick keeps choosing this folder).
static void CopyInSettings(const std::wstring& from, const std::wstring& to) {
    if (FileExists(to + L"\\wallpaper.ini")) return;
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

static std::wstring Pick() {
    const std::wstring exeDir = ExeDirectory();
    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::wstring appData = std::wstring(local) + L"\\WallpaperPlus";
    if (!local[0]) return exeDir;

    if (FileExists(exeDir + L"\\wallpaper.ini") && CanWriteTo(exeDir)) return exeDir;
    if (FileExists(appData + L"\\wallpaper.ini")) return appData;  // chosen on an earlier run
    if (!UnderTemp(exeDir) && CanWriteTo(exeDir)) return exeDir;
    return appData;
}

const std::wstring& DataDirectory() {
    static const std::wstring dir = [] {
        std::wstring d = Pick();
        CreateDirectoryW(d.c_str(), nullptr);  // no-op if it exists; the parent always does
        if (d != ExeDirectory()) CopyInSettings(ExeDirectory(), d);
        return d;
    }();
    return dir;
}
