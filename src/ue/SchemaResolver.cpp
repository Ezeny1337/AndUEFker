#include "anduefker/ue/SchemaResolver.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace anduefker::ue
{
    namespace
    {
        std::optional<uintptr_t> Add(uintptr_t base, int32_t offset)
        {
            if (offset < 0 || base > UINTPTR_MAX - static_cast<uintptr_t>(offset))
                return std::nullopt;
            return base + static_cast<uintptr_t>(offset);
        }

        bool ReadPointer(const IMemorySource &memory, uintptr_t address, uintptr_t &value)
        {
            return memory.Read(address, value);
        }

        bool IsReadablePointer(const IMemorySource &memory, uintptr_t address)
        {
            return address != 0 && memory.IsReadable(address, sizeof(uintptr_t));
        }
    } // namespace

    SchemaResolver::SchemaResolver(const IMemorySource &memory,
                                   const RuntimeBinding &binding,
                                   const EngineVersion &version,
                                   SchemaProbeNames names)
        : memory_(memory), binding_(binding), version_(version), names_(std::move(names))
    {
    }

    std::optional<uintptr_t> SchemaResolver::FindObjectByName(const EngineSchema &schema,
                                                              const std::string &name) const
    {
        if (name.empty())
            return std::nullopt;
        ObjectStoreReader objects(memory_, binding_.objectRoot.address, binding_.objects, binding_.decode);
        if (!objects.Initialize())
            return std::nullopt;
        NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                              schema.fname, schema.features);
        for (int32_t index = 0; index < objects.Count(); ++index)
        {
            const auto object = objects.ObjectAt(index);
            if (!object)
                continue;
            const auto nameAddress = Add(*object, schema.uobject.name);
            if (!nameAddress)
                continue;
            int32_t rawIndex = 0;
            if (!memory_.Read(*nameAddress, rawIndex))
                continue;
            rawIndex = binding_.decode.nameIndex(rawIndex, *nameAddress);
            const auto objectName = names.ReadName(rawIndex);
            if (objectName && *objectName == name)
                return object;
        }
        return std::nullopt;
    }

    bool SchemaResolver::FindPointerField(uintptr_t first,
                                          uintptr_t expected,
                                          int32_t minOffset,
                                          int32_t maxOffset,
                                          int32_t &result) const
    {
        if (first == 0 || expected == 0 || minOffset < 0 || maxOffset < minOffset)
            return false;
        for (int32_t offset = minOffset; offset <= maxOffset; offset += static_cast<int32_t>(sizeof(uintptr_t)))
        {
            const auto address = Add(first, offset);
            if (!address)
                continue;
            uintptr_t value = 0;
            if (ReadPointer(memory_, *address, value) && value == expected)
            {
                result = offset;
                return true;
            }
        }
        return false;
    }

    bool SchemaResolver::FindInt32Field(uintptr_t object,
                                        int32_t expected,
                                        int32_t minOffset,
                                        int32_t maxOffset,
                                        int32_t &result) const
    {
        if (object == 0 || minOffset < 0 || maxOffset < minOffset)
            return false;
        for (int32_t offset = minOffset; offset <= maxOffset; offset += 4)
        {
            const auto address = Add(object, offset);
            if (!address)
                continue;
            int32_t value = 0;
            if (memory_.Read(*address, value) && value == expected)
            {
                result = offset;
                return true;
            }
        }
        return false;
    }

    bool SchemaResolver::ResolveUObjectSchema(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        // 前两个具有不同 InternalIndex 值的存活对象提供了一个保守的 bootstrap 基准
        // 具体的 class/name/outer 语义将在后续进行验证，本阶段绝不接受单个偶然碰巧相等的整数
        ObjectStoreReader objects(memory_, binding_.objectRoot.address, binding_.objects, binding_.decode);
        if (!objects.Initialize())
        {
            report.failures.push_back("object store could not be initialized for schema bootstrap");
            return false;
        }

        std::vector<std::pair<int32_t, uintptr_t>> samples;
        for (int32_t index = 0; index < objects.Count() && samples.size() < 256; ++index)
        {
            const auto object = objects.ObjectAt(index);
            if (!object)
                continue;
            samples.emplace_back(index, *object);
        }
        if (samples.size() < 2)
        {
            report.failures.push_back("not enough live UObject samples for schema bootstrap");
            return false;
        }

        schema.fname.comparisonIndex = 0;
        schema.fname.size = schema.features.casePreservingName ? 0xC : 0x8;
        schema.fname.number = schema.features.outlineNumberName ? -1 : 0x4;

        const NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                                    schema.fname, schema.features);

        // 通过已验证的 name 容器解码候选的 int32 值，以此定位 FName 字段
        // 一个有效的字段必须能在多个互不相关的 UObject 上生成合理的名称，而绝不能仅仅依靠单个可读的索引
        static constexpr const char *knownNames[] = {
            "None", "Object", "Class", "Struct", "Field", "Package", "Function",
            "ScriptStruct", "Guid", "Color", "Vector", "World", "Engine"};
        int32_t bestNameOffset = -1;
        int32_t bestNameScore = 0;
        int32_t bestNameHits = 0;
        int32_t bestKnownHits = 0;
        int32_t bestUniqueHits = 0;
        std::vector<std::string> bestNameSamples;
        for (int32_t offset = 0; offset <= 0x70; offset += 4)
        {
            // 在受支持的 64 位运行时布局中，FName 是一个 8 字节的值
            // 类似 +0x1C 这样的偏移量，实际上指向的是位于 +0x18 处真实 FName 的 Number 后半部分
            // 该位置连续出现的零可能会假冒成 NAME_None
            if (offset % static_cast<int32_t>(sizeof(uintptr_t)) != 0)
                continue;

            int32_t hits = 0;
            int32_t knownHits = 0;
            std::unordered_set<std::string> uniqueNames;
            std::vector<std::string> candidateSamples;
            for (const auto &[index, object] : samples)
            {
                int32_t rawNameIndex = 0;
                const auto address = Add(object, offset);
                if (!address || !memory_.Read(*address, rawNameIndex))
                    continue;
                rawNameIndex = binding_.decode.nameIndex(rawNameIndex, *address);
                const auto name = names.ReadName(rawNameIndex);
                if (name && !name->empty() && name->size() <= 1024)
                {
                    ++hits;
                    uniqueNames.insert(*name);
                    for (const char *known : knownNames)
                    {
                        if (*name == known)
                        {
                            ++knownHits;
                            break;
                        }
                    }
                    if (candidateSamples.size() < 8)
                    {
                        candidateSamples.push_back("object_index=" + std::to_string(index) +
                                                   " raw_name_index=" + std::to_string(rawNameIndex) +
                                                   " name=" + *name);
                    }
                }
            }
            const int32_t uniqueHits = static_cast<int32_t>(uniqueNames.size());
            // 相比于重复命中一小组已知常量，算法应优先选择具有名称多样性的方案
            // 真正的 UObject::Name 字段应当展现出许多不同的对象名称，包含零值或重复 bookkeeping values 的字段
            // 绝不能仅仅因为这些值能解码为已知名称就胜出
            const int32_t score = uniqueHits * 100 + knownHits * 20 + hits + 10;
            if (hits > 0)
            {
                report.evidence.push_back("UObject::Name candidate offset=" + std::to_string(offset) +
                                          " hits=" + std::to_string(hits) +
                                          " unique_names=" + std::to_string(uniqueHits) +
                                          " known_names=" + std::to_string(knownHits) +
                                          " score=" + std::to_string(score));
            }
            if (score > bestNameScore)
            {
                bestNameScore = score;
                bestNameHits = hits;
                bestKnownHits = knownHits;
                bestUniqueHits = uniqueHits;
                bestNameOffset = offset;
                bestNameSamples = std::move(candidateSamples);
            }
        }
        report.evidence.push_back("UObject::Name best candidate offset=" + std::to_string(bestNameOffset) +
                                  " score=" + std::to_string(bestNameScore) +
                                  " hits=" + std::to_string(bestNameHits) +
                                  " unique_names=" + std::to_string(bestUniqueHits) +
                                  " known_names=" + std::to_string(bestKnownHits));
        for (const std::string &sample : bestNameSamples)
            report.evidence.push_back("UObject::Name sample: " + sample);
        if (bestNameHits < 8 || bestKnownHits < 1 || bestUniqueHits < 8)
        {
            report.failures.push_back("UObject::Name did not decode consistently through the validated name store; valid_names=" +
                                      std::to_string(bestNameHits) + " unique_names=" + std::to_string(bestUniqueHits) +
                                      " known_names=" + std::to_string(bestKnownHits));
            return false;
        }
        schema.uobject.name = bestNameOffset;
        report.evidence.push_back("UObject::Name offset=" + std::to_string(bestNameOffset) +
                                  " valid_names=" + std::to_string(bestNameHits) +
                                  " unique_names=" + std::to_string(bestUniqueHits) +
                                  " known_names=" + std::to_string(bestKnownHits));

        // Class 是一个指针字段，其指向的目标本身就是一个可读的 UObject
        // 评分候选偏移量时，应同时结合指针的有效性以及通过上述定位到的字段解码目标对象名称的能力
        int32_t bestClassOffset = -1;
        int32_t bestClassHits = 0;
        for (int32_t offset = static_cast<int32_t>(sizeof(uintptr_t)); offset <= 0x70; offset += 4)
        {
            int32_t hits = 0;
            for (const auto &[index, object] : samples)
            {
                (void)index;
                const auto address = Add(object, offset);
                if (!address)
                    continue;
                uintptr_t classAddress = 0;
                if (!memory_.Read(*address, classAddress) || !IsReadablePointer(memory_, classAddress))
                    continue;
                const auto classNameAddress = Add(classAddress, bestNameOffset);
                if (!classNameAddress)
                    continue;
                int32_t rawClassName = 0;
                if (!memory_.Read(*classNameAddress, rawClassName))
                    continue;
                rawClassName = binding_.decode.nameIndex(rawClassName, *classNameAddress);
                if (names.ReadName(rawClassName))
                    ++hits;
            }
            if (hits > bestClassHits)
            {
                bestClassHits = hits;
                bestClassOffset = offset;
            }
        }
        if (bestClassHits < 3)
        {
            report.failures.push_back("UObject::Class did not decode consistently through the validated name store");
            return false;
        }
        schema.uobject.classPointer = bestClassOffset;

        for (int32_t offset = static_cast<int32_t>(sizeof(uintptr_t)); offset <= 0x80; offset += 4)
        {
            bool matches = true;
            for (const auto &[index, object] : samples)
            {
                const auto address = Add(object, offset);
                if (!address)
                {
                    matches = false;
                    break;
                }
                int32_t value = 0;
                if (!memory_.Read(*address, value) || value != index)
                {
                    matches = false;
                    break;
                }
            }
            if (matches && schema.uobject.internalIndex < 0)
            {
                schema.uobject.internalIndex = offset;
                break;
            }
        }

        if (schema.uobject.internalIndex < 0)
        {
            report.failures.push_back("UObject::InternalIndex was not resolved");
            return false;
        }

        if (schema.uobject.classPointer < 0)
        {
            report.failures.push_back("UObject::Class was not resolved");
            return false;
        }

        // Outer 是一个可为空的 UObject 指针
        // 应优先选择其非零值能回调指向采样对象集（sampled object set）的字段，这可以避免误将 UObject 体内的任意可读指针接受为 Outer
        const std::unordered_set<uintptr_t> sampleObjects = [&samples]()
        {
            std::unordered_set<uintptr_t> result;
            for (const auto &[index, object] : samples)
            {
                (void)index;
                result.insert(object);
            }
            return result;
        }();
        int32_t bestOuterOffset = -1;
        int32_t bestOuterHits = 0;
        for (int32_t offset = static_cast<int32_t>(sizeof(uintptr_t)); offset <= 0x80; offset += 4)
        {
            int32_t hits = 0;
            for (const auto &[index, object] : samples)
            {
                (void)index;
                const auto address = Add(object, offset);
                if (!address)
                    continue;
                uintptr_t value = 0;
                if (!memory_.Read(*address, value))
                    continue;
                value = binding_.decode.objectOuter(value, *address);
                if (value == 0 || sampleObjects.contains(value))
                    ++hits;
            }
            if (hits > bestOuterHits && offset != schema.uobject.classPointer)
            {
                bestOuterHits = hits;
                bestOuterOffset = offset;
            }
        }
        if (bestOuterHits >= 3)
            schema.uobject.outer = bestOuterOffset;

        int32_t bestFlagsOffset = -1;
        int32_t bestFlagsHits = 0;
        for (int32_t offset = static_cast<int32_t>(sizeof(uintptr_t)); offset <= 0x40; offset += 4)
        {
            int32_t hits = 0;
            for (const auto &[index, object] : samples)
            {
                (void)index;
                const auto address = Add(object, offset);
                if (!address)
                    continue;
                int32_t rawFlags = 0;
                if (!memory_.Read(*address, rawFlags))
                    continue;
                const uint32_t flags = static_cast<uint32_t>(binding_.decode.objectFlags(rawFlags, *address));
                if ((flags & 0xFFFF0000u) == 0 && (flags & 0x1u) != 0)
                    ++hits;
            }
            if (hits > bestFlagsHits)
            {
                bestFlagsHits = hits;
                bestFlagsOffset = offset;
            }
        }
        if (bestFlagsHits >= 3)
            schema.uobject.flags = bestFlagsOffset;
        else
        {
            std::vector<uintptr_t> flagObjects;
            for (int32_t index = 0; index < objects.Count() && flagObjects.size() < 256; ++index)
            {
                const auto object = objects.ObjectAt(index);
                if (object)
                    flagObjects.push_back(*object);
            }
            for (int32_t offset = static_cast<int32_t>(sizeof(uintptr_t)); offset <= 0x40; offset += 4)
            {
                int32_t hits = 0;
                for (uintptr_t object : flagObjects)
                {
                    const auto address = Add(object, offset);
                    if (!address)
                        continue;
                    int32_t rawFlags = 0;
                    if (!memory_.Read(*address, rawFlags))
                        continue;
                    const uint32_t flags = static_cast<uint32_t>(binding_.decode.objectFlags(rawFlags, *address));
                    if (flags == 0x43u)
                        ++hits;
                }
                if (hits >= 3)
                {
                    schema.uobject.flags = offset;
                    break;
                }
            }
        }
        if (schema.uobject.flags < 0)
        {
            report.failures.push_back("UObject::Flags was not resolved");
            return false;
        }

        report.evidence.push_back("resolved UObject::InternalIndex and UObject::Class bootstrap fields");
        report.evidence.push_back("resolved UObject::Name through name-store cross-validation");
        return true;
    }

    bool SchemaResolver::ValidateUObjectSchema(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        if (schema.uobject.internalIndex < 0 || schema.uobject.classPointer < 0)
            return false;
        report.evidence.push_back("UObject bootstrap fields are structurally consistent");
        return true;
    }

    bool SchemaResolver::ResolveStructSchema(EngineSchema &schema, SchemaResolutionReport &report) const
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

        const std::array<std::pair<uintptr_t, int32_t>, 3> knownSizes = {
            std::pair{*guid, 0x10}, std::pair{*color, 0x04},
            std::pair{*vector, schema.features.largeWorldCoordinates ? 0x18 : 0x0C}};
        for (int32_t offset = 0; offset <= 0x100 - 4; offset += 4)
        {
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
            {
                schema.ustruct.size = offset;
                break;
            }
        }
        if (schema.ustruct.size < 0)
        {
            report.failures.push_back("UStruct::Size was not resolved from Guid/Color/Vector");
            return false;
        }

        const auto structObject = FindObjectByName(schema, names_.structClass);
        const auto fieldObject = FindObjectByName(schema, names_.fieldClass);
        if (structObject && fieldObject &&
            FindPointerField(*structObject, *fieldObject, static_cast<int32_t>(sizeof(uintptr_t)), 0x100, schema.ustruct.superStruct))
        {
            report.evidence.push_back("resolved UStruct::SuperStruct from Struct -> Field relation");
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

    bool SchemaResolver::ResolveFieldSchema(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        if (!schema.features.useFProperty)
        {
            const auto guid = FindObjectByName(schema, names_.guidStruct);
            const auto color = FindObjectByName(schema, names_.colorStruct);
            if (!guid || !color)
            {
                report.failures.push_back("UProperty samples are unavailable");
                return false;
            }

            NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                                  schema.fname, schema.features);
            const auto readObjectName = [&](uintptr_t object) -> std::optional<std::string>
            {
                const auto address = Add(object, schema.uobject.name);
                if (!address)
                    return std::nullopt;
                int32_t raw = 0;
                if (!memory_.Read(*address, raw))
                    return std::nullopt;
                raw = binding_.decode.nameIndex(raw, *address);
                return names.ReadName(raw);
            };

            const auto findChildren = [&](uintptr_t structure) -> std::optional<int32_t>
            {
                for (int32_t offset = 0x20; offset <= 0x100; offset += 4)
                {
                    uintptr_t field = 0;
                    const auto address = Add(structure, offset);
                    if (!address || !memory_.Read(*address, field) || !IsReadablePointer(memory_, field))
                        continue;
                    const auto fieldName = readObjectName(field);
                    if (fieldName && (*fieldName == "A" || *fieldName == "B" || *fieldName == "R"))
                        return offset;
                }
                return std::nullopt;
            };

            const auto children = findChildren(*guid);
            if (!children)
            {
                report.failures.push_back("UStruct::Children was not resolved for UProperty");
                return false;
            }
            schema.ustruct.children = *children;

            uintptr_t firstField = 0;
            if (!memory_.Read(*Add(*guid, *children), firstField))
                return false;
            for (int32_t offset = 0x20; offset <= 0x80; offset += 4)
            {
                uintptr_t next = 0;
                const auto address = Add(firstField, offset);
                if (!address || !memory_.Read(*address, next))
                    continue;
                const auto nextName = readObjectName(next);
                if (nextName && (*nextName == "B" || *nextName == "C"))
                {
                    schema.ufield.next = offset;
                    break;
                }
            }
            if (schema.ufield.next < 0)
            {
                report.failures.push_back("UField::Next was not resolved");
                return false;
            }
            schema.validation.fields = true;
            report.evidence.push_back("UProperty family does not require FField runtime schema");
            return true;
        }

        const auto guid = FindObjectByName(schema, names_.guidStruct);
        if (!guid)
        {
            report.failures.push_back("FProperty Guid sample is unavailable");
            return false;
        }
        NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                              schema.fname, schema.features);

        const auto readFieldName = [&](uintptr_t field, int32_t nameOffset) -> std::optional<std::string>
        {
            int32_t raw = 0;
            const auto address = Add(field, nameOffset);
            if (!address || !memory_.Read(*address, raw))
                return std::nullopt;
            raw = binding_.decode.nameIndex(raw, *address);
            return names.ReadName(raw);
        };

        int32_t foundPropertiesOffset = -1;
        uintptr_t firstField = 0;
        int32_t foundNameOffset = -1;
        for (int32_t propertiesOffset = 0x20; propertiesOffset <= 0x100 && foundPropertiesOffset < 0; propertiesOffset += 4)
        {
            uintptr_t candidate = 0;
            const auto address = Add(*guid, propertiesOffset);
            if (!address || !memory_.Read(*address, candidate) || !IsReadablePointer(memory_, candidate))
                continue;

            for (int32_t nameOffset : {0x20, 0x28, 0x30, 0x38})
            {
                const auto fieldName = readFieldName(candidate, nameOffset);
                if (fieldName && (*fieldName == "A" || *fieldName == "D"))
                {
                    foundPropertiesOffset = propertiesOffset;
                    firstField = candidate;
                    foundNameOffset = nameOffset;
                    break;
                }
            }
        }
        if (foundPropertiesOffset < 0)
        {
            report.failures.push_back("UStruct::ChildProperties was not resolved");
            return false;
        }
        schema.ustruct.childProperties = foundPropertiesOffset;
        schema.ffield.name = foundNameOffset;

        for (int32_t nextOffset : {0x18, 0x20, 0x28, 0x30, 0x38})
        {
            uintptr_t next = 0;
            const auto address = Add(firstField, nextOffset);
            if (!address || !memory_.Read(*address, next) || !IsReadablePointer(memory_, next))
                continue;
            const auto nextName = readFieldName(next, foundNameOffset);
            if (nextName && (*nextName == "B" || *nextName == "C"))
            {
                schema.ffield.next = nextOffset;
                break;
            }
        }
        if (schema.ffield.next < 0)
        {
            report.failures.push_back("FField::Next was not resolved");
            return false;
        }

        for (int32_t classOffset : {0x08, 0x10, 0x18})
        {
            uintptr_t fieldClass = 0;
            const auto address = Add(firstField, classOffset);
            if (!address || !memory_.Read(*address, fieldClass) || !IsReadablePointer(memory_, fieldClass))
                continue;
            schema.ffield.classPointer = classOffset;
            for (int32_t classNameOffset : {0x00, 0x08, 0x10, 0x18})
            {
                const auto nameAddress = Add(fieldClass, classNameOffset);
                if (!nameAddress)
                    continue;
                int32_t raw = 0;
                if (!memory_.Read(*nameAddress, raw))
                    continue;
                raw = binding_.decode.nameIndex(raw, *nameAddress);
                const auto className = names.ReadName(raw);
                if (className && className->find("Property") != std::string::npos)
                {
                    schema.ffieldClass.name = classNameOffset;
                    break;
                }
            }
            if (schema.ffieldClass.name >= 0)
                break;
        }
        if (schema.ffield.classPointer < 0 || schema.ffieldClass.name < 0)
        {
            report.failures.push_back("FField::Class or FFieldClass::Name was not resolved");
            return false;
        }

        const int32_t ownerStorageSize = schema.features.fFieldOwnerMask
                                             ? static_cast<int32_t>(sizeof(uintptr_t))
                                             : static_cast<int32_t>(sizeof(uintptr_t) * 2);
        schema.ffield.owner = schema.ffield.next - ownerStorageSize;
        schema.ffieldClass.castFlags = schema.ffieldClass.name + static_cast<int32_t>(sizeof(uintptr_t));
        schema.validation.fields = true;
        report.evidence.push_back("resolved FField chain from CoreUObject.Guid properties");
        return true;
    }

    bool SchemaResolver::ResolvePropertySchema(EngineSchema &schema, SchemaResolutionReport &report) const
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

        auto matchesInt32 = [&](int32_t offset, const std::function<int32_t(int32_t)> &transform)
        {
            for (const auto &[property, expected] : properties)
            {
                int32_t value = 0;
                const auto address = Add(property, offset);
                if (!address || !memory_.Read(*address, value) || transform(value) != expected)
                    return false;
            }
            return true;
        };

        for (int32_t offset = 0; offset <= 0x100 - 4; offset += 4)
        {
            if (matchesInt32(offset, [](int32_t value)
                             { return value; }))
            {
                schema.property.offsetInternal = offset;
                break;
            }
        }
        if (schema.property.offsetInternal < 0)
        {
            report.failures.push_back("FProperty::Offset_Internal was not resolved");
            return false;
        }

        // Guid 字段是四个 4 字节构成的标量属性（scalar properties）
        // 这为 ElementSize 和 ArrayDim 提供了一个独立的鉴别特征（discriminator），而不是依赖源码中的定义顺序
        for (int32_t offset = 0; offset <= schema.property.offsetInternal; offset += 4)
        {
            bool valid = true;
            for (const auto &[property, expected] : properties)
            {
                (void)expected;
                int32_t value = 0;
                const auto address = Add(property, offset);
                if (!address || !memory_.Read(*address, value) || value != 4)
                {
                    valid = false;
                    break;
                }
            }
            if (valid && offset != schema.property.offsetInternal)
            {
                schema.property.elementSize = offset;
                break;
            }
        }
        if (schema.property.elementSize < 0)
        {
            report.failures.push_back("FProperty::ElementSize was not resolved");
            return false;
        }

        for (int32_t offset = 0; offset <= schema.property.elementSize; offset += 4)
        {
            bool valid = true;
            for (const auto &[property, expected] : properties)
            {
                int32_t value = 0;
                const auto address = Add(property, offset);
                if (!address || !memory_.Read(*address, value) || value != 1)
                {
                    valid = false;
                    break;
                }
            }
            if (valid)
            {
                schema.property.arrayDim = offset;
                break;
            }
        }
        if (schema.property.arrayDim < 0)
        {
            report.failures.push_back("FProperty::ArrayDim was not resolved");
            return false;
        }

        // UE 5.6 的 EPropertyFlags 是 uint64_t。在 64 位目标上，它应当在
        // ElementSize 之后、Offset_Internal 之前按自然边界对齐。之前按 4
        // 字节扫描会把 ElementSize 的后半部分和填充区（例如 0xCDCDCDCD）
        // 误当成 flags。
        const int32_t flagsStart = (schema.property.elementSize + 4 +
                                    static_cast<int32_t>(sizeof(uint64_t)) - 1) /
                                   static_cast<int32_t>(sizeof(uint64_t)) * static_cast<int32_t>(sizeof(uint64_t));
        const int32_t flagsEnd = schema.property.offsetInternal - static_cast<int32_t>(sizeof(uint64_t));
        for (int32_t offset = flagsStart; offset <= flagsEnd; offset += static_cast<int32_t>(sizeof(uint64_t)))
        {
            bool valid = true;
            for (const auto &[property, expected] : properties)
            {
                (void)expected;
                uint64_t flags = 0;
                const auto address = Add(property, offset);
                if (!address || !memory_.Read(*address, flags) || flags == 0 ||
                    (flags & 0xE000000000000000ull) != 0 ||
                    (flags & 0xFFFFFFFFull) == 0xCDCDCDCDull)
                {
                    valid = false;
                    break;
                }
            }
            if (valid)
            {
                schema.property.propertyFlags = offset;
                break;
            }
        }
        if (schema.property.propertyFlags < 0)
        {
            report.failures.push_back("FProperty::PropertyFlags was not resolved");
            return false;
        }

        schema.property.baseSize = schema.property.elementSize;
        schema.validation.properties = true;
        report.evidence.push_back("resolved property ArrayDim/ElementSize/Flags/Offset_Internal from Guid chain");
        return true;
    }

    bool SchemaResolver::ResolvePropertySubtypes(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        if (!schema.features.useFProperty)
        {
            report.evidence.push_back("property subtype probing skipped for UProperty family");
            return true;
        }

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
        const ObjectStoreReader &objects = model.Objects();
        for (int32_t index = 0; index < objects.Count() && samples.size() < wanted.size(); ++index)
        {
            const auto object = objects.ObjectAt(index);
            if (!object)
                continue;
            const auto className = model.ClassName(*object);
            if (!className || (*className != "Class" && *className != "ScriptStruct"))
                continue;
            const auto first = model.StructProperties(*object);
            if (!first)
                continue;
            for (const FieldMetadata &field : model.Fields(*first, 2048))
            {
                if (std::find(wanted.begin(), wanted.end(), field.className) != wanted.end() &&
                    !samples.contains(field.className))
                    samples.emplace(field.className, field.address);
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
        auto isFieldClass = [&](uintptr_t value)
        {
            const auto field = model.Field(value);
            return field && field->className.find("Property") != std::string::npos;
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
            for (const char *name : {"DelegateProperty", "MulticastDelegateProperty",
                                     "MulticastInlineDelegateProperty", "MulticastSparseDelegateProperty"})
            {
                const int32_t offset = findPointer(name, [&](uintptr_t value)
                                                   { return isUObjectClass(value, "Function"); });
                if (offset >= 0)
                {
                    schema.propertySubtypes.delegateSignature = offset;
                    break;
                }
            }
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

    bool SchemaResolver::ResolveFunctionSchema(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        ObjectStoreReader objects(memory_, binding_.objectRoot.address, binding_.objects, binding_.decode);
        if (!objects.Initialize())
        {
            report.failures.push_back("object store unavailable for function schema");
            return false;
        }
        NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                              schema.fname, schema.features);

        const auto readObjectName = [&](uintptr_t object) -> std::optional<std::string>
        {
            const auto address = Add(object, schema.uobject.name);
            if (!address)
                return std::nullopt;
            int32_t raw = 0;
            if (!memory_.Read(*address, raw))
                return std::nullopt;
            raw = binding_.decode.nameIndex(raw, *address);
            return names.ReadName(raw);
        };

        struct FunctionSample
        {
            uintptr_t address = 0;
            bool parameterChainValid = false;
            int32_t parameterCount = 0;
            int32_t parameterEnd = 0;
        };

        std::vector<FunctionSample> functions;
        for (int32_t index = 0; index < objects.Count() && functions.size() < 64; ++index)
        {
            const auto object = objects.ObjectAt(index);
            if (!object)
                continue;
            const auto classAddress = Add(*object, schema.uobject.classPointer);
            if (!classAddress)
                continue;
            uintptr_t classObject = 0;
            if (!memory_.Read(*classAddress, classObject))
                continue;
            const auto className = readObjectName(classObject);
            if (className && *className == "Function")
                functions.push_back(FunctionSample{*object});
        }
        if (functions.size() < 2)
        {
            report.failures.push_back("not enough UFunction samples");
            return false;
        }

        // 通过两个函数对象解析 UField::Next
        // 下一个指针（Next）可能为空，因此在可用时，应利用前两个链表条目中的字段名称来进行解析
        for (int32_t offset = 0x20; offset <= 0x100 && schema.ufield.next < 0; offset += 4)
        {
            size_t readable = 0;
            for (const FunctionSample &sample : functions)
            {
                uintptr_t next = 0;
                const auto address = Add(sample.address, offset);
                if (address && memory_.Read(*address, next) && (next == 0 || IsReadablePointer(memory_, next)))
                    ++readable;
            }
            if (readable == functions.size())
                schema.ufield.next = offset;
        }
        if (schema.ufield.next < 0)
        {
            report.failures.push_back("UField::Next was not resolved from UFunction samples");
            return false;
        }

        ObjectModelReader model(memory_, binding_, schema);
        if (!model.Initialize())
        {
            report.failures.push_back("object model could not be initialized for UFunction parameter validation");
            return false;
        }

        // 记录每个函数的参数字段形状
        // UE 将参数属性存放在 UStruct::ChildProperties 链中，NumParms 和 ParmsSize 必须与该链的独立解析结果一致
        for (FunctionSample &sample : functions)
        {
            const auto firstAddress = Add(sample.address, schema.ustruct.childProperties);
            uintptr_t current = 0;
            if (!firstAddress || !memory_.Read(*firstAddress, current))
                continue;

            std::unordered_set<uintptr_t> visited;
            bool valid = true;
            while (current != 0 && visited.insert(current).second && visited.size() <= 256)
            {
                const auto field = model.Field(current);
                const auto property = model.Property(current);
                if (!field || !property || property->arrayDim <= 0 || property->elementSize <= 0 ||
                    property->offset < 0)
                {
                    valid = false;
                    break;
                }

                const int64_t end = static_cast<int64_t>(property->offset) +
                                    static_cast<int64_t>(property->elementSize) * property->arrayDim;
                if (end <= property->offset || end > 0x10000)
                {
                    valid = false;
                    break;
                }
                // UFunction::InitializeDerivedMembers 只统计带有 CPF_Parm 的属性
                // ChildProperties 还可能包含用于默认值的局部属性
                // 它们不属于 NumParms/ParmsSize
                if ((property->flags & 0x00000080ull) != 0)
                {
                    ++sample.parameterCount;
                    sample.parameterEnd = std::max(sample.parameterEnd, static_cast<int32_t>(end));
                }
                current = field->nextAddress;
            }
            if (current != 0 || visited.size() > 256)
                valid = false;
            sample.parameterChainValid = valid;
        }

        // UE 5.6 按以下顺序声明 UFunction 自身字段：
        //
        //   EFunctionFlags FunctionFlags;
        //   uint8         NumParms;
        //   uint16        ParmsSize;
        constexpr uint32_t kFunctionFlagEvidenceMask =
            0x00000040u | // FUNC_Net
            0x00000200u | // FUNC_Exec
            0x00000400u | // FUNC_Native
            0x00000800u | // FUNC_Event
            0x00010000u | // FUNC_MulticastDelegate
            0x00020000u | // FUNC_Public
            0x00040000u | // FUNC_Private
            0x00080000u | // FUNC_Protected
            0x00100000u | // FUNC_Delegate
            0x00400000u | // FUNC_HasOutParms
            0x04000000u | // FUNC_BlueprintCallable
            0x08000000u;  // FUNC_BlueprintEvent

        const auto isPlausibleFunctionFlags = [&](uint32_t flags)
        {
            return flags != 0 && flags != 0xCDCDCDCDu &&
                   (flags & kFunctionFlagEvidenceMask) != 0;
        };
        const auto isPlausibleParameterShape = [](uint8_t numParams, uint16_t paramSize)
        {
            // 有参数的函数必须预留参数存储空间，没有参数的函数不能声明一个任意的非零大小
            return (numParams == 0 && paramSize == 0) ||
                   (numParams != 0 && paramSize != 0);
        };

        // PropertiesSize 是字段偏移，而不是 sizeof(UStruct)
        // 这里只把它用作下界锚点，后面的参数链一致性检查才是候选字段的语义验证
        int32_t functionDataStart = schema.ustruct.size >= 0 ? schema.ustruct.size + 4 : 0;
        functionDataStart = (functionDataStart + 3) & ~3;

        int32_t bestFunctionFlagsOffset = -1;
        size_t bestFunctionFlagsHits = 0;
        size_t bestParameterShapeHits = 0;
        size_t parameterChainSamples = 0;
        for (const FunctionSample &sample : functions)
            parameterChainSamples += sample.parameterChainValid ? 1u : 0u;
        // 在当前 Android Shipping/Cooked 配置中，UE 5.6 的 UStruct 字段
        // 从 PropertiesSize 到 UFunction::FunctionFlags 之间包含：
        // MinAlignment/StructStateFlags、Script、四条 PropertyLink、
        // ScriptAndPropertyObjectReferences、UnresolvedScriptProperties 和
        // UnversionedGameSchema，总计 88 字节。优先使用这个源码推导位置，
        // 不让普通数据字段的“像 flags”值赢过真实布局。
        const int32_t sourceFunctionFlagsOffset = schema.ustruct.size >= 0 ? schema.ustruct.size + 88 : -1;
        size_t sourceFunctionFlagsHits = 0;
        size_t sourceParameterShapeHits = 0;
        for (int32_t offset = functionDataStart; offset <= 0x200; offset += 4)
        {
            size_t flagHits = 0;
            size_t parameterShapeHits = 0;
            for (const FunctionSample &sample : functions)
            {
                if (!sample.parameterChainValid)
                    continue;

                const auto flagsAddress = Add(sample.address, offset);
                const auto numParamsAddress = Add(sample.address, offset + 4);
                const auto paramSizeAddress = Add(sample.address, offset + 6);
                if (!flagsAddress || !numParamsAddress || !paramSizeAddress)
                    continue;

                uint32_t flags = 0;
                uint8_t numParams = 0;
                uint16_t paramSize = 0;
                if (!memory_.Read(*flagsAddress, flags) ||
                    !memory_.Read(*numParamsAddress, numParams) ||
                    !memory_.Read(*paramSizeAddress, paramSize))
                    continue;
                if (!isPlausibleFunctionFlags(flags))
                    continue;

                ++flagHits;
                if (isPlausibleParameterShape(numParams, paramSize) &&
                    numParams == sample.parameterCount && paramSize >= sample.parameterEnd)
                    ++parameterShapeHits;
            }

            if (offset == sourceFunctionFlagsOffset)
            {
                sourceFunctionFlagsHits = flagHits;
                sourceParameterShapeHits = parameterShapeHits;
            }

            if (flagHits > bestFunctionFlagsHits ||
                (flagHits == bestFunctionFlagsHits && parameterShapeHits > bestParameterShapeHits))
            {
                bestFunctionFlagsOffset = offset;
                bestFunctionFlagsHits = flagHits;
                bestParameterShapeHits = parameterShapeHits;
            }
        }

        const size_t requiredFunctionHits = std::max<size_t>(4, (parameterChainSamples * 3) / 4);
        if (sourceFunctionFlagsOffset >= 0 && sourceFunctionFlagsHits >= requiredFunctionHits)
        {
            bestFunctionFlagsOffset = sourceFunctionFlagsOffset;
            bestFunctionFlagsHits = sourceFunctionFlagsHits;
            bestParameterShapeHits = sourceParameterShapeHits;
            report.evidence.push_back("UFunction::FunctionFlags preferred source-layout candidate offset=" +
                                      std::to_string(sourceFunctionFlagsOffset) +
                                      " flag_hits=" + std::to_string(sourceFunctionFlagsHits) +
                                      " parameter_shape_hits=" + std::to_string(sourceParameterShapeHits));
        }
        if (bestFunctionFlagsOffset < 0 || bestFunctionFlagsHits < requiredFunctionHits)
        {
            report.failures.push_back("UFunction::FunctionFlags was not resolved consistently; parameter_chain_samples=" +
                                      std::to_string(parameterChainSamples) +
                                      " flag_hits=" + std::to_string(bestFunctionFlagsHits) +
                                      " parameter_shape_hits=" + std::to_string(bestParameterShapeHits));
            return false;
        }
        schema.ufunction.functionFlags = bestFunctionFlagsOffset;
        const bool parametersResolved = parameterChainSamples >= 4 && bestParameterShapeHits >= requiredFunctionHits;
        if (parametersResolved)
        {
            schema.ufunction.numParams = bestFunctionFlagsOffset + 4;
            schema.ufunction.paramSize = bestFunctionFlagsOffset + 6;
            report.evidence.push_back("resolved UFunction::FunctionFlags, NumParms and ParmsSize from multiple samples; flags_offset=" +
                                      std::to_string(schema.ufunction.functionFlags) +
                                      " flag_hits=" + std::to_string(bestFunctionFlagsHits) +
                                      " parameter_shape_hits=" + std::to_string(bestParameterShapeHits));
        }
        else
        {
            report.failures.push_back("UFunction::NumParms/ParmsSize remain unresolved; parameter_chain_samples=" +
                                      std::to_string(parameterChainSamples) +
                                      " flag_hits=" + std::to_string(bestFunctionFlagsHits) +
                                      " parameter_shape_hits=" + std::to_string(bestParameterShapeHits) +
                                      " flags_offset=" + std::to_string(schema.ufunction.functionFlags));
            report.evidence.push_back("resolved UFunction::FunctionFlags only; NumParms/ParmsSize were withheld because parameter-chain validation was insufficient");
        }

        // UE 5.6 中 Func 相对于 FunctionFlags 只可能落在几个由条件编译决定的位置
        // 即无 event graph、带 event graph、再加 Live Coding 指针
        // 限制在这些源码布局位置，避免把对象后部其他可执行地址误认为 UFunction::Func
        const std::array<int32_t, 4> nativeFunctionDeltas = {0x18, 0x28, 0x30, 0x38};
        int32_t bestNativeFunctionOffset = -1;
        size_t bestNativeFunctionHits = 0;
        for (const int32_t delta : nativeFunctionDeltas)
        {
            const int32_t offset = schema.ufunction.functionFlags + delta;
            size_t hits = 0;
            for (const FunctionSample &sample : functions)
            {
                uintptr_t native = 0;
                const auto address = Add(sample.address, offset);
                if (address && memory_.Read(*address, native) && native != 0 &&
                    memory_.IsExecutable(native, sizeof(uintptr_t)))
                    ++hits;
            }
            if (hits > bestNativeFunctionHits)
            {
                bestNativeFunctionOffset = offset;
                bestNativeFunctionHits = hits;
            }
        }
        if (bestNativeFunctionOffset < 0 || bestNativeFunctionHits < std::max<size_t>(2, functions.size() / 2))
        {
            report.failures.push_back("UFunction::ExecFunction was not resolved");
            return false;
        }
        schema.ufunction.nativeFunction = bestNativeFunctionOffset;
        report.evidence.push_back("resolved UFunction::ExecFunction from executable pointers; offset=" +
                                  std::to_string(schema.ufunction.nativeFunction) +
                                  " hits=" + std::to_string(bestNativeFunctionHits));

        const auto readClassName = [&](uintptr_t object) -> std::optional<std::string>
        {
            const auto classAddress = Add(object, schema.uobject.classPointer);
            if (!classAddress)
                return std::nullopt;
            uintptr_t classObject = 0;
            if (!memory_.Read(*classAddress, classObject))
                return std::nullopt;
            const auto nameAddress = Add(classObject, schema.uobject.name);
            if (!nameAddress)
                return std::nullopt;
            int32_t raw = 0;
            if (!memory_.Read(*nameAddress, raw))
                return std::nullopt;
            raw = binding_.decode.nameIndex(raw, *nameAddress);
            return names.ReadName(raw);
        };

        for (const char *ownerName : {"KismetSystemLibrary", "Actor", "PlayerController"})
        {
            const auto owner = FindObjectByName(schema, ownerName);
            if (!owner)
                continue;
            for (int32_t offset = 0x20; offset <= 0x180; offset += 4)
            {
                uintptr_t child = 0;
                const auto address = Add(*owner, offset);
                if (!address || !memory_.Read(*address, child) || child == 0)
                    continue;
                const auto childClass = readClassName(child);
                if (childClass && *childClass == "Function")
                {
                    schema.ustruct.children = offset;
                    break;
                }
            }
            if (schema.ustruct.children >= 0)
                break;
        }
        if (schema.ustruct.children < 0)
        {
            report.failures.push_back("UStruct::Children was not resolved from reflected classes");
            return false;
        }

        schema.validation.functions = true;
        report.evidence.push_back("resolved UField::Next and UStruct::Children for UFunction reflection");
        return true;
    }

    bool SchemaResolver::ResolveEnumSchema(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        ObjectStoreReader objects(memory_, binding_.objectRoot.address, binding_.objects, binding_.decode);
        if (!objects.Initialize())
            return false;
        NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                              schema.fname, schema.features);
        std::vector<uintptr_t> enumObjects;
        for (int32_t index = 0; index < objects.Count(); ++index)
        {
            const auto object = objects.ObjectAt(index);
            if (!object)
                continue;
            const auto classAddress = Add(*object, schema.uobject.classPointer);
            if (!classAddress)
                continue;
            uintptr_t classObject = 0;
            if (!memory_.Read(*classAddress, classObject))
                continue;
            const auto nameAddress = Add(classObject, schema.uobject.name);
            if (!nameAddress)
                continue;
            int32_t raw = 0;
            if (!memory_.Read(*nameAddress, raw))
                continue;
            raw = binding_.decode.nameIndex(raw, *nameAddress);
            const auto className = names.ReadName(raw);
            if (className && *className == "Enum")
            {
                enumObjects.push_back(*object);
                if (enumObjects.size() >= 32)
                    break;
            }
        }
        if (enumObjects.size() < 2)
        {
            report.failures.push_back("not enough UEnum sample objects were found");
            return false;
        }

        const int32_t nameSize = schema.fname.size > 0 ? schema.fname.size : static_cast<int32_t>(sizeof(uintptr_t));
        const int32_t valueOffset = (nameSize + static_cast<int32_t>(alignof(int64_t)) - 1) /
                                    static_cast<int32_t>(alignof(int64_t)) * static_cast<int32_t>(alignof(int64_t));
        const int32_t entryStride = valueOffset + static_cast<int32_t>(sizeof(int64_t));
        const int32_t namesStart = std::max({0x30,
                                             schema.ufield.next >= 0
                                                 ? schema.ufield.next + static_cast<int32_t>(sizeof(uintptr_t))
                                                 : 0x30});

        int32_t bestNamesOffset = -1;
        size_t bestValidArrays = 0;
        size_t bestNonEmptyArrays = 0;
        size_t bestValidEntries = 0;
        for (int32_t offset = (namesStart + 7) & ~7; offset <= 0x180; offset += 8)
        {
            size_t validArrays = 0;
            size_t nonEmptyArrays = 0;
            size_t validEntries = 0;
            for (uintptr_t enumObject : enumObjects)
            {
                uintptr_t data = 0;
                int32_t count = 0;
                int32_t capacity = 0;
                const auto dataAddress = Add(enumObject, offset);
                const auto countAddress = Add(enumObject, offset + static_cast<int32_t>(sizeof(uintptr_t)));
                const auto capacityAddress = Add(enumObject, offset + static_cast<int32_t>(sizeof(uintptr_t) + sizeof(int32_t)));
                if (!dataAddress || !countAddress || !capacityAddress || !memory_.Read(*dataAddress, data) ||
                    !memory_.Read(*countAddress, count) || !memory_.Read(*capacityAddress, capacity))
                    continue;
                if (count < 0 || count > 0x100000 || capacity < count || capacity > 0x100000)
                    continue;
                if (count > 0 && (!IsReadablePointer(memory_, data) || !memory_.IsReadable(data, entryStride)))
                    continue;

                ++validArrays;
                if (count == 0)
                    continue;

                const int32_t sampleCount = std::min(count, 8);
                const int32_t requiredEntries = std::min(count, 4);
                int32_t decodedEntries = 0;
                for (int32_t entryIndex = 0; entryIndex < sampleCount; ++entryIndex)
                {
                    const auto entry = Add(data, entryIndex * entryStride);
                    if (!entry)
                        break;
                    const auto valueAddress = Add(*entry, valueOffset);
                    int64_t value = 0;
                    const auto entryName = names.ReadFName(*entry);
                    if (valueAddress && entryName && !entryName->empty() && memory_.Read(*valueAddress, value))
                        ++decodedEntries;
                }
                validEntries += static_cast<size_t>(decodedEntries);
                if (decodedEntries >= requiredEntries)
                    ++nonEmptyArrays;
            }

            if (validArrays > bestValidArrays ||
                (validArrays == bestValidArrays && nonEmptyArrays > bestNonEmptyArrays) ||
                (validArrays == bestValidArrays && nonEmptyArrays == bestNonEmptyArrays &&
                 validEntries > bestValidEntries))
            {
                bestNamesOffset = offset;
                bestValidArrays = validArrays;
                bestNonEmptyArrays = nonEmptyArrays;
                bestValidEntries = validEntries;
            }
        }

        const size_t requiredArrays = std::max<size_t>(4, enumObjects.size() / 2);
        const size_t requiredNonEmptyArrays = std::min<size_t>(4, enumObjects.size());
        if (bestNamesOffset < 0 || bestValidArrays < requiredArrays ||
            bestNonEmptyArrays < requiredNonEmptyArrays)
        {
            report.failures.push_back("UEnum::Names was not resolved from FName/int64 array samples; valid_arrays=" +
                                      std::to_string(bestValidArrays) +
                                      " non_empty_arrays=" + std::to_string(bestNonEmptyArrays) +
                                      " valid_entries=" + std::to_string(bestValidEntries));
            return false;
        }
        schema.uenum.names = bestNamesOffset;
        report.evidence.push_back("resolved UEnum::Names from FName/int64 entries; offset=" +
                                  std::to_string(schema.uenum.names) +
                                  " valid_arrays=" + std::to_string(bestValidArrays) +
                                  " non_empty_arrays=" + std::to_string(bestNonEmptyArrays) +
                                  " valid_entries=" + std::to_string(bestValidEntries));

        // 在源码布局中，UEnum 将 CppForm 和 EEnumFlags 紧跟在 name/value pairs 的 TArray 之后存储
        // 将这些保持为可选探测项，因为已 Cook 或分支构建版本可能会插入其他字段
        size_t formHits = 0;
        size_t flagHits = 0;
        for (uintptr_t enumObject : enumObjects)
        {
            const auto enumFormAddress = Add(enumObject, schema.uenum.names + static_cast<int32_t>(sizeof(uintptr_t) * 2));
            const auto enumFlagsAddress = enumFormAddress ? Add(*enumFormAddress, 1) : std::nullopt;
            uint8_t cppForm = 0;
            uint8_t enumFlags = 0;
            if (enumFormAddress && memory_.Read(*enumFormAddress, cppForm) && cppForm <= 2)
                ++formHits;
            if (enumFlagsAddress && memory_.Read(*enumFlagsAddress, enumFlags) && (enumFlags & ~0x03u) == 0)
                ++flagHits;
        }
        if (formHits >= requiredNonEmptyArrays)
        {
            schema.uenum.cppForm = schema.uenum.names + static_cast<int32_t>(sizeof(uintptr_t) * 2);
            report.evidence.push_back("resolved UEnum::CppForm; hits=" + std::to_string(formHits));
        }
        if (flagHits >= requiredNonEmptyArrays)
        {
            schema.uenum.flags = schema.uenum.cppForm >= 0 ? schema.uenum.cppForm + 1 : schema.uenum.names + static_cast<int32_t>(sizeof(uintptr_t) * 2) + 1;
            report.evidence.push_back("resolved UEnum::EnumFlags; hits=" + std::to_string(flagHits));
        }
        schema.validation.enums = true;
        report.evidence.push_back(schema.features.enumUsesFNameData ? "UE5.6 FNameData enum family selected" : "legacy enum container family selected");
        return true;
    }

    SchemaResolutionReport SchemaResolver::Resolve(EngineSchema &schema) const
    {
        SchemaResolutionReport report;
        schema.family = SchemaCatalog::FamilyFor(version_);
        schema.features = SchemaCatalog::FeaturesFor(version_);
        report.evidence.push_back("engine feature matrix version=" + version_.ToString() +
                                  " use_fproperty=" + std::to_string(schema.features.useFProperty) +
                                  " use_name_pool=" + std::to_string(schema.features.useNamePool) +
                                  " ffield_owner_mask=" + std::to_string(schema.features.fFieldOwnerMask) +
                                  " large_world_coordinates=" + std::to_string(schema.features.largeWorldCoordinates));
        if (schema.family == EngineFamily::Unknown)
        {
            report.failures.push_back("engine version is unknown");
            return report;
        }

        if (!ResolveUObjectSchema(schema, report) || !ValidateUObjectSchema(schema, report) ||
            !ResolveStructSchema(schema, report) || !ResolveFieldSchema(schema, report) ||
            !ResolvePropertySchema(schema, report) || !ResolvePropertySubtypes(schema, report) ||
            !ResolveFunctionSchema(schema, report) ||
            !ResolveEnumSchema(schema, report))
            return report;

        schema.validation.uobject = true;
        schema.validation.fname = binding_.names.IsValid();
        if (schema.features.useFProperty)
            schema.validation.fields = true;
        schema.validation.familyEvidence = version_.ToString();
        if (!schema.validation.properties || !schema.validation.functions || !schema.validation.enums)
        {
            report.failures.push_back("one or more schema semantic probes failed");
            return report;
        }
        report.accepted = schema.IsReadyForReflection();
        return report;
    }
} // namespace anduefker::ue
