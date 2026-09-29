#pragma once
#include "Common.h"
// Route the shared NR compatibility loader's diagnostics to the standalone log.
#define LOG_INFO(...) nr::Log(std::format(__VA_ARGS__))
#define LOG_WARN(...) nr::Log(std::format(__VA_ARGS__))
#define LOG_ERROR(...) nr::Log(std::format(__VA_ARGS__))
