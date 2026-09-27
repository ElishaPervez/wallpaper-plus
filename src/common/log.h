#pragma once
#include <string>

// Small append-only log next to the exe. Overwritten on each launch.
void LogInit(const std::wstring& path);
void Log(const wchar_t* fmt, ...);
