#include "ProbeContext.hpp"

#include <algorithm>
#include <unordered_map>
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

        schema.property.baseSize = schema.property.elementSize;
        schema.validation.properties = true;
        report.evidence.push_back("resolved property ArrayDim/ElementSize/Flags/Offset_Internal from Guid chain; array_dim=" +
                                  std::to_string(schema.property.arrayDim) + " element_size=" +
                                  std::to_string(schema.property.elementSize) + " flags=" +
                                  std::to_string(schema.property.propertyFlags) + " offset=" +
                                  std::to_string(schema.property.offsetInternal));
        return true;
    }


    bool SchemaProbeContext::ResolvePropertySubtypes(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        if (!schema.features.useFProperty)
            report.evidence.push_back("property subtype probing enabled for UProperty family");

        ObjectModelReader model(memory_, binding_, schema);
        if (!model.Initialize())
        {
            report.evidence.push_back("property subtype probing could not initialize object model");
            return true;
        }

        const std::vector<std::string> wanted = {
            "BoolProperty", "ByteProperty", "ObjectProperty", "ObjectPropertyBase", "ClassProperty",
            "StructProperty", "ArrayProperty", "SetProperty", "MapProperty", "EnumProperty",
            "DelegateProperty", "MulticastDelegateProperty", "MulticastInlineDelegateProperty",
            "MulticastSparseDelegateProperty", "OptionalProperty"};
        std::unordered_map<std::string, uintptr_t> samples;
        std::vector<uintptr_t> delegateSamples;
        const auto isDelegatePropertyName = [](FieldKind kind, const std::string &name)
        {
            const std::string normalized = NormalizeRuntimeFieldName(name);
            if (!IsPropertyFieldKind(kind))
                return false;
            return normalized == "DelegateProperty" || normalized == "MulticastDelegateProperty" ||
                   normalized == "MulticastInlineDelegateProperty" || normalized == "MulticastSparseDelegateProperty";
        };
        const ObjectStoreReader &objects = model.Objects();
        for (int32_t index = 0; index < objects.Count() && samples.size() < wanted.size(); ++index)
        {
            const auto object = objects.ReadObject(index);
            if (!object.IsValid())
                continue;
            const auto className = model.ClassName(object.address);
            if (!className || (*className != "Class" && *className != "ScriptStruct"))
                continue;
            const auto first = model.StructProperties(object.address);
            if (!first)
                continue;
            const FieldChainResult fields = model.FieldsWithStatus(*first, 2048);
            if (!fields.Complete())
            {
                report.evidence.push_back("property subtype probe field chain was incomplete; status=" +
                                          std::to_string(static_cast<int>(fields.status)));
                continue;
            }
            for (const FieldMetadata &field : fields.fields)
            {
                const std::string &propertyClassName = field.normalizedClassName;
                if (isDelegatePropertyName(field.kind, field.className) && delegateSamples.size() < 128)
                    delegateSamples.push_back(field.address);
                if (std::find(wanted.begin(), wanted.end(), propertyClassName) != wanted.end() &&
                    !samples.contains(propertyClassName))
                    samples.emplace(propertyClassName, field.address);
            }
        }

        // FProperty 自身的链表字段和 RepNotifyFunc 位于所有具体属性负载之前
        // 如果紧跟 PropertyFlags 开始探测 subtype
        // 就会把 PropertyLinkNext/NextRef 等字段误认为 Array::Inner、Set::ElementProp或 Map::KeyProp
        const int32_t fPropertyBaseTail = schema.property.offsetInternal + static_cast<int32_t>(sizeof(int32_t)) +
                                          static_cast<int32_t>(sizeof(uintptr_t) * 4) +
                                          std::max(schema.fname.size, static_cast<int32_t>(sizeof(uintptr_t)));
        const int32_t propertyTail = std::max({fPropertyBaseTail,
                                               schema.property.propertyFlags + static_cast<int32_t>(sizeof(uint64_t)),
                                               schema.ffield.name + static_cast<int32_t>(sizeof(uintptr_t)),
                                               schema.ffield.next + static_cast<int32_t>(sizeof(uintptr_t))});
        const int32_t firstSubtypeOffset = (propertyTail + static_cast<int32_t>(sizeof(uintptr_t)) - 1) /
                                           static_cast<int32_t>(sizeof(uintptr_t)) * static_cast<int32_t>(sizeof(uintptr_t));

        auto readPointer = [&](uintptr_t field, int32_t offset, uintptr_t &value)
        {
            const auto address = Add(field, offset);
            return address && memory_.Read(*address, value) && value != 0;
        };
        auto isUObjectClass = [&](uintptr_t value, const std::string &expected)
        {
            const auto className = model.ClassName(value);
            return className && (expected.empty() || *className == expected);
        };
        const auto isFunctionObject = [&](uintptr_t value)
        {
            const auto className = model.ClassName(value);
            return className && (*className == "Function" || *className == "DelegateFunction" ||
                                 *className == "SparseDelegateFunction" || *className == "VerseFunction");
        };
        auto isFieldClass = [&](uintptr_t value)
        {
            const auto field = model.Field(value);
            return field && IsPropertyFieldKind(field->kind);
        };
        auto findPointer = [&](const std::string &sampleName, const auto &predicate) -> int32_t
        {
            const auto sample = samples.find(sampleName);
            if (sample == samples.end())
                return -1;
            for (int32_t offset = firstSubtypeOffset; offset <= 0x180; offset += static_cast<int32_t>(sizeof(uintptr_t)))
            {
                uintptr_t value = 0;
                if (readPointer(sample->second, offset, value) && predicate(value))
                    return offset;
            }
            return -1;
        };

        if (schema.propertySubtypes.boolBase < 0 && samples.contains("BoolProperty"))
            schema.propertySubtypes.boolBase = firstSubtypeOffset;
        if (schema.propertySubtypes.objectClass < 0)
            schema.propertySubtypes.objectClass = findPointer("ObjectProperty", [&](uintptr_t value)
                                                              { return isUObjectClass(value, "Class"); });
        if (schema.propertySubtypes.classMetaClass < 0)
        {
            const int32_t classOffset = findPointer("ClassProperty", [&](uintptr_t value)
                                                    { return isUObjectClass(value, "Class"); });
            if (classOffset >= 0)
            {
                uintptr_t ignored = 0;
                if (readPointer(samples["ClassProperty"], classOffset + static_cast<int32_t>(sizeof(uintptr_t)), ignored) &&
                    isUObjectClass(ignored, "Class"))
                    schema.propertySubtypes.classMetaClass = classOffset + static_cast<int32_t>(sizeof(uintptr_t));
            }
        }
        if (schema.propertySubtypes.structType < 0)
            schema.propertySubtypes.structType = findPointer("StructProperty", [&](uintptr_t value)
                                                             {
            const auto className = model.ClassName(value);
            return className && (*className == "Struct" || *className == "ScriptStruct"); });
        if (schema.propertySubtypes.arrayInner < 0)
            schema.propertySubtypes.arrayInner = findPointer("ArrayProperty", isFieldClass);
        if (schema.propertySubtypes.setElement < 0)
            schema.propertySubtypes.setElement = findPointer("SetProperty", isFieldClass);
        if (schema.propertySubtypes.mapBase < 0)
        {
            const int32_t keyOffset = findPointer("MapProperty", isFieldClass);
            if (keyOffset >= 0)
            {
                uintptr_t value = 0;
                if (readPointer(samples["MapProperty"], keyOffset + static_cast<int32_t>(sizeof(uintptr_t)), value) &&
                    isFieldClass(value))
                    schema.propertySubtypes.mapBase = keyOffset;
            }
        }
        if (schema.propertySubtypes.enumBase < 0)
        {
            const int32_t underlyingOffset = findPointer("EnumProperty", isFieldClass);
            if (underlyingOffset >= 0)
            {
                uintptr_t value = 0;
                if (readPointer(samples["EnumProperty"], underlyingOffset + static_cast<int32_t>(sizeof(uintptr_t)), value) &&
                    isUObjectClass(value, "Enum"))
                    schema.propertySubtypes.enumBase = underlyingOffset;
            }
        }
        if (schema.propertySubtypes.delegateSignature < 0)
        {
            const auto readProbePointer = [&](uintptr_t field, int32_t offset, uintptr_t &value)
            {
                const auto address = Add(field, offset);
                return address && memory_.IsReadable(*address, sizeof(uintptr_t)) && memory_.Read(*address, value) && value != 0;
            };
            // 在所有的 Delegate 属性变体中，UE 5.6 都将 SignatureFunction 声明在紧跟 FProperty 基类之后的位置
            // 因此，通过源码推导出的偏移量是我们在此时所需的唯一候选值
            // 扫描后续的其他任意字段不仅会产生误报，还会导致不必要的远程读取失败
            const int32_t offset = firstSubtypeOffset;
            size_t nonZeroHits = 0;
            size_t readableHits = 0;
            size_t objectClassHits = 0;
            size_t functionHits = 0;
            for (uintptr_t sample : delegateSamples)
            {
                uintptr_t value = 0;
                if (!readProbePointer(sample, offset, value))
                    continue;
                ++nonZeroHits;
                if (!IsReadablePointer(memory_, value))
                    continue;
                ++readableHits;
                if (model.ClassName(value))
                    ++objectClassHits;
                if (isFunctionObject(value))
                    ++functionHits;
            }
            report.evidence.push_back("delegate subtype samples=" + std::to_string(delegateSamples.size()) +
                                      " source_offset=" + std::to_string(offset) +
                                      " nonzero=" + std::to_string(nonZeroHits) +
                                      " readable=" + std::to_string(readableHits) +
                                      " object_class=" + std::to_string(objectClassHits) +
                                      " function_hits=" + std::to_string(functionHits));
            if (functionHits >= 4)
                schema.propertySubtypes.delegateSignature = offset;
        }
        if (schema.propertySubtypes.optionalValue < 0)
            schema.propertySubtypes.optionalValue = findPointer("OptionalProperty", isFieldClass);
        if (schema.propertySubtypes.byteEnum < 0)
            schema.propertySubtypes.byteEnum = findPointer("ByteProperty", [&](uintptr_t value)
                                                           { return isUObjectClass(value, "Enum"); });

        report.evidence.push_back("property subtypes: object_class=" + std::to_string(schema.propertySubtypes.objectClass) +
                                  " class_meta_class=" + std::to_string(schema.propertySubtypes.classMetaClass) +
                                  " struct_type=" + std::to_string(schema.propertySubtypes.structType) +
                                  " array_inner=" + std::to_string(schema.propertySubtypes.arrayInner) +
                                  " set_element=" + std::to_string(schema.propertySubtypes.setElement) +
                                  " map_base=" + std::to_string(schema.propertySubtypes.mapBase) +
                                  " enum_base=" + std::to_string(schema.propertySubtypes.enumBase) +
                                  " delegate_signature=" + std::to_string(schema.propertySubtypes.delegateSignature) +
                                  " optional_value=" + std::to_string(schema.propertySubtypes.optionalValue));
        return true;
    }
} // namespace anduefker::ue::schema_probe
