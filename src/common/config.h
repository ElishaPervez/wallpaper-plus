#pragma once
#include <map>
#include <string>
#include <vector>

enum class FitMode { Fill, Fit, Stretch };

// Which chip decodes the videos. Whatever is chosen, a video the chosen chip can't handle falls
// through to the next one, ending with the processor, so something always plays.
//  Auto: the GPU driving the monitor first, then NVIDIA's decoder, then other GPUs (fastest first), then the processor.
//  PowerSaving: integrated GPU first (keeps a laptop's discrete GPU asleep), then others, then the processor.
//  Cpu: always decode on the processor; the GPU only draws.
enum class DecoderPreference { Auto, PowerSaving, Cpu };

// What one monitor plays. One video = a single wallpaper; several = a playlist.
struct MonitorSetting {
    std::vector<std::wstring> videos;
    FitMode fit = FitMode::Fill;
    int brightness = 100;    // percent, 10..100 (below 100 dims toward black)
    double speed = 1.0;      // playback rate, 0.25..2
    int rotateMinutes = 30;  // playlists: switch video after this much playing time (0 = never)
    bool shuffle = false;    // playlists: random order
    bool operator==(const MonitorSetting&) const = default;
};

struct Config {
    bool pauseWhenCovered = true;         // freeze a monitor while windows fully cover it
    bool pauseAllWhenFullscreen = false;  // freeze every monitor while a fullscreen app has focus
    std::vector<std::wstring> pauseFor;   // freeze every monitor while any of these exes run (lowercase)
    int fpsCap = 0;                       // max frames shown per second (0 = video's own rate)
    DecoderPreference decoder = DecoderPreference::Auto;
    bool autostart = true;
    bool paused = false;                  // manual "pause all" from the tray or settings window
    MonitorSetting defaults;
    std::map<int, MonitorSetting> perMonitor;  // key: 1-based monitor number, left to right

    MonitorSetting ForMonitor(int number) const;
};

// Reads the ini file. If it doesn't exist, writes a commented template first.
Config LoadConfig(const std::wstring& path);

const char* DecoderName(DecoderPreference d);  // "auto" | "power_saving" | "cpu"
DecoderPreference DecoderFromName(const std::wstring& name);

// Writes the whole file (atomically, so a reader never sees half a file).
bool SaveConfig(const std::wstring& path, const Config& cfg);
