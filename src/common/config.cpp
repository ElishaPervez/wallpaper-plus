#include "config.h"
#include "log.h"

#include <windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return {};
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    std::wstringstream in(s);
    std::wstring part;
    while (std::getline(in, part, sep)) {
        part = Trim(part);
        if (part.size() >= 2 && part.front() == L'"' && part.back() == L'"') part = part.substr(1, part.size() - 2);
        if (!part.empty()) out.push_back(part);
    }
    return out;
}

using Section = std::map<std::wstring, std::wstring>;

static std::map<std::wstring, Section> ParseIni(const std::wstring& text) {
    std::map<std::wstring, Section> out;
    std::wstring current;
    std::wistringstream in(text);
    std::wstring line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') {
            current = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Lower(Trim(line.substr(0, eq)));
        std::wstring value = Trim(line.substr(eq + 1));
        if (!value.empty() && value.front() == L'"') {
            // Quoted: taken verbatim up to the closing quote (Windows paths can't contain '"'), so
            // names like "clip #2.mp4" survive. SaveConfig always quotes video paths.
            size_t close = value.find(L'"', 1);
            value = value.substr(1, close == std::wstring::npos ? std::wstring::npos : close - 1);
        } else {
            // Unquoted (hand-edited): ';' or '#' after whitespace starts a comment.
            for (size_t i = 1; i < value.size(); ++i) {
                if ((value[i] == L';' || value[i] == L'#') && (value[i - 1] == L' ' || value[i - 1] == L'\t')) {
                    value.resize(i);
                    break;
                }
            }
            value = Trim(value);
        }
        out[current][key] = value;
    }
    return out;
}

static bool ParseBool(const std::wstring& v, bool fallback) {
    std::wstring l = Lower(v);
    if (l == L"true" || l == L"yes" || l == L"1" || l == L"on") return true;
    if (l == L"false" || l == L"no" || l == L"0" || l == L"off") return false;
    return fallback;
}

static void ApplySection(const Section& s, MonitorSetting& m) {
    // Windows paths can't contain '|', so it separates playlist entries.
    if (auto it = s.find(L"video"); it != s.end()) m.videos = Split(it->second, L'|');
    if (auto it = s.find(L"fit"); it != s.end()) {
        std::wstring f = Lower(it->second);
        m.fit = f == L"fit" ? FitMode::Fit : f == L"stretch" ? FitMode::Stretch : FitMode::Fill;
    }
    if (auto it = s.find(L"brightness"); it != s.end()) m.brightness = std::clamp(_wtoi(it->second.c_str()), 10, 100);
    if (auto it = s.find(L"speed"); it != s.end()) m.speed = std::clamp(_wtof(it->second.c_str()), 0.25, 2.0);
    if (auto it = s.find(L"rotate_minutes"); it != s.end()) m.rotateMinutes = std::max(0, _wtoi(it->second.c_str()));
    if (auto it = s.find(L"shuffle"); it != s.end()) m.shuffle = ParseBool(it->second, false);
}

MonitorSetting Config::ForMonitor(int number) const {
    auto it = perMonitor.find(number);
    return it != perMonitor.end() ? it->second : defaults;
}

Config LoadConfig(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SaveConfig(path, Config{});
        Log(L"Wrote settings template to %s", path.c_str());
    }

    std::string bytes;
    // An editor may briefly hold the file locked while saving; retry a few times.
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::ifstream f(path, std::ios::binary);
        if (f) {
            bytes.assign(std::istreambuf_iterator<char>(f), {});
            break;
        }
        Sleep(50);
    }
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
        (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);

    auto ini = ParseIni(Utf8ToWide(bytes));
    Config cfg;
    const Section& g = ini[L"general"];
    auto get = [&](const wchar_t* key) -> const std::wstring* {
        auto it = g.find(key);
        return it != g.end() ? &it->second : nullptr;
    };
    if (auto v = get(L"pause_when_covered")) cfg.pauseWhenCovered = ParseBool(*v, true);
    if (auto v = get(L"pause_all_when_fullscreen")) cfg.pauseAllWhenFullscreen = ParseBool(*v, false);
    if (auto v = get(L"pause_for"))
        for (auto& exe : Split(*v, L',')) cfg.pauseFor.push_back(Lower(exe));
    if (auto v = get(L"fps_cap")) cfg.fpsCap = std::clamp(_wtoi(v->c_str()), 0, 240);
    if (auto v = get(L"autostart")) cfg.autostart = ParseBool(*v, true);
    if (auto v = get(L"paused")) cfg.paused = ParseBool(*v, false);

    ApplySection(ini[L"default"], cfg.defaults);
    for (const auto& [name, section] : ini) {
        if (name.rfind(L"monitor", 0) != 0) continue;
        int n = _wtoi(name.c_str() + 7);
        if (n <= 0) continue;
        MonitorSetting m = cfg.defaults;  // unspecified keys inherit from [default]
        ApplySection(section, m);
        cfg.perMonitor[n] = m;
    }
    return cfg;
}

static void WriteSection(std::ostringstream& o, const MonitorSetting& m) {
    std::wstring videos;
    for (size_t i = 0; i < m.videos.size(); ++i) videos += (i ? L" | " : L"") + m.videos[i];
    static const char* const kFit[] = {"fill", "fit", "stretch"};
    char speed[16];
    snprintf(speed, sizeof(speed), "%g", m.speed);
    o << "video = \"" << WideToUtf8(videos) << "\"\r\n"
      << "fit = " << kFit[(int)m.fit] << "\r\n"
      << "brightness = " << m.brightness << "\r\n"
      << "speed = " << speed << "\r\n"
      << "rotate_minutes = " << m.rotateMinutes << "\r\n"
      << "shuffle = " << (m.shuffle ? "true" : "false") << "\r\n";
}

bool SaveConfig(const std::wstring& path, const Config& cfg) {
    std::ostringstream o;
    std::wstring pauseFor;
    for (size_t i = 0; i < cfg.pauseFor.size(); ++i) pauseFor += (i ? L", " : L"") + cfg.pauseFor[i];
    o << "; Wallpaper Plus settings. The settings window writes this file; hand edits apply on save.\r\n"
      << "; video: full path to a video. Several paths separated by | make a playlist.\r\n"
      << "; fit: fill (crop to fill) | fit (black bars) | stretch.  brightness: 10-100.  speed: 0.25-2.\r\n"
      << "; Monitors are numbered left to right, starting at 1.\r\n\r\n"
      << "[general]\r\n"
      << "pause_when_covered = " << (cfg.pauseWhenCovered ? "true" : "false") << "\r\n"
      << "pause_all_when_fullscreen = " << (cfg.pauseAllWhenFullscreen ? "true" : "false") << "\r\n"
      << "pause_for = \"" << WideToUtf8(pauseFor) << "\"\r\n"
      << "fps_cap = " << cfg.fpsCap << "\r\n"
      << "autostart = " << (cfg.autostart ? "true" : "false") << "\r\n"
      << "paused = " << (cfg.paused ? "true" : "false") << "\r\n\r\n"
      << "[default]\r\n";
    WriteSection(o, cfg.defaults);
    for (const auto& [n, m] : cfg.perMonitor) {
        o << "\r\n[monitor" << n << "]\r\n";
        WriteSection(o, m);
    }

    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        const std::string text = o.str();
        f.write(text.data(), (std::streamsize)text.size());
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
