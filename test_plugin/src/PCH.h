#pragma once

#pragma warning(push)
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>

#include <spdlog/sinks/basic_file_sink.h>
#pragma warning(pop)

using namespace std::literals;

namespace logger = SKSE::log;

#define DLLEXPORT __declspec(dllexport)
