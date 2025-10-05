#pragma once

namespace Hooks {
    struct D3DInitHook
    {
        using D3DInitFunc = void();
        static constexpr auto id = REL::RelocationID(75595, 77226);
        static constexpr auto offset = REL::VariantOffset(0x50, 0x2BC, 0x00); // VR unknown
        static std::uintptr_t Install(D3DInitFunc* func);
    };

    struct D3DPresentHook
    {
        using D3DPresentFunc = void __fastcall(std::uint32_t);
        static constexpr auto id = REL::RelocationID(75461, 77246);
        static constexpr auto offset = REL::VariantOffset(0x9, 0x9, 0x15);
        static std::uintptr_t Install(D3DPresentFunc* func);
    };
}