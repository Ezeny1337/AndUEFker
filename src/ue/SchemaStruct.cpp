#include "ProbeContext.hpp"

#include <array>

namespace anduefker::ue::schema_probe
{
    bool SchemaProbeContext::ResolveStructSchema(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        const auto guid = FindObjectByName(schema, names_.guidStruct);
        const auto color = FindObjectByName(schema, names_.colorStruct);
        const auto vector = FindObjectByName(schema, names_.vectorStruct);
        if (!guid || !color || !vector)
        {
            report.failures.push_back("reflection samples unavailable: Guid=" + std::string(guid ? "yes" : "no") +
                                      " Color=" + std::string(color ? "yes" : "no") +
                                      " Vector=" + std::string(vector ? "yes" : "no"));
            return false;
        }

        // FVector 的大小由 FLargeWorldCoordinatesReal 决定，而该类型受构建配置影响不能只根据 UE 主版本决定
        // 优先尝试 profile 的预期值，同时保留另一种 ABI变体，避免把 LWC 当成版本事实
        const std::array<int32_t, 2> vectorSizes = schema.features.largeWorldCoordinates
                                                       ? std::array<int32_t, 2>{0x18, 0x0C}
                                                       : std::array<int32_t, 2>{0x0C, 0x18};
        struct StructSizeCandidate
        {
            int32_t offset = -1;
            int32_t vectorSize = 0;
        };
        std::vector<StructSizeCandidate> sizeCandidates;
        for (int32_t offset = 0; offset <= 0x100 - 4; offset += 4)
        {
            for (const int32_t vectorSize : vectorSizes)
            {
                const std::array<std::pair<uintptr_t, int32_t>, 3> knownSizes = {
                    std::pair{*guid, 0x10}, std::pair{*color, 0x04}, std::pair{*vector, vectorSize}};
                bool matches = true;
                for (const auto &[object, expected] : knownSizes)
                {
                    int32_t actual = 0;
                    const auto address = Add(object, offset);
                    if (!address || !memory_.Read(*address, actual) || actual != expected)
                    {
                        matches = false;
                        break;
                    }
                }
                if (matches)
                    sizeCandidates.push_back({offset, vectorSize});
            }
        }
        if (!sizeCandidates.empty())
        {
            if (sizeCandidates.size() > 1)
            {
                report.ambiguous = true;
                report.failures.push_back("UStruct::PropertiesSize candidates are ambiguous across FVector layouts; candidates=" +
                                          std::to_string(sizeCandidates.size()));
                return false;
            }
            schema.ustruct.propertiesSizeOffset = sizeCandidates.front().offset;
            schema.features.largeWorldCoordinates = sizeCandidates.front().vectorSize == 0x18;
            report.evidence.push_back("resolved UStruct::PropertiesSize; offset=" +
                                      std::to_string(schema.ustruct.propertiesSizeOffset) +
                                      " guid_size=16 color_size=4 vector_size=" +
                                      std::to_string(sizeCandidates.front().vectorSize) +
                                      " candidates=" + std::to_string(sizeCandidates.size()));
            report.versionEvidence.push_back({"large-world-coordinates",
                                              sizeCandidates.front().vectorSize == 0x18 ? "FVector.size=24" : "FVector.size=12",
                                              VersionEvidenceStrength::Strong,
                                              "observed from Guid/Color/Vector UStruct::PropertiesSize samples"});
        }
        if (schema.ustruct.propertiesSizeOffset < 0)
        {
            report.failures.push_back("UStruct::PropertiesSize was not resolved from Guid/Color/Vector size variants");
            return false;
        }

        const auto structObject = FindObjectByName(schema, names_.structClass);
        const auto fieldObject = FindObjectByName(schema, names_.fieldClass);
        if (structObject && fieldObject &&
            FindPointerField(*structObject, *fieldObject, static_cast<int32_t>(sizeof(uintptr_t)), 0x100, schema.ustruct.superStruct))
        {
            report.evidence.push_back("resolved UStruct::SuperStruct from Struct -> Field relation; PropertiesSize offset=" +
                                      std::to_string(schema.ustruct.propertiesSizeOffset));
        }
        else
        {
            report.failures.push_back("UStruct::SuperStruct was not resolved");
            return false;
        }

        // 不对单个指针形状的字段进行推断来确定 Children/ChildProperties 和 MinAlignment
        // 它们的链表语义（linked-list semantics）将在下一个属性族（property-family）阶段中进行解析
        schema.ustruct.children = -1;
        schema.ustruct.childProperties = -1;
        schema.ustruct.minAlignment = -1;
        schema.validation.structs = true;
        report.evidence.push_back(schema.features.useFProperty ? "FProperty struct schema selected" : "UProperty struct schema selected");
        return true;
    }
} // namespace anduefker::ue::schema_probe
