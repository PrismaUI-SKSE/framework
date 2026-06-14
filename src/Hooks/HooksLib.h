#pragma once

namespace Hooks {
    struct UpdateHook {
        using FuncDefinition = void(RE::Main*, float);
        static constexpr auto Id = REL::RelocationID(35551, 36544);
        static constexpr auto Offset = REL::VariantOffset(0x11F, 0x160, 0x11F);
    };

    struct InnerUpdateHook {
        using FuncDefinition = void();
        static constexpr auto Id = REL::RelocationID(35565, 36564);
        static constexpr auto Offset = REL::VariantOffset(0x748, 0xC26, 0x7EE);
    };

    struct D3DPresentHook {
        using FuncDefinition = void __fastcall(std::uint32_t);
        static constexpr auto Id = REL::RelocationID(75461, 77246);
        static constexpr auto Offset = REL::VariantOffset(0x9, 0x9, 0x15);
    };
}
