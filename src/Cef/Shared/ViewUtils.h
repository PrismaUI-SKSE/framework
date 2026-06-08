#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace PrismaUI::Cef::ViewUtils {
    inline bool TryParseViewIdFromFrameName(std::string_view frameName, std::uint64_t& viewId) {
        if (frameName.empty()) {
            return false;
        }

        std::uint64_t value = 0;
        const char* first = frameName.data();
        const char* last = first + frameName.size();
        const auto [ptr, ec] = std::from_chars(first, last, value, 10);
        if (ec != std::errc{} || ptr != last) {
            return false;
        }

        viewId = value;
        return true;
    }
}
