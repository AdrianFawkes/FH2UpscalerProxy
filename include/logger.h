#pragma once
// ============================================================================
// logger.h — Simple file logger for the DLSS proxy
// ============================================================================
#include <string>
#include <format>

namespace Log {

void Init(const wchar_t* logFileName = L"fh2_dlss_proxy.log");
void Shutdown();

void Info(const std::string& msg);
void Warn(const std::string& msg);
void Error(const std::string& msg);

template <typename... Args>
void InfoF(std::format_string<Args...> fmt, Args&&... args) {
    Info(std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void WarnF(std::format_string<Args...> fmt, Args&&... args) {
    Warn(std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void ErrorF(std::format_string<Args...> fmt, Args&&... args) {
    Error(std::format(fmt, std::forward<Args>(args)...));
}

} // namespace Log
