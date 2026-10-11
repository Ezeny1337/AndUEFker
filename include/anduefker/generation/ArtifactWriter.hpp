#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "anduefker/ir/ReflectionIR.hpp"
#include "anduefker/ir/ReflectionLayout.hpp"
#include "anduefker/app/RuntimeContext.hpp"
#include "anduefker/generation/CppTypeResolver.hpp"

namespace anduefker::generation
{
    using ::anduefker::app::RuntimeContext;
    using ::anduefker::ir::EnumIR;
    using ::anduefker::ir::EnumUnderlyingType;
    using ::anduefker::ir::FunctionIR;
    using ::anduefker::ir::LayoutIssueKind;
    using ::anduefker::ir::LayoutRepresentation;
    using ::anduefker::ir::ParseStatus;
    using ::anduefker::ir::PropertyIR;
    using ::anduefker::ir::PropertyKind;
    using ::anduefker::ir::ReflectionIR;
    using ::anduefker::ir::ReflectionStats;
    using ::anduefker::ir::TypeIR;

    struct ArtifactProvenance
    {
        std::string producerCommit = "unknown";
        std::string producerVersion = "unknown";
        std::string runId;
        std::string producerWorktree = "unknown";
        int32_t targetPid = -1;
        uint64_t addressSpaceGeneration = 0;
    };

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
        std::vector<std::string> generationDiagnostics;
    };

    class ArtifactWriter
    {
    public:
        ArtifactWriter(const RuntimeContext &context,
                       const ReflectionIR &reflection,
                       std::filesystem::path outputRoot,
                       std::string packageName,
                       ArtifactProvenance provenance = {},
                       std::function<void(const std::string &)> diagnostic = {});

        [[nodiscard]] ArtifactResult Write() const;

    private:
        struct GenerationOwner
        {
            uintptr_t address = 0;
            std::string fullName;
            std::string scope;
            uintptr_t superAddress = 0;
            int32_t size = 0;
            int32_t baseSize = -1;
            int32_t initialCursor = 0;
        };
        struct LayoutEvent
        {
            std::string severity;
            std::string category;
            std::string message;
            uintptr_t ownerAddress = 0;
            const PropertyIR *property = nullptr;
            uintptr_t conflictingAddress = 0;
            std::string conflictingProperty;
            int32_t cursor = 0;
            std::string cursorSource;
            std::string strategy;
            std::optional<size_t> layoutIssueIndex;
        };
        struct OpaqueField
        {
            uintptr_t ownerAddress = 0;
            const PropertyIR *property = nullptr;
            std::string reason;
            std::string failurePath;
            const ::anduefker::ir::TypeReferenceIR *failureNode = nullptr;
        };
        struct GenerationReport
        {
            size_t opaqueFields = 0;
            size_t omittedFields = 0;
            size_t layoutWarnings = 0;
            size_t layoutEvents = 0;
            std::map<std::string, size_t> opaqueReasons;
            std::map<PropertyStorageKind, size_t> storageKinds;
            std::map<std::pair<std::string, std::string>, size_t> cppRepresentationFailures;
            std::function<void(const std::string &)> diagnostic;
            std::map<uintptr_t, GenerationOwner> owners;
            std::map<std::string, size_t> counts;
            // 非所有权属性视图在 const IR 导出期间保持有效
            std::vector<OpaqueField> opaqueDetails;
            std::vector<LayoutEvent> events;
            FieldDescriptions fields;
            std::map<std::string, size_t> typeIds;
            std::set<size_t> loggedTypeIds;
            mutable std::set<uintptr_t> loggedOwners;
            std::map<std::string, size_t> opaqueLogSamples;
            std::map<std::string, size_t> opaqueLogOmitted;
            [[nodiscard]] ParseStatus Status() const
            {
                // 有意设计的 Opaque storage 并不是缺失的字段
                return omittedFields == 0 && layoutWarnings == 0 ? ParseStatus::Complete : ParseStatus::Partial;
            }
            void RegisterOwner(GenerationOwner owner);
            void LogOwner(uintptr_t address) const;
            void RecordOpaque(OpaqueField field);
            void Warn(LayoutEvent event)
            {
                event.severity = "error";
                ++layoutEvents;
                ++layoutWarnings;
                Record(event);
                ++counts[event.category];
                events.push_back(std::move(event));
            }
            void Warn(std::string category, std::string message)
            {
                RegisterOwner({0, "<generation>", "generation", 0, 0, -1, 0});
                LayoutEvent event;
                event.category = std::move(category);
                event.message = std::move(message);
                event.strategy = "unchanged-description";
                Warn(std::move(event));
            }
            void Info(LayoutEvent event)
            {
                event.severity = "info";
                ++layoutEvents;
                Record(event);
                ++counts[event.category];
                events.push_back(std::move(event));
            }
            void Record(const LayoutEvent &event) const;
        };
        using FieldGenerationEntry = FieldDescription;
        struct FieldGenerationPlan
        {
            LayoutRepresentation representation = LayoutRepresentation::SequentialMembers;
            bool declarationDependencyBlocked = false;
            int32_t initialOffset = 0;
            int32_t size = 0;
            std::vector<FieldGenerationEntry> fields;
        };
        [[nodiscard]] std::string ManifestJson(const GenerationReport &report, ParseStatus status) const;
        [[nodiscard]] std::string DiagnosticsJson(const GenerationReport &report, ParseStatus status) const;
        void ReflectionJson(std::ostream &stream, const GenerationReport &report, const CppSymbols &symbols) const;
        [[nodiscard]] std::string RuntimeJson() const;
        void BasicTypes(std::ostream &stream, const CppSymbols &symbols) const;
        void Types(std::ostream &stream, const CppSymbols &symbols, GenerationReport &report) const;
        void Enums(std::ostream &stream, const CppSymbols &symbols) const;
        void Functions(std::ostream &stream, const CppSymbols &symbols, GenerationReport &report) const;
        [[nodiscard]] FieldGenerationPlan BuildFieldGenerationPlan(const TypeIR &owner,
                                                                   const FunctionIR *function,
                                                                   const std::vector<PropertyIR> &properties,
                                                                   int32_t size,
                                                                   const CppSymbols &symbols) const;
        void WriteFields(std::ostream &stream,
                         const TypeIR &owner,
                         const FunctionIR *function,
                         const FieldGenerationPlan &plan,
                         const CppSymbols &symbols,
                         GenerationReport &report) const;

        const RuntimeContext &context_;
        const ReflectionIR &reflection_;
        std::filesystem::path outputRoot_;
        std::string packageName_;
        ArtifactProvenance provenance_;
        std::function<void(const std::string &)> diagnostic_;
    };
} // namespace anduefker::generation
