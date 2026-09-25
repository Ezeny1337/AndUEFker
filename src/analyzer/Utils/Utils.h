#pragma once

#include <cstdint>

namespace Utils
{
    namespace Memory
    {
        inline uint64_t WrapAddress(uint64_t address)
        {
            return static_cast<uint64_t>(static_cast<uintptr_t>(address));
        }
    } // namespace Memory
} // namespace Utils
