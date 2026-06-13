#pragma once

namespace Hooks {
    struct D3DPresentHook {
        using D3DPresentFunc = void __fastcall(std::uint32_t);
        static constexpr auto id = REL::RelocationID(75461, 77246);
        static constexpr auto offset = REL::VariantOffset(0x9, 0x9, 0x15);
        static std::uintptr_t Install(D3DPresentFunc* func);
    };
}