#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "anduefker/ue/EngineVersion.hpp"
#include "anduefker/ue/ObjectModelReader.hpp"
#include "anduefker/ue/SchemaCatalog.hpp"

namespace anduefker::ue
{
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::memory::IMemorySource;

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

    struct SchemaResolutionReport
    {
        bool accepted = false;
        std::vector<std::string> evidence;
        std::vector<std::string> failures;
    };

    class SchemaResolver
    {
    public:
        SchemaResolver(const IMemorySource &memory,
                       const RuntimeBinding &binding,
                       const EngineVersion &version,
                       SchemaProbeNames names = {});

        [[nodiscard]] SchemaResolutionReport Resolve(EngineSchema &schema) const;

    private:
        [[nodiscard]] std::optional<uintptr_t> FindObjectByName(const EngineSchema &schema,
                                                                const std::string &name) const;
        [[nodiscard]] bool FindPointerField(uintptr_t first,
                                            uintptr_t expected,
                                            int32_t minOffset,
                                            int32_t maxOffset,
                                            int32_t &result) const;
        [[nodiscard]] bool FindInt32Field(uintptr_t object,
                                          int32_t expected,
                                          int32_t minOffset,
                                          int32_t maxOffset,
                                          int32_t &result) const;
        [[nodiscard]] bool ValidateUObjectSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        [[nodiscard]] bool ResolveUObjectSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        [[nodiscard]] bool ResolveStructSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        [[nodiscard]] bool ResolveFieldSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        [[nodiscard]] bool ResolvePropertySchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        [[nodiscard]] bool ResolvePropertySubtypes(EngineSchema &schema, SchemaResolutionReport &report) const;
        [[nodiscard]] bool ResolveFunctionSchema(EngineSchema &schema, SchemaResolutionReport &report) const;
        [[nodiscard]] bool ResolveEnumSchema(EngineSchema &schema, SchemaResolutionReport &report) const;

        const IMemorySource &memory_;
        const RuntimeBinding &binding_;
        EngineVersion version_;
        SchemaProbeNames names_;
    };
} // namespace anduefker::ue
