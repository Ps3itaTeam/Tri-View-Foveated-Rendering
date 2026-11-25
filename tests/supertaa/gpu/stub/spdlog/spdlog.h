// test stub of spdlog: formats with std::format and prints
#pragma once
#include <cstdio>
#include <format>
#include <string>
namespace spdlog {
template <typename... Args> void info(std::format_string<Args...> f, Args&&... args) { std::printf("[info] %s\n", std::format(f, std::forward<Args>(args)...).c_str()); }
template <typename... Args> void warn(std::format_string<Args...> f, Args&&... args) { std::printf("[warn] %s\n", std::format(f, std::forward<Args>(args)...).c_str()); }
template <typename... Args> void error(std::format_string<Args...> f, Args&&... args) { std::printf("[error] %s\n", std::format(f, std::forward<Args>(args)...).c_str()); }
}
