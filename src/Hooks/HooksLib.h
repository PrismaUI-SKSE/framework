#pragma once

namespace Hooks {
    struct UpdateHook {
        using FuncDefinition = void(RE::Main*, float);
        static constexpr auto Id = REL::RelocationID(35551, 36544);
        static constexpr auto Offset = REL::VariantOffset(0x11F, 0x160, 0x11F);
        static constexpr auto Type = HookType::WriteCall;
    };

    struct LateUpdateHook {
        using FuncDefinition = void();
        static constexpr auto Id = REL::RelocationID(35565, 36564);
        static constexpr auto Offset = REL::VariantOffset(0x748, 0xC26, 0x7EE);
        static constexpr auto Type = HookType::WriteCall;
    };

    struct D3DInitHook {
        using FuncDefinition = void();
        static constexpr auto Id = REL::RelocationID(75595, 77226);
        static constexpr auto Offset = REL::VariantOffset(0x50, 0x2BC, 0x00);  // VR unknown
        static constexpr auto Type = HookType::WriteCall;
    };

    struct D3DPresentHook {
        using FuncDefinition = void(std::uint32_t);
        static constexpr auto Id = REL::RelocationID(75461, 77246);
        static constexpr auto Offset = REL::VariantOffset(0x9, 0x9, 0x15);
        static constexpr auto Type = HookType::WriteCall;
    };

    struct RendererBegin {
        using FuncDefinition = void(RE::BSGraphics::Renderer*, std::uint32_t);
        static constexpr auto Id = REL::RelocationID(75460, 77245);
        static constexpr auto Offset = REL::VariantOffset(0, 0, 0);
        static constexpr auto Type = HookType::Direct;
    };
}
