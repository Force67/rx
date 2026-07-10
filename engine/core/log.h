#ifndef RX_CORE_LOG_H_
#define RX_CORE_LOG_H_

#include <format>
#include <string_view>

#include "core/export.h"

namespace rx {

enum class LogLevel { kTrace, kDebug, kInfo, kWarn, kError };

namespace detail {
RX_CORE_EXPORT void LogMessage(LogLevel level, std::string_view message);
}

RX_CORE_EXPORT void SetLogLevel(LogLevel level);

template <typename... Args>
void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
  detail::LogMessage(level, std::format(fmt, std::forward<Args>(args)...));
}

#define RX_TRACE(...) ::rx::Log(::rx::LogLevel::kTrace, __VA_ARGS__)
#define RX_DEBUG(...) ::rx::Log(::rx::LogLevel::kDebug, __VA_ARGS__)
#define RX_INFO(...) ::rx::Log(::rx::LogLevel::kInfo, __VA_ARGS__)
#define RX_WARN(...) ::rx::Log(::rx::LogLevel::kWarn, __VA_ARGS__)
#define RX_ERROR(...) ::rx::Log(::rx::LogLevel::kError, __VA_ARGS__)

}  // namespace rx

#endif  // RX_CORE_LOG_H_
