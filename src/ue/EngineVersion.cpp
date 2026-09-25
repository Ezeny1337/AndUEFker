#include "anduefker/ue/EngineVersion.hpp"

#include <cctype>

namespace anduefker::ue
{
    std::string EngineVersion::ToString() const
    {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    EngineVersion ParseEngineVersion(std::string text)
    {
        EngineVersion result;
        result.source = text;

        size_t cursor = 0;
        auto readPart = [&]() -> int
        {
            while (cursor < text.size() && !std::isdigit(static_cast<unsigned char>(text[cursor])))
                ++cursor;
            int value = 0;
            bool found = false;
            while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])))
            {
                found = true;
                value = value * 10 + (text[cursor] - '0');
                ++cursor;
            }
            return found ? value : -1;
        };

        result.major = readPart();
        result.minor = readPart();
        result.patch = readPart();
        if (result.major < 0 || result.minor < 0)
            return {};
        if (result.patch < 0)
            result.patch = 0;
        return result;
    }
} // namespace anduefker::ue
