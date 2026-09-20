#pragma once

#include <cstddef>
#include <cstdint>

namespace itemtypes
{
    inline constexpr std::size_t NativeCount = 46;
    inline constexpr std::size_t ExpandedCount = 64; // six-bit network field

    // Relocate Sub Rosa's fixed item-type table and patch every IDA-verified
    // code reference to the expanded allocation. Windows 0.38f client only.
    bool Install(std::uintptr_t executableBase);
}
