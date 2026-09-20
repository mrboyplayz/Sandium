#include "ItemTypeExpansion.hpp"

#include "Addresses.hpp"
#include "ItemTypeRelocationData.hpp"
#include "structs/ItemType.hpp"

#include <cstring>
#include <limits>

#if _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace itemtypes
{
    namespace
    {
        // This instruction uses the table address as the exclusive end of the
        // unrelated 12-byte-stride array immediately preceding ItemTypes.
        // It is an address xref, but not an ItemType table access.
        constexpr std::uint32_t precedingArrayEndReferenceRva = 0x128C26;
    }

    bool Install(std::uintptr_t executableBase)
    {
#if !_WIN32
        (void)executableBase;
        return false;
#else
        static bool installed = false;
        if (installed)
            return true;

        constexpr std::uintptr_t originalTableRva = 0x42A7E180;
        constexpr std::uintptr_t expandedTableRva = 0x75000000;
        constexpr std::uintptr_t textRva = 0x1000;
        constexpr std::size_t textSize = 0x26CDF7;
        constexpr std::size_t entrySize = sizeof(structs::ItemType);
        static_assert(entrySize == 5072, "ItemType layout changed");

        const auto originalTable = executableBase + originalTableRva;
        const auto requestedTable = executableBase + expandedTableRva;
        const std::size_t expandedBytes = ExpandedCount * entrySize;
        void *allocation = VirtualAlloc(reinterpret_cast<void *>(requestedTable), expandedBytes,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (allocation != reinterpret_cast<void *>(requestedTable))
        {
            if (allocation)
                VirtualFree(allocation, 0, MEM_RELEASE);
            return false;
        }

        // Refuse to patch an executable whose instructions do not still point
        // at the IDA-verified 0.38f table. This also protects modified builds.
        for (const auto &patch : detail::referencePatches)
        {
            if (patch.instructionRva == precedingArrayEndReferenceRva)
                continue;
            const auto instruction = executableBase + patch.instructionRva;
            std::int32_t oldDisplacement = 0;
            std::memcpy(&oldDisplacement,
                        reinterpret_cast<const void *>(instruction + patch.displacementOffset),
                        sizeof(oldDisplacement));
            const auto expected = originalTable + patch.targetOffset;
            const auto actual = patch.ripRelative
                ? static_cast<std::uintptr_t>(static_cast<std::intptr_t>(instruction + patch.instructionSize) + oldDisplacement)
                : executableBase + static_cast<std::uint32_t>(oldDisplacement);
            if (actual != expected)
            {
                VirtualFree(allocation, 0, MEM_RELEASE);
                return false;
            }
        }

        std::memset(allocation, 0, expandedBytes);
        std::memcpy(allocation, reinterpret_cast<const void *>(originalTable), NativeCount * entrySize);

        DWORD oldProtection = 0;
        if (!VirtualProtect(reinterpret_cast<void *>(executableBase + textRva), textSize,
                            PAGE_EXECUTE_READWRITE, &oldProtection))
        {
            VirtualFree(allocation, 0, MEM_RELEASE);
            return false;
        }

        for (const auto &patch : detail::referencePatches)
        {
            if (patch.instructionRva == precedingArrayEndReferenceRva)
                continue;
            const auto instruction = executableBase + patch.instructionRva;
            const auto target = requestedTable + patch.targetOffset;
            const std::intptr_t displacement = patch.ripRelative
                ? static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(instruction + patch.instructionSize)
                : static_cast<std::intptr_t>(expandedTableRva + patch.targetOffset);
            if (displacement < std::numeric_limits<std::int32_t>::min() ||
                displacement > std::numeric_limits<std::int32_t>::max())
            {
                DWORD ignored = 0;
                VirtualProtect(reinterpret_cast<void *>(executableBase + textRva), textSize, oldProtection, &ignored);
                return false;
            }
            const auto value = static_cast<std::int32_t>(displacement);
            std::memcpy(reinterpret_cast<void *>(instruction + patch.displacementOffset), &value, sizeof(value));
        }

        // subhook unprotects hook entry points once when each hook is created,
        // then removes/reinstalls jumps with direct memcpy. Restoring the whole
        // text section here would make the active SetupItemTypes hook fault as
        // its scoped remover reinstalls the jump. Keep executable text writable,
        // matching the protection subhook already expects for hooked pages.
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(executableBase + textRva), textSize);
        addresses::ItemTypes.Register(requestedTable);
        installed = true;
        return true;
#endif
    }
}
