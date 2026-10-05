#pragma once

#include <iosfwd>
#include <optional>
#include <string_view>

#include "anduefker/ir/ReflectionIR.hpp"

namespace anduefker::generation
{
    struct ReflectionIdentity
    {
        std::string_view engine;
        std::string_view profileId;
        std::string_view profileLabel;
        std::string_view versionRange;
    };

    [[nodiscard]] std::string EscapeJson(std::string_view value);
    void WriteStatsJson(std::ostream &stream, const ir::ReflectionStats &stats, std::optional<size_t> opaqueFields = std::nullopt);
    void WriteCaptureJson(std::ostream &stream, const ir::CaptureInfo &capture);
    void WriteReflectionJson(std::ostream &stream, const ir::ReflectionIR &reflection, const ReflectionIdentity &identity);
} // namespace anduefker::generation
