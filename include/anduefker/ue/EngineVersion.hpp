#pragma once

#include <cstdint>
#include <string>

namespace anduefker::ue
{
    struct EngineVersion
    {
        int major = 0;
        int minor = 0;
        int patch = 0;
        std::string source;

        [[nodiscard]] bool IsValid() const { return major > 0 && minor >= 0 && patch >= 0; }
        [[nodiscard]] int32_t Packed() const { return major * 10000 + minor * 100 + patch; }
        [[nodiscard]] std::string ToString() const;
    };

    [[nodiscard]] EngineVersion ParseEngineVersion(std::string text);
} // namespace anduefker::ue
