#pragma once

#include <cstdint>
#include <string>

namespace roster
{
    // Start the background roster fetcher (once per process). Safe to call
    // repeatedly. Refetches every 20s from the master.
    void Start();

    // Phone (as prefix*10000+suffix) for an account name, or 0 if unknown.
    std::uint32_t PhoneForName(const std::string &name);
}
