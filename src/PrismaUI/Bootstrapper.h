#pragma once

#include <expected>

namespace PrismaUI::Bootstrapper {
    std::expected<void, std::string> Initialize();
}
