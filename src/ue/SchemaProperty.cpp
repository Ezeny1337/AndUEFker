#include "ProbeContext.hpp"

#include <algorithm>
#include <unordered_set>

namespace anduefker::ue::schema_probe
{
    bool SchemaProbeContext::ResolvePropertySchema(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        const auto guid = FindObjectByName(schema, names_.guidStruct);
        if (!guid)
        {
            report.failures.push_back("Guid sample is unavailable for property schema");
            return false;
        }
        NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                              schema.fname, schema.features);

        const int32_t firstOffset = schema.features.useFProperty ? schema.ustruct.childProperties : schema.ustruct.children;
        const int32_t nextOffset = schema.features.useFProperty ? schema.ffield.next : schema.ufield.next;
        const int32_t nameOffset = schema.features.useFProperty ? schema.ffield.name : schema.uobject.name;
        const auto firstAddress = Add(*guid, firstOffset);
        if (!firstAddress)
            return false;
        uintptr_t current = 0;
        if (!memory_.Read(*firstAddress, current) || current == 0)
            return false;

        std::vector<std::pair<uintptr_t, int32_t>> properties;
        std::unordered_set<uintptr_t> visited;
        while (current != 0 && properties.size() < 32 && visited.insert(current).second)
        {
            const auto nameAddress = Add(current, nameOffset);
            if (!nameAddress)
                break;
            int32_t rawName = 0;
            if (!memory_.Read(*nameAddress, rawName))
                break;
            rawName = binding_.decode.nameIndex(rawName, *nameAddress);
            const auto propertyName = names.ReadName(rawName);
            if (!propertyName)
                break;
            if (*propertyName == "A" || *propertyName == "B" || *propertyName == "C" || *propertyName == "D")
            {
                const int32_t expectedOffset = (*propertyName == "A" ? 0 : *propertyName == "B" ? 4
                                                                       : *propertyName == "C"   ? 8
                                                                                                : 12);
                properties.emplace_back(current, expectedOffset);
            }
            const auto nextAddressValue = Add(current, nextOffset);
            if (!nextAddressValue || !memory_.Read(*nextAddressValue, current))
                break;
        }
        if (properties.size() < 4)
        {
            report.failures.push_back("Guid property chain did not expose A/B/C/D");
            return false;
        }

        const auto matchesUniform = [&](int32_t offset, int32_t expected)
        {
            for (const auto &[property, ignored] : properties)
            {
                (void)ignored;
                int32_t value = 0;
                const auto address = Add(property, offset);
                if (!address || !memory_.Read(*address, value) || value != expected)
                    return false;
            }
            return true;
        };

        const int32_t pointerSize = static_cast<int32_t>(sizeof(uintptr_t));
        const int32_t fieldTail = schema.features.useFProperty
                                      ? schema.ffield.name + schema.fname.size + static_cast<int32_t>(sizeof(uint32_t))
                                      : schema.ufield.next + pointerSize;
        const int32_t propertyStart = ((fieldTail + pointerSize - 1) / pointerSize) * pointerSize;

        struct PropertyHeaderCandidate
        {
            int32_t arrayDim = -1;
            int32_t elementSize = -1;
            bool sourceLayout = false;
        };
        std::vector<PropertyHeaderCandidate> headerCandidates;
        const int32_t candidateStart = std::max<int32_t>(0, propertyStart - pointerSize);
        for (int32_t arrayDimOffset = candidateStart; arrayDimOffset <= 0xA0 - static_cast<int32_t>(sizeof(int32_t) * 2);
             arrayDimOffset += static_cast<int32_t>(sizeof(int32_t)))
        {
            const int32_t elementSizeOffset = arrayDimOffset + static_cast<int32_t>(sizeof(int32_t));
            if (!matchesUniform(arrayDimOffset, 1) || !matchesUniform(elementSizeOffset, 4))
                continue;
            headerCandidates.push_back({arrayDimOffset, elementSizeOffset, arrayDimOffset == propertyStart});
        }

        if (headerCandidates.empty())
        {
            report.failures.push_back("FProperty::ArrayDim/ElementSize candidates were rejected; source_offset=" +
                                      std::to_string(propertyStart) + " scan_start=" +
                                      std::to_string(candidateStart) + " candidates=0");
            return false;
        }

