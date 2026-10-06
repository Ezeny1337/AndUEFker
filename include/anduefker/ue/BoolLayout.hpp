#pragma once

#include <array>
#include <cstdint>

namespace anduefker::ue
{
    inline bool IsValidBoolLayout(const std::array<uint8_t, 4> &layout, int32_t elementSize)
    {
        return (layout[0] == 1 || layout[0] == 2 || layout[0] == 4 || layout[0] == 8) &&
               layout[0] == elementSize && layout[1] < layout[0] && layout[2] != 0 &&
               (layout[2] & (layout[2] - 1)) == 0 &&
               (layout[3] == layout[2] || (layout[3] == 0xFF && layout[2] == 1 && layout[1] == 0));
    }
} // namespace anduefker::ue
