#ifndef RX_CORE_LOG_H_
#define RX_CORE_LOG_H_


#include "base/memory/move.h"
#include "base/strings/string_ref.h"
#include "core/export.h"
#include "core/format.h"

namespace rx {

enum class LogLevel { kTrace, kDebug, kInfo, kWarn, kError };

namespace detail {
RX_CORE_EXPORT void LogMessage(LogLevel level, base::StringRef message);
}

RX_CORE_EXPORT void SetLogLevel(LogLevel level);

template <typename... Args>
void Log(LogLevel level, FormatString<typename format_detail::Identity<Args>::type...> fmt,
         const Args&... args) {
  detail::LogMessage(level, StrFormat<Args...>(fmt, args...));
}

#define RX_TRACE(...) ::rx::Log(::rx::LogLevel::kTrace, __VA_ARGS__)
#define RX_DEBUG(...) ::rx::Log(::rx::LogLevel::kDebug, __VA_ARGS__)
#define RX_INFO(...) ::rx::Log(::rx::LogLevel::kInfo, __VA_ARGS__)
#define RX_WARN(...) ::rx::Log(::rx::LogLevel::kWarn, __VA_ARGS__)
#define RX_ERROR(...) ::rx::Log(::rx::LogLevel::kError, __VA_ARGS__)

}  // namespace rx

#endif  // RX_CORE_LOG_H_
