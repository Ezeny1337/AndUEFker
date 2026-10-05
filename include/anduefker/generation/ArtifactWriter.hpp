#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <string>
#include <utility>

#include "anduefker/ir/ReflectionIR.hpp"
#include "anduefker/app/RuntimeContext.hpp"
#include "anduefker/generation/CppTypeResolver.hpp"

namespace anduefker::generation
{
    using ::anduefker::app::RuntimeContext;
    using ::anduefker::ir::EnumIR;
    using ::anduefker::ir::EnumUnderlyingType;
    using ::anduefker::ir::FunctionIR;
    using ::anduefker::ir::ParseStatus;
    using ::anduefker::ir::PropertyIR;
    using ::anduefker::ir::PropertyKind;
    using ::anduefker::ir::ReflectionIR;
    using ::anduefker::ir::ReflectionStats;
    using ::anduefker::ir::TypeIR;

    struct ArtifactResult
    {
        ParseStatus status = ParseStatus::Failed;
        std::filesystem::path outputPath;
        size_t filesWritten = 0;
        size_t opaqueFields = 0;
        size_t omittedFields = 0;
        size_t layoutWarnings = 0;
        ParseStatus reflectionStatus = ParseStatus::Failed;
        ParseStatus sdkStatus = ParseStatus::Failed;
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
        struct GenerationReport
        {
            size_t opaqueFields = 0;
            size_t omittedFields = 0;
            size_t layoutWarnings = 0;
            std::vector<std::string> diagnostics;
            [[nodiscard]] ParseStatus Status() const
            {
                // 不透明容器是有意的描述，而不是缺少字段
                return omittedFields == 0 && layoutWarnings == 0 ? ParseStatus::Complete : ParseStatus::Partial;
            }
            void Warn(std::string message)
            {
                ++layoutWarnings;
                if (diagnostics.size() < 32)
                    diagnostics.push_back(std::move(message));
            }
        };
        [[nodiscard]] std::string ManifestJson(const GenerationReport &report, ParseStatus status) const;
        [[nodiscard]] std::string DiagnosticsJson(const GenerationReport &report, ParseStatus status) const;
        [[nodiscard]] std::string ReflectionJson() const;
        [[nodiscard]] std::string RuntimeJson() const;
        [[nodiscard]] std::string BasicTypes() const;
        [[nodiscard]] std::string Types(const CppSymbols &symbols, GenerationReport &report) const;
        [[nodiscard]] std::string Enums(const CppSymbols &symbols) const;
        [[nodiscard]] std::string Functions(const CppSymbols &symbols, GenerationReport &report) const;
        void WriteFields(std::ostringstream &stream,
                         const std::vector<PropertyIR> &properties,
                         int32_t initialOffset,
                         int32_t size,
                         const CppSymbols &symbols,
                         GenerationReport &report) const;

        const RuntimeContext &context_;
        const ReflectionIR &reflection_;
        std::filesystem::path outputRoot_;
        std::string packageName_;
    };
} // namespace anduefker::generation
