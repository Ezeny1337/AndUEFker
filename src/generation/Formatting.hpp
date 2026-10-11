#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace anduefker::generation::detail
{
    inline std::string Hex(uint64_t value)
    {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::uppercase << value;
        return stream.str();
    }

} // namespace anduefker::generation::detail
