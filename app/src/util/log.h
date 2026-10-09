#pragma once

#include <spdlog/spdlog.h>

// <syslog.h> (pulled in by sd-journal.h) defines LOG_INFO and LOG_DEBUG as priorities. Include this
// header after it; these then win, and nothing here needs syslog's.
#undef LOG_INFO
#undef LOG_WARN
#undef LOG_ERROR
#undef LOG_DEBUG
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#define LOG_DEBUG(...) spdlog::debug(__VA_ARGS__)

namespace btb {
void init_logging(bool verbose);
}
