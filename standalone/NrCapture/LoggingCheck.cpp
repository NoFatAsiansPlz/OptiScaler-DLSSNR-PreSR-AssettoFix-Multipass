#include "Common.h"
#include <fstream>
#include <iostream>
namespace nr {
void LoggingCheck() {
    const auto file = std::filesystem::temp_directory_path() /
        std::format(L"nr-log-check-{}-{}.log", GetCurrentProcessId(), GetTickCount64());
    auto backup = file; backup += L".1";
    struct Cleanup {
        std::filesystem::path file, backup;
        ~Cleanup() { std::error_code e; std::filesystem::remove(file, e); std::filesystem::remove(backup, e); }
    } cleanup{file, backup};
    auto contents = [](const auto& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    };
    AppendBoundedLog(file, std::string(MaxLogBytes - 1, 'a'));
    if (std::filesystem::file_size(file) != MaxLogBytes || std::filesystem::exists(backup))
        throw std::runtime_error("Log rotated before its exact size limit.");
    AppendBoundedLog(file, "new entry");
    if (std::filesystem::file_size(backup) != MaxLogBytes || contents(file) != "new entry\n")
        throw std::runtime_error("Log rotation lost the incoming entry or previous log.");
    { std::ofstream old(file, std::ios::binary | std::ios::trunc); old << std::string(MaxLogBytes * 3, 'b') << "newest bytes"; }
    AppendBoundedLog(file, "after legacy log");
    if (std::filesystem::file_size(backup) != MaxLogBytes || !contents(backup).ends_with("newest bytes") ||
        contents(file) != "after legacy log\n") throw std::runtime_error("Oversized old log was not bounded while preserving its tail.");
    AppendBoundedLog(file, std::string(MaxLogBytes * 2, 'c'));
    if (std::filesystem::file_size(file) != MaxLogBytes || contents(backup) != "after legacy log\n")
        throw std::runtime_error("Oversized entry exceeded the log limit or retained extra generations.");
    // A failed backup must not append indefinitely or throw through rendering.
    std::filesystem::remove(backup); std::filesystem::create_directory(backup);
    AppendBoundedLog(file, "cannot rotate");
    if (std::filesystem::file_size(file) != MaxLogBytes) throw std::runtime_error("Failed rotation grew the log.");
    std::cout << "PASS 2 MiB boundary, one backup, oversized legacy log/entry, tail retention and failed-rotation bound\n";
}
}
