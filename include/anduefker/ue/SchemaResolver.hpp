#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "anduefker/ue/ObjectModelReader.hpp"
#include "anduefker/ue/SchemaCatalog.hpp"

namespace anduefker::ue
{
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::memory::IMemorySource;

    enum class ProbeSampleState
    {
        NotObserved,
        Unreadable,
        Decoded,
        SemanticMismatch,
        SemanticMatch,
    };

    struct SchemaProbeNames
    {
        std::string objectClass = "Object";
        std::string classClass = "Class";
        std::string structClass = "Struct";
        std::string fieldClass = "Field";
        std::string guidStruct = "Guid";
        std::string colorStruct = "Color";
        std::string vectorStruct = "Vector";
    };

    struct SchemaProbeBootstrap
    {
        std::shared_ptr<const ObjectStoreReader> objects;
        std::vector<std::pair<int32_t, uintptr_t>> objectSamples;
        std::string failure;

        [[nodiscard]] bool IsValid() const
        {
            return objects != nullptr && objectSamples.size() >= 2;
        }
    };

    [[nodiscard]] std::shared_ptr<const SchemaProbeBootstrap> CreateSchemaProbeBootstrap(
        const IMemorySource &memory,
        const RuntimeBinding &binding,
        size_t maxSamples = 256);

    struct SchemaResolutionReport
    {
        bool accepted = false;
        bool addressSpaceChanged = false;
        int32_t score = 0;
        std::string profileId;
        std::string profileLabel;
        std::string failureStage;
        std::vector<std::string> evidence;
        std::vector<std::string> failures;
    };

    using SchemaCandidateSummary = SchemaResolutionReport;

    struct SchemaSelectionResult
    {
        bool accepted = false;
        bool ambiguous = false;
        bool layoutAmbiguous = false;
        size_t selectedIndex = 0;
        std::vector<SchemaCandidateSummary> candidates;
    };

    [[nodiscard]] SchemaSelectionResult SelectSchemaCandidates(std::vector<SchemaCandidateSummary> candidates,
                                                               const std::vector<EngineSchema> &schemas);

    class SchemaResolver
    {
    public:
        SchemaResolver(const IMemorySource &memory,
                       const RuntimeBinding &binding,
                       const EngineProfile &profile,
                       SchemaProbeNames names = {},
                       uintptr_t moduleBase = 0,
                       uintptr_t moduleEnd = 0,
                       std::shared_ptr<const SchemaProbeBootstrap> bootstrap = {});

        [[nodiscard]] SchemaResolutionReport Resolve(EngineSchema &schema) const;

    private:
        const IMemorySource &memory_;
        const RuntimeBinding &binding_;
        EngineProfile profile_;
        SchemaProbeNames names_;
        uintptr_t moduleBase_ = 0;
        uintptr_t moduleEnd_ = 0;
        std::shared_ptr<const SchemaProbeBootstrap> bootstrap_;
    };
} // namespace anduefker::ue
