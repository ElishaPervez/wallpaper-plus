#include "uninstall.h"

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";  // as in main.cpp
static const wchar_t kRunValue[] = L"WallpaperPlus";

static std::wstring SidText(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return {};
    std::wstring s = text;
    LocalFree(text);
    return s;
}

// The account this process runs as (the one that approved the uninstall).
static std::wstring OwnSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    std::vector<BYTE> buf(SECURITY_MAX_SID_SIZE + sizeof(TOKEN_USER));
    DWORD size = 0;
    std::wstring sid;
    if (GetTokenInformation(token, TokenUser, buf.data(), (DWORD)buf.size(), &size))
        sid = SidText(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid);
    CloseHandle(token);
    return sid;
}

// The user signed in to this Windows session: the one who started the uninstall, whoever approved it.
static std::wstring SessionSid() {
    LPWSTR user = nullptr, domain = nullptr;
    DWORD bytes = 0;
    std::wstring sid;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSUserName, &user, &bytes) &&
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSDomainName, &domain, &bytes) &&
        user && *user) {
        const std::wstring account = std::wstring(domain ? domain : L"") + L"\\" + user;
        BYTE sidBuf[SECURITY_MAX_SID_SIZE];
        DWORD sidSize = sizeof(sidBuf), domainSize = 256;
        wchar_t domainName[256];
        SID_NAME_USE use;
        if (LookupAccountNameW(nullptr, account.c_str(), sidBuf, &sidSize, domainName, &domainSize, &use))
            sid = SidText(sidBuf);
    }
    if (user) WTSFreeMemory(user);
    if (domain) WTSFreeMemory(domain);
    return sid;
}

static std::wstring RegText(HKEY root, const std::wstring& key, const wchar_t* value, DWORD flags) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key.c_str(), value, flags, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// A user's roaming or local application data folder ("AppData" / "Local AppData"), read from their
// own registry, which is loaded while they're signed in. Empty if it can't be worked out for sure.
static std::wstring UserFolder(const std::wstring& sid, const wchar_t* name) {
    // Unexpanded: %USERPROFILE% in there is theirs, not this process's.
    std::wstring path = RegText(HKEY_USERS, sid + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                                name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND);
    static const std::wstring kProfile = L"%USERPROFILE%";
    if (path.size() >= kProfile.size() && _wcsnicmp(path.c_str(), kProfile.c_str(), kProfile.size()) == 0) {
        const std::wstring profile = RegText(HKEY_LOCAL_MACHINE,
                                             L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + sid,
                                             L"ProfileImagePath", RRF_RT_REG_SZ);  // e.g. %SystemDrive%\Users\name, expanded
        if (profile.empty()) return {};
        path = profile + path.substr(kProfile.size());
    }
    if (path.empty() || path.find(L'%') != std::wstring::npos) return {};  // another variable: its value for them is unknown
    return path;
}

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) s = path;
    CoTaskMemFree(path);
    return s;
}

// Removes the start-at-sign-in entry that points at this exe, for every user whose registry is
// loaded (everyone signed in). Other users' entries stay; Windows skips ones whose program is gone.
static void RemoveStartupEntries() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(HKEY_USERS, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring key = std::wstring(sub) + L"\\" + kRunKey;
        std::wstring cmd = RegText(HKEY_USERS, key, kRunValue, RRF_RT_REG_SZ);
        if (!cmd.empty() && cmd.front() == L'"') cmd = cmd.substr(1, cmd.find(L'"', 1) - 1);
        if (cmd.empty() || _wcsicmp(cmd.c_str(), self) != 0) continue;
        HKEY run;
        if (RegOpenKeyExW(HKEY_USERS, key.c_str(), 0, KEY_SET_VALUE, &run) != ERROR_SUCCESS) continue;
        RegDeleteValueW(run, kRunValue);
        RegCloseKey(run);
    }
}

// Deletes the folder and everything in it. The settings window's browser processes can take a
// moment to let go of their files after it closes, so this tries a few times.
static bool DeleteFolder(const std::wstring& dir) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        if (!fs::exists(dir, ec) && !ec) return true;
        Sleep(500);
    }
    return false;
}

int UninstallCleanup(bool deleteData) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RemoveStartupEntries();
    int code = 0;
    if (deleteData) {
        const std::wstring own = OwnSid(), session = SessionSid();
        std::wstring roaming, local;
        if (!session.empty() && _wcsicmp(own.c_str(), session.c_str()) == 0) {  // the usual case: the same account
            roaming = KnownFolder(FOLDERID_RoamingAppData);
            local = KnownFolder(FOLDERID_LocalAppData);
        } else if (!session.empty()) {
            roaming = UserFolder(session, L"AppData");
            local = UserFolder(session, L"Local AppData");
        }
        if (roaming.empty() || local.empty()) {
            code = 2;  // not guessing: deleting another account's folders is worse than leaving these
        } else {
            if (!DeleteFolder(roaming + L"\\WallpaperPlus")) code = 1;
            if (!DeleteFolder(local + L"\\WallpaperPlus")) code = 1;
        }
    }
    CoUninitialize();
    return code;
}
