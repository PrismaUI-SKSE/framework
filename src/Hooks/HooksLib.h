#pragma once

namespace Hooks {
    struct UpdateHook {
        using FuncDefinition = void(RE::Main*, float);
        static constexpr auto Id = REL::RelocationID(35551, 36544);
        static constexpr auto Offset = REL::VariantOffset(0x11F, 0x160, 0x11F);
    };

    struct D3DInitHook {
        using FuncDefinition = void();
        static constexpr auto Id = REL::RelocationID(75595, 77226);
        static constexpr auto Offset = REL::VariantOffset(0x50, 0x2BC, 0x00);  // VR unknown
    };

    struct D3DPresentHook {
        using FuncDefinition = void __fastcall(std::uint32_t);
        static constexpr auto Id = REL::RelocationID(75461, 77246);
        static constexpr auto Offset = REL::VariantOffset(0x9, 0x9, 0x15);
    };
}