        struct PropertyTailCandidate
        {
            PropertyHeaderCandidate header;
            int32_t propertyFlags = -1;
            int32_t offsetInternal = -1;
            size_t exactOffsetHits = 0;
            size_t plausibleOffsetHits = 0;
            bool sourceFlagsLayout = false;
        };
        std::vector<PropertyTailCandidate> tailCandidates;
        PropertyTailCandidate bestNearCandidate;
        bool haveNearCandidate = false;
        // EPropertyFlags 位于 ArrayDim 和 ElementSize 之后，类型为 uint64_t
        // 构建配置或字段扩展可能在 flags 与 Offset_Internal 之间插入成员
        // 遍历完整的有界尾部候选，而不是把 Offset_Internal 固定在某一个 delta 上
        for (const PropertyHeaderCandidate &header : headerCandidates)
        {
            const int32_t flagsAlignment = static_cast<int32_t>(alignof(uint64_t));
            const int32_t flagsStart = header.elementSize + static_cast<int32_t>(sizeof(int32_t));
            const int32_t sourceFlagsOffset = (flagsStart + flagsAlignment - 1) / flagsAlignment * flagsAlignment;
            for (int32_t flagsOffset = header.elementSize + static_cast<int32_t>(sizeof(int32_t));
                 flagsOffset <= 0x100 - static_cast<int32_t>(sizeof(uint64_t)); flagsOffset += 4)
            {
                bool flagsValid = true;
                for (const auto &[property, ignored] : properties)
                {
                    (void)ignored;
                    uint64_t flags = 0;
                    const auto address = Add(property, flagsOffset);
                    if (!address || !memory_.Read(*address, flags) || flags == 0 ||
                        (flags & 0xE000000000000000ull) != 0 ||
                        (flags & 0xFFFFFFFFull) == 0xCDCDCDCDull)
                    {
                        flagsValid = false;
                        break;
                    }
                }
                if (!flagsValid)
                    continue;

                for (int32_t offset = flagsOffset + static_cast<int32_t>(sizeof(uint64_t));
                     offset <= 0x120 - static_cast<int32_t>(sizeof(int32_t)); offset += 4)
                {
                    size_t exactOffsetHits = 0;
                    size_t plausibleOffsetHits = 0;
                    for (const auto &[property, expected] : properties)
                    {
                        int32_t value = 0;
                        const auto address = Add(property, offset);
                        if (!address || !memory_.Read(*address, value))
                            continue;
                        if (value == expected)
                            ++exactOffsetHits;
                        if (value >= 0 && value <= 0x10000)
                            ++plausibleOffsetHits;
                    }
                    PropertyTailCandidate candidate{header, flagsOffset, offset, exactOffsetHits,
                                                    plausibleOffsetHits, flagsOffset == sourceFlagsOffset};
                    if (exactOffsetHits == properties.size())
                        tailCandidates.push_back(candidate);
                    if (!haveNearCandidate ||
                        exactOffsetHits > bestNearCandidate.exactOffsetHits ||
                        (exactOffsetHits == bestNearCandidate.exactOffsetHits &&
                         plausibleOffsetHits > bestNearCandidate.plausibleOffsetHits))
                    {
                        bestNearCandidate = candidate;
                        haveNearCandidate = true;
                    }
                }
            }
        }

        std::sort(tailCandidates.begin(), tailCandidates.end(),
                  [](const PropertyTailCandidate &left, const PropertyTailCandidate &right)
                  {
                      if (left.header.sourceLayout != right.header.sourceLayout)
                          return left.header.sourceLayout;
                      if (left.sourceFlagsLayout != right.sourceFlagsLayout)
                          return left.sourceFlagsLayout;
                      if (left.exactOffsetHits != right.exactOffsetHits)
                          return left.exactOffsetHits > right.exactOffsetHits;
                      return left.propertyFlags < right.propertyFlags;
                  });
        if (tailCandidates.empty())
        {
            std::string nearSummary = " none";
            if (haveNearCandidate)
                nearSummary = " header_array_dim=" + std::to_string(bestNearCandidate.header.arrayDim) +
                              " header_element_size=" + std::to_string(bestNearCandidate.header.elementSize) +
                              " flags=" + std::to_string(bestNearCandidate.propertyFlags) +
                              " offset=" + std::to_string(bestNearCandidate.offsetInternal) +
                              " exact_hits=" + std::to_string(bestNearCandidate.exactOffsetHits) +
                              " plausible_hits=" + std::to_string(bestNearCandidate.plausibleOffsetHits);
            report.failures.push_back("FProperty::Offset_Internal was not resolved after PropertyFlags; headers=" +
                                      std::to_string(headerCandidates.size()) + " tail_candidates=0 near=" +
                                      nearSummary);
            return false;
        }
        // 相同源码布局优先级下，等价语义证据无法唯一确定偏移。
        if (tailCandidates.size() > 1 &&
            tailCandidates[0].header.sourceLayout == tailCandidates[1].header.sourceLayout &&
            tailCandidates[0].sourceFlagsLayout == tailCandidates[1].sourceFlagsLayout)
        {
            report.failures.push_back("property layout candidates are ambiguous; candidates=" +
                                      std::to_string(tailCandidates.size()) + " first_flags=" +
                                      std::to_string(tailCandidates[0].propertyFlags) + " first_offset=" +
                                      std::to_string(tailCandidates[0].offsetInternal) + " second_flags=" +
                                      std::to_string(tailCandidates[1].propertyFlags) + " second_offset=" +
                                      std::to_string(tailCandidates[1].offsetInternal));
            return false;
        }
        const PropertyTailCandidate &selectedTail = tailCandidates.front();
        schema.property.arrayDim = selectedTail.header.arrayDim;
        schema.property.elementSize = selectedTail.header.elementSize;
        schema.property.propertyFlags = selectedTail.propertyFlags;
        schema.property.offsetInternal = selectedTail.offsetInternal;
        report.evidence.push_back("resolved " + std::string(schema.features.useFProperty ? "FProperty" : "UProperty") +
                                  " layout candidate; source_offset=" +
                                  std::to_string(propertyStart) + " selected_array_dim=" +
                                  std::to_string(schema.property.arrayDim) + " selected_element_size=" +
                                  std::to_string(schema.property.elementSize) + " flags=" +
                                  std::to_string(schema.property.propertyFlags) + " offset=" +
                                  std::to_string(schema.property.offsetInternal) + " headers=" +
                                  std::to_string(headerCandidates.size()) + " source_flags_layout=" +
                                  std::to_string(selectedTail.sourceFlagsLayout) + " tail_candidates=" +
                                  std::to_string(tailCandidates.size()));

        schema.property.baseSize = -1;
        schema.validation.properties = true;
        report.evidence.push_back("resolved property ArrayDim/ElementSize/Flags/Offset_Internal from Guid chain; array_dim=" +
                                  std::to_string(schema.property.arrayDim) + " element_size=" +
                                  std::to_string(schema.property.elementSize) + " flags=" +
                                  std::to_string(schema.property.propertyFlags) + " offset=" +
                                  std::to_string(schema.property.offsetInternal));
        return true;
    }

} // namespace anduefker::ue::schema_probe
