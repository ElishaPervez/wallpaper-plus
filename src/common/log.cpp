#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

static FILE* g_file = nullptr;
static std::mutex g_mutex;

void LogInit(const std::wstring& path) {
    g_file = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
}

void Log(const wchar_t* fmt, ...) {
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard lock(g_mutex);
    fwprintf(g_file, L"%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_file, fmt, args);
    va_end(args);
    fputwc(L'\n', g_file);
    fflush(g_file);
}
