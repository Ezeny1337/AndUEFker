#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "anduefker/ir/ReflectionIR.hpp"
#include "anduefker/app/RuntimeContext.hpp"

namespace anduefker::generation
{
    using ::anduefker::app::RuntimeContext;
    using ::anduefker::ir::EnumIR;
    using ::anduefker::ir::EnumUnderlyingType;
    using ::anduefker::ir::EnumValueIR;
    using ::anduefker::ir::FunctionIR;
    using ::anduefker::ir::ParseStatus;
    using ::anduefker::ir::PropertyIR;
    using ::anduefker::ir::PropertyKind;
    using ::anduefker::ir::ReflectionIR;
    using ::anduefker::ir::ReflectionStats;
    using ::anduefker::ir::TypeIR;
    using ::anduefker::ir::TypeKind;
    using ::anduefker::ir::TypeReferenceIR;

    struct ArtifactResult
    {
        ParseStatus status = ParseStatus::Failed;
        std::filesystem::path outputPath;
        size_t filesWritten = 0;
        size_t opaqueFields = 0;
        std::string error;
    };

    class ArtifactWriter
    {
    public:
        ArtifactWriter(const RuntimeContext &context,
                       const ReflectionIR &reflection,
                       std::filesystem::path outputRoot,
                       std::string packageName);

        [[nodiscard]] ArtifactResult Write() const;

    private:
        [[nodiscard]] static std::string Sanitize(std::string value, const char *fallback);
        [[nodiscard]] static std::string JsonEscape(const std::string &value);
        [[nodiscard]] std::string ManifestJson(size_t opaqueFields) const;
        [[nodiscard]] std::string DiagnosticsJson() const;
        [[nodiscard]] std::string ReflectionJson() const;
        [[nodiscard]] std::string RuntimeJson() const;
        [[nodiscard]] std::string BasicTypes() const;
        [[nodiscard]] std::string Types(size_t &opaqueFields) const;
        [[nodiscard]] std::string Enums() const;
        [[nodiscard]] std::string Functions(size_t &opaqueFields) const;

        const RuntimeContext &context_;
        const ReflectionIR &reflection_;
        std::filesystem::path outputRoot_;
        std::string packageName_;
    };
} // namespace anduefker::generation
