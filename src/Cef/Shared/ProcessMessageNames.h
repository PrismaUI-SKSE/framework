#pragma once

#include "BrowserToRendererMessages.h"
#include "RendererToBrowserMessages.h"

// Shared renderer/browser bridge names that are not process-message names.

namespace PrismaUI::Cef::Messages {
    inline constexpr const char* kImeFocusListener = "__prismaNativeImeFocusChanged";
}
