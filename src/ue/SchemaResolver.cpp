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
                                   const EngineProfile &profile,
                                   SchemaProbeNames names)
        : memory_(memory), binding_(binding), profile_(profile), names_(std::move(names))
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

        // Outer 位于完整 FName 之后。ARM32 如果从 FName 内部开始探测，
        // 可能把 Number 读成大量空值组成的伪指针，因此评分前排除该区域。
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
        const int32_t pointerSize = static_cast<int32_t>(sizeof(uintptr_t));
        const int32_t nameEnd = schema.uobject.name + schema.fname.size;
        const int32_t firstOuterOffset = ((nameEnd + pointerSize - 1) / pointerSize) * pointerSize;
        const auto readObjectName = [&](uintptr_t object) -> std::optional<std::string>
        {
            const auto address = Add(object, schema.uobject.name);
            if (!address)
                return std::nullopt;
            int32_t rawName = 0;
            if (!memory_.Read(*address, rawName))
                return std::nullopt;
            rawName = binding_.decode.nameIndex(rawName, *address);
            return names.ReadName(rawName);
        };
        int32_t bestOuterOffset = -1;
        int32_t bestOuterScore = 0;
        size_t sourceOuterObjects = 0;
        size_t sourceOuterSampleHits = 0;
        int32_t sourceOuterScore = 0;
        for (int32_t offset = firstOuterOffset; offset <= 0x80; offset += pointerSize)
        {
            size_t validObjects = 0;
            size_t sampleHits = 0;
            size_t nullHits = 0;
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
                if (value == 0)
                {
                    ++nullHits;
                    continue;
                }
                if (value == object || !IsReadablePointer(memory_, value) || !readObjectName(value))
                    continue;
                ++validObjects;
                if (sampleObjects.contains(value))
                    ++sampleHits;
            }
            const int32_t score = static_cast<int32_t>(validObjects * 100 + sampleHits * 25 + nullHits);
            if (offset == firstOuterOffset)
            {
                sourceOuterObjects = validObjects;
                sourceOuterSampleHits = sampleHits;
                sourceOuterScore = score;
            }
            if (validObjects >= 3 && offset != schema.uobject.classPointer &&
                (score > bestOuterScore || (score == bestOuterScore && offset == firstOuterOffset)))
            {
                bestOuterScore = score;
                bestOuterOffset = offset;
            }
        }
        if (sourceOuterObjects < 3)
        {
            report.failures.push_back("UObject::Outer source-layout candidate was rejected; offset=" +
                                      std::to_string(firstOuterOffset) +
                                      " valid_objects=" + std::to_string(sourceOuterObjects) +
                                      " best_candidate=" + std::to_string(bestOuterOffset));
            return false;
        }
        schema.uobject.outer = firstOuterOffset;
        report.evidence.push_back("resolved UObject::Outer from source-layout candidate offset=" +
                                  std::to_string(firstOuterOffset) + " valid_objects=" +
                                  std::to_string(sourceOuterObjects) + " sampled_objects=" +
                                  std::to_string(sourceOuterSampleHits) + " score=" +
                                  std::to_string(sourceOuterScore) + " rejected_best_candidate=" +
                                  std::to_string(bestOuterOffset) + " rejected_score=" +
                                  std::to_string(bestOuterScore));

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
            const int32_t nextStart = schema.uobject.outer >= 0
                                          ? schema.uobject.outer + static_cast<int32_t>(sizeof(uintptr_t))
                                          : 0x20;
            for (int32_t offset = nextStart; offset <= 0x80; offset += 4)
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

        const int32_t pointerSize = static_cast<int32_t>(sizeof(uintptr_t));
        const int32_t ownerStorageSize = schema.features.fFieldOwnerMask ? pointerSize : pointerSize * 2;
        // 在此 ABI 和 Owner 表示形式下，FField 的字段声明顺序依次为：
        // 虚函数表指针、ClassPrivate、Owner、Next 以及 NamePrivate，
        // 在此优先采用源码定义的内存布局，然后再尝试有界备选方案。
        const std::array<int32_t, 7> nameOffsets = pointerSize == 4
                                                       ? std::array<int32_t, 7>{schema.features.fFieldOwnerMask ? 0x10 : 0x14,
                                                                                schema.features.fFieldOwnerMask ? 0x14 : 0x10, 0x18, 0x20, 0x28, 0x30, 0x38}
                                                       : std::array<int32_t, 7>{schema.features.fFieldOwnerMask ? 0x20 : 0x28,
                                                                                schema.features.fFieldOwnerMask ? 0x28 : 0x20, 0x30, 0x38, 0x18, 0x40, 0x48};
        const std::array<int32_t, 3> classOffsets = {pointerSize, pointerSize * 2, pointerSize * 3};
        const std::array<int32_t, 4> classNameOffsets = {0x00, 0x08, 0x10, 0x18};
        size_t readableRoots = 0;
        size_t namedRoots = 0;
        size_t chainedRoots = 0;
        size_t classMatches = 0;
        size_t ownerMatches = 0;

        for (int32_t propertiesOffset = 0x20; propertiesOffset <= 0x100; propertiesOffset += 4)
        {
            uintptr_t firstField = 0;
            const auto rootAddress = Add(*guid, propertiesOffset);
            if (!rootAddress || !memory_.Read(*rootAddress, firstField) || !IsReadablePointer(memory_, firstField))
                continue;
            ++readableRoots;

            for (const int32_t nameOffset : nameOffsets)
            {
                const auto firstName = readFieldName(firstField, nameOffset);
                if (!firstName || firstName->size() != 1 || (*firstName)[0] < 'A' || (*firstName)[0] > 'D')
                    continue;
                ++namedRoots;
                const int32_t nextOffset = nameOffset - pointerSize;
                const int32_t ownerOffset = nextOffset - ownerStorageSize;
                if (ownerOffset < pointerSize * 2)
                    continue;

                std::array<uintptr_t, 4> fields{};
                std::array<bool, 4> seenNames{};
                uintptr_t field = firstField;
                bool chainValid = true;
                for (size_t index = 0; index < fields.size(); ++index)
                {
                    const auto fieldName = field ? readFieldName(field, nameOffset) : std::nullopt;
                    if (!fieldName || fieldName->size() != 1 || (*fieldName)[0] < 'A' || (*fieldName)[0] > 'D' ||
                        seenNames[static_cast<size_t>((*fieldName)[0] - 'A')])
                    {
                        chainValid = false;
                        break;
                    }
                    seenNames[static_cast<size_t>((*fieldName)[0] - 'A')] = true;
                    fields[index] = field;
                    const auto nextAddress = Add(field, nextOffset);
                    if (!nextAddress || !memory_.Read(*nextAddress, field) ||
                        (index + 1 < fields.size() && !IsReadablePointer(memory_, field)))
                    {
                        chainValid = false;
                        break;
                    }
                }
                if (!chainValid || field != 0)
                    continue;
                ++chainedRoots;

                for (const int32_t classOffset : classOffsets)
                {
                    if (classOffset + pointerSize > ownerOffset)
                        continue;
                    for (const int32_t classNameOffset : classNameOffsets)
                    {
                        bool classesValid = true;
                        for (uintptr_t current : fields)
                        {
                            uintptr_t fieldClass = 0;
                            const auto classAddress = Add(current, classOffset);
                            if (!classAddress || !memory_.Read(*classAddress, fieldClass) ||
                                !IsReadablePointer(memory_, fieldClass))
                            {
                                classesValid = false;
                                break;
                            }
                            const auto className = readFieldName(fieldClass, classNameOffset);
                            if (!className || className->find("Property") == std::string::npos)
                            {
                                classesValid = false;
                                break;
                            }
                        }
                        if (!classesValid)
                            continue;
                        ++classMatches;

                        bool ownersValid = true;
                        for (uintptr_t current : fields)
                        {
                            uintptr_t rawOwner = 0;
                            const auto ownerAddress = Add(current, ownerOffset);
                            if (!ownerAddress || !memory_.Read(*ownerAddress, rawOwner) ||
                                (schema.features.fFieldOwnerMask && (rawOwner & 1u) == 0) ||
                                (schema.features.fFieldOwnerMask ? (rawOwner & ~static_cast<uintptr_t>(1)) : rawOwner) != *guid)
                            {
                                ownersValid = false;
                                break;
                            }
                            if (!schema.features.fFieldOwnerMask)
                            {
                                uint8_t isUObject = 0;
                                const auto discriminator = Add(*ownerAddress, pointerSize);
                                if (!discriminator || !memory_.Read(*discriminator, isUObject) || isUObject != 1)
                                {
                                    ownersValid = false;
                                    break;
                                }
                            }
                        }
                        if (!ownersValid)
                            continue;
                        ++ownerMatches;

                        schema.ustruct.childProperties = propertiesOffset;
                        schema.ffield.classPointer = classOffset;
                        schema.ffield.owner = ownerOffset;
                        schema.ffield.next = nextOffset;
                        schema.ffield.name = nameOffset;
                        schema.ffieldClass.name = classNameOffset;
                        schema.ffieldClass.castFlags = classNameOffset + pointerSize;
                        schema.validation.fields = true;
                        report.evidence.push_back("resolved FField chain from CoreUObject.Guid properties; child_properties=" +
                                                  std::to_string(propertiesOffset) + " class=" + std::to_string(classOffset) +
                                                  " owner=" + std::to_string(ownerOffset) + " next=" + std::to_string(nextOffset) +
                                                  " name=" + std::to_string(nameOffset) + " class_name=" +
                                                  std::to_string(classNameOffset) + "; four names, classes and owners validated");
                        return true;
                    }
                }
            }
        }
        report.failures.push_back("UStruct::ChildProperties/FField chain was not resolved; pointer_width=" +
                                  std::to_string(pointerSize) + " owner_mask=" +
                                  std::to_string(schema.features.fFieldOwnerMask) + " readable_roots=" +
                                  std::to_string(readableRoots) + " named_roots=" + std::to_string(namedRoots) +
                                  " four_field_chains=" + std::to_string(chainedRoots) +
                                  " class_matches=" + std::to_string(classMatches) +
                                  " owner_matches=" + std::to_string(ownerMatches));
        return false;
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

        if (!matchesUniform(propertyStart, 1))
        {
            report.failures.push_back("FProperty::ArrayDim source-layout candidate was rejected; offset=" +
                                      std::to_string(propertyStart));
            return false;
        }
        schema.property.arrayDim = propertyStart;

        const int32_t elementSizeOffset = propertyStart + static_cast<int32_t>(sizeof(int32_t));
        if (!matchesUniform(elementSizeOffset, 4))
        {
            report.failures.push_back("FProperty::ElementSize source-layout candidate was rejected; offset=" +
                                      std::to_string(elementSizeOffset));
            return false;
        }
        schema.property.elementSize = elementSizeOffset;

        // EPropertyFlags 位于 ArrayDim 和 ElementSize 之后，类型为 uint64_t。
        // 只在后续的对齐位置搜索，再从选中的 flags 字段之后定位 Offset_Internal。
        const int32_t flagsStart = propertyStart + static_cast<int32_t>(sizeof(int32_t) * 2);
        for (int32_t offset = flagsStart; offset <= 0x100 - static_cast<int32_t>(sizeof(uint64_t));
             offset += static_cast<int32_t>(sizeof(uint64_t)))
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

        for (int32_t offset = schema.property.propertyFlags + static_cast<int32_t>(sizeof(uint64_t));
             offset <= 0x100 - static_cast<int32_t>(sizeof(int32_t)); offset += 4)
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
            report.failures.push_back("FProperty::Offset_Internal was not resolved after PropertyFlags");
            return false;
        }

        schema.property.baseSize = schema.property.elementSize;
        schema.validation.properties = true;
        report.evidence.push_back("resolved property ArrayDim/ElementSize/Flags/Offset_Internal from Guid chain; array_dim=" +
                                  std::to_string(schema.property.arrayDim) + " element_size=" +
                                  std::to_string(schema.property.elementSize) + " flags=" +
                                  std::to_string(schema.property.propertyFlags) + " offset=" +
                                  std::to_string(schema.property.offsetInternal));
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
        std::vector<uintptr_t> delegateSamples;
        const auto isDelegatePropertyName = [](const std::string &name)
        {
            return name == "DelegateProperty" || name == "MulticastDelegateProperty" ||
                   name == "MulticastInlineDelegateProperty" || name == "MulticastSparseDelegateProperty";
        };
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
                if (isDelegatePropertyName(field.className) && delegateSamples.size() < 128)
                    delegateSamples.push_back(field.address);
                if (std::find(wanted.begin(), wanted.end(), field.className) != wanted.end() &&
                    !samples.contains(field.className))
                    samples.emplace(field.className, field.address);
            }
        }

        // FProperty 自身的链表字段和 RepNotifyFunc 位于所有具体属性负载之前，
        // 如果紧跟 PropertyFlags 开始探测 subtype，
        // 就会把 PropertyLinkNext/NextRef 等字段误认为 Array::Inner、Set::ElementProp或 Map::KeyProp。
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
            const auto readProbePointer = [&](uintptr_t field, int32_t offset, uintptr_t &value)
            {
                const auto address = Add(field, offset);
                return address && memory_.IsReadable(*address, sizeof(uintptr_t)) && memory_.Read(*address, value) && value != 0;
            };
            // 在所有的 Delegate 属性变体中，UE 5.6 都将 SignatureFunction 声明在紧跟 FProperty 基类之后的位置。
            // 因此，通过源码推导出的偏移量是我们在此时所需的唯一候选值。
            // 扫描后续的其他任意字段不仅会产生误报，还会导致不必要的远程读取失败。
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
            struct Parameter
            {
                uint64_t flags = 0;
                int32_t end = 0;
            };
            uintptr_t address = 0;
            bool parameterChainValid = false;
            std::vector<Parameter> properties;
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
                functions.push_back(FunctionSample{*object, false, {}});
        }
        if (functions.size() < 2)
        {
            report.failures.push_back("not enough UFunction samples");
            return false;
        }

        // 通过两个函数对象解析 UField::Next，
        // 下一个指针可能为空，因此在可用时，应利用前两个链表条目中的字段名称来进行解析。
        const int32_t nextStart = schema.uobject.outer >= 0
                                      ? schema.uobject.outer + static_cast<int32_t>(sizeof(uintptr_t))
                                      : 0x20;
        for (int32_t offset = nextStart; offset <= 0x100 && schema.ufield.next < 0; offset += 4)
        {
            size_t readable = 0;
            size_t named = 0;
            for (const FunctionSample &sample : functions)
            {
                uintptr_t next = 0;
                const auto address = Add(sample.address, offset);
                if (!address || !memory_.Read(*address, next))
                    continue;
                if (next == 0)
                {
                    ++readable;
                    continue;
                }
                if (!IsReadablePointer(memory_, next))
                    continue;
                ++readable;
                if (readObjectName(next))
                    ++named;
            }
            if (readable == functions.size() && named > 0)
                schema.ufield.next = offset;
        }
        if (schema.ufield.next < 0)
        {
            report.failures.push_back("UField::Next was not resolved from UFunction samples; scan_start=" +
                                      std::to_string(nextStart));
            return false;
        }

        ObjectModelReader model(memory_, binding_, schema);
        if (!model.Initialize())
        {
            report.failures.push_back("object model could not be initialized for UFunction parameter validation");
            return false;
        }

        // 记录每个函数的参数字段形状，
        // UE 将参数属性存放在 UStruct::ChildProperties 链中，NumParms 和 ParmsSize 必须与该链的独立解析结果一致。
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
                sample.properties.push_back({property->flags, static_cast<int32_t>(end)});
                current = field->nextAddress;
            }
            if (current != 0 || visited.size() > 256)
                valid = false;
            sample.parameterChainValid = valid;
        }

        // UE 按以下顺序声明 UFunction 自身字段：
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
            // 有参数的函数必须预留参数存储空间，没有参数的函数不能声明一个任意的非零大小。
            return (numParams == 0 && paramSize == 0) ||
                   (numParams != 0 && paramSize != 0);
        };

        const auto expectedParameters = [](const FunctionSample &sample, uint32_t flags)
        {
            int32_t count = 0;
            int32_t size = 0;
            for (const FunctionSample::Parameter &property : sample.properties)
            {
                if ((property.flags & 0x80ull) != 0) // CPF_Parm
                {
                    ++count;
                    size = property.end; // UE's InitializeDerivedMembers assigns, rather than takes the maximum.
                }
                else if ((flags & 0x00800000u) == 0 || (property.flags & 0x200ull) == 0) // FUNC_HasDefaults / CPF_ZeroConstructor
                    break;
            }
            return std::pair{count, size};
        };

        // PropertiesSize 是字段偏移，而不是 sizeof(UStruct)。
        // 这里只把它用作下界锚点，后面的参数链一致性检查才是候选字段的语义验证。
        int32_t functionDataStart = schema.ustruct.size >= 0 ? schema.ustruct.size + 4 : 0;
        functionDataStart = (functionDataStart + 3) & ~3;

        struct FunctionCandidate
        {
            int32_t offset = -1;
            size_t flagHits = 0;
            size_t shapeHits = 0;
            size_t countMismatches = 0;
            size_t sizeMismatches = 0;
            size_t bothMismatches = 0;
            size_t invalidShapes = 0;
        };
        FunctionCandidate best;
        FunctionCandidate source;
        size_t tiedCandidates = 0;
        size_t parameterChainSamples = 0;
        for (const FunctionSample &sample : functions)
            parameterChainSamples += sample.parameterChainValid ? 1u : 0u;
        // +88 只是 UE 5.6 的 UStruct 布局
        const int32_t sourceFunctionFlagsOffset = schema.ustruct.size >= 0 ? schema.ustruct.size + 88 : -1;
        for (int32_t offset = functionDataStart; offset <= 0x200; offset += 4)
        {
            FunctionCandidate candidate;
            candidate.offset = offset;
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

                ++candidate.flagHits;
                const auto [expectedCount, expectedSize] = expectedParameters(sample, flags);
                const bool countMatches = expectedCount <= 0xFF && numParams == expectedCount;
                const bool sizeMatches = expectedSize <= 0xFFFF && paramSize == expectedSize;
                if (!isPlausibleParameterShape(numParams, paramSize))
                    ++candidate.invalidShapes;
                else if (countMatches && sizeMatches)
                    ++candidate.shapeHits;
                else if (!countMatches && !sizeMatches)
                    ++candidate.bothMismatches;
                else if (!countMatches)
                    ++candidate.countMismatches;
                else
                    ++candidate.sizeMismatches;
            }

            if (offset == sourceFunctionFlagsOffset)
                source = candidate;

            if (candidate.shapeHits > best.shapeHits ||
                (candidate.shapeHits == best.shapeHits && candidate.flagHits > best.flagHits))
            {
                best = candidate;
                tiedCandidates = 1;
            }
            else if (candidate.shapeHits == best.shapeHits && candidate.flagHits == best.flagHits)
                ++tiedCandidates;
        }

        const size_t requiredFunctionHits = std::max<size_t>(4, (parameterChainSamples * 3) / 4);
        // 源码布局提示仅可在没有其他明确区别时用于打破平衡，绝不能盖过更优的语义证据。
        const bool sourceLayoutApplies = version_.major == 5 && version_.minor == 6 && sizeof(uintptr_t) == 8;
        const FunctionCandidate scannedBest = best;
        if (sourceLayoutApplies && source.offset >= 0 &&
            source.shapeHits == best.shapeHits && source.flagHits == best.flagHits)
            best = source;
        const std::string candidateSummary = " scanned_offset=" + std::to_string(scannedBest.offset) +
                                             " scanned_flags=" + std::to_string(scannedBest.flagHits) +
                                             " scanned_shapes=" + std::to_string(scannedBest.shapeHits) +
                                             " source_5_6_offset=" + std::to_string(source.offset) +
                                             " source_flags=" + std::to_string(source.flagHits) +
                                             " source_shapes=" + std::to_string(source.shapeHits) +
                                             " selected_offset=" + std::to_string(best.offset) +
                                             " tied_candidates=" + std::to_string(tiedCandidates);
        if (best.offset < 0 || best.flagHits < requiredFunctionHits || best.shapeHits < requiredFunctionHits ||
            (tiedCandidates > 1 && (!sourceLayoutApplies || best.offset != source.offset)))
        {
            std::string mismatchSamples;
            size_t reportedSamples = 0;
            for (size_t index = 0; index < functions.size() && reportedSamples < 3; ++index)
            {
                const FunctionSample &sample = functions[index];
                if (!sample.parameterChainValid || best.offset < 0)
                    continue;
                const auto flagsAddress = Add(sample.address, best.offset);
                const auto countAddress = Add(sample.address, best.offset + 4);
                const auto sizeAddress = Add(sample.address, best.offset + 6);
                uint32_t flags = 0;
                uint8_t count = 0;
                uint16_t size = 0;
                if (!flagsAddress || !countAddress || !sizeAddress ||
                    !memory_.Read(*flagsAddress, flags) || !memory_.Read(*countAddress, count) ||
                    !memory_.Read(*sizeAddress, size) || !isPlausibleFunctionFlags(flags))
                    continue;
                const auto [expectedCount, expectedSize] = expectedParameters(sample, flags);
                if (count == expectedCount && size == expectedSize)
                    continue;
                mismatchSamples += " sample_index=" + std::to_string(index) +
                                   " expected_count=" + std::to_string(expectedCount) +
                                   " actual_count=" + std::to_string(count) +
                                   " expected_size=" + std::to_string(expectedSize) +
                                   " actual_size=" + std::to_string(size);
                ++reportedSamples;
            }
            report.failures.push_back("UFunction::NumParms/ParmsSize remain unresolved; parameter_chain_samples=" +
                                      std::to_string(parameterChainSamples) + " required=" +
                                      std::to_string(requiredFunctionHits) + candidateSummary +
                                      " count_mismatch=" + std::to_string(best.countMismatches) +
                                      " size_mismatch=" + std::to_string(best.sizeMismatches) +
                                      " both_mismatch=" + std::to_string(best.bothMismatches) +
                                      " invalid_shape=" + std::to_string(best.invalidShapes) + mismatchSamples);
            return false;
        }
        schema.ufunction.functionFlags = best.offset;
        schema.ufunction.numParams = best.offset + 4;
        schema.ufunction.paramSize = best.offset + 6;
        report.evidence.push_back("resolved UFunction::FunctionFlags, NumParms and ParmsSize from multiple samples;" + candidateSummary);

        const std::vector<int32_t> nativeFunctionDeltas =
            schema.family == EngineFamily::UE4FProperty && sizeof(uintptr_t) == 4
                ? std::vector<int32_t>{0x14, 0x1C}
                : std::vector<int32_t>{0x18, 0x28, 0x30, 0x38};
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
            report.failures.push_back("UFunction::ExecFunction was not resolved; flags_offset=" +
                                      std::to_string(schema.ufunction.functionFlags) +
                                      " best_native_offset=" + std::to_string(bestNativeFunctionOffset) +
                                      " executable_hits=" + std::to_string(bestNativeFunctionHits) +
                                      candidateSummary);
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
        report.evidence.push_back("resolved UField::Next and UStruct::Children for UFunction reflection; next=" +
                                  std::to_string(schema.ufield.next) + " children=" +
                                  std::to_string(schema.ustruct.children));
        return true;
    }

    bool SchemaResolver::ResolveEnumSchema(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        ObjectStoreReader objects(memory_, binding_.objectRoot.address, binding_.objects, binding_.decode);
        if (!objects.Initialize())
            return false;
        NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                              schema.fname, schema.features);
        const auto readObjectName = [&](uintptr_t object) -> std::optional<std::string>
        {
            const auto address = Add(object, schema.uobject.name);
            if (!address)
                return std::nullopt;
            return names.ReadFName(*address);
        };
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
        const int32_t pointerSize = static_cast<int32_t>(sizeof(uintptr_t));
        const int32_t namesStart = std::max(pointerSize == 4 ? 0x20 : 0x30,
                                            schema.ufield.next >= 0 ? schema.ufield.next + pointerSize : 0);

        struct EnumCandidate
        {
            int32_t offset = -1;
            size_t validArrays = 0;
            size_t emptyArrays = 0;
            size_t invalidHeaders = 0;
            size_t unreadableNonEmpty = 0;
            size_t readableNonEmptyArrays = 0;
            size_t nonEmptyArrays = 0;
            size_t validEntries = 0;
            size_t nameFailures = 0;
            size_t duplicateNames = 0;
            size_t valueFailures = 0;
        };
        std::vector<EnumCandidate> candidates;
        for (int32_t offset = (namesStart + pointerSize - 1) / pointerSize * pointerSize;
             offset <= 0x180; offset += pointerSize)
        {
            EnumCandidate candidate;
            candidate.offset = offset;
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
                {
                    ++candidate.invalidHeaders;
                    continue;
                }
                if (count > 0 && (!IsReadablePointer(memory_, data) || !memory_.IsReadable(data, entryStride)))
                {
                    ++candidate.unreadableNonEmpty;
                    continue;
                }

                ++candidate.validArrays;
                if (count == 0)
                {
                    ++candidate.emptyArrays;
                    continue;
                }
                ++candidate.readableNonEmptyArrays;

                const int32_t sampleCount = std::min(count, 8);
                const int32_t requiredEntries = std::min(count, 4);
                int32_t decodedEntries = 0;
                std::unordered_set<std::string> seenNames;
                for (int32_t entryIndex = 0; entryIndex < sampleCount; ++entryIndex)
                {
                    const auto entry = Add(data, entryIndex * entryStride);
                    if (!entry)
                        break;
                    const auto entryName = names.ReadFName(*entry);
                    if (!entryName || entryName->empty())
                    {
                        ++candidate.nameFailures;
                        break;
                    }
                    if (!seenNames.insert(*entryName).second)
                    {
                        ++candidate.duplicateNames;
                        break;
                    }
                    const auto valueAddress = Add(*entry, valueOffset);
                    int64_t value = 0;
                    if (!valueAddress || !memory_.Read(*valueAddress, value))
                    {
                        ++candidate.valueFailures;
                        break;
                    }
                    ++decodedEntries;
                }
                candidate.validEntries += static_cast<size_t>(decodedEntries);
                if (decodedEntries >= requiredEntries)
                    ++candidate.nonEmptyArrays;
            }
            candidates.push_back(candidate);
        }

        std::sort(candidates.begin(), candidates.end(), [](const EnumCandidate &left, const EnumCandidate &right)
                  {
            if (left.nonEmptyArrays != right.nonEmptyArrays)
                return left.nonEmptyArrays > right.nonEmptyArrays;
            if (left.validEntries != right.validEntries)
                return left.validEntries > right.validEntries;
            if (left.validArrays != right.validArrays)
                return left.validArrays > right.validArrays;
            return left.offset < right.offset; });

        const auto describe = [](const EnumCandidate &candidate)
        {
            return " offset=" + std::to_string(candidate.offset) +
                   " headers=" + std::to_string(candidate.validArrays) +
                   " empty=" + std::to_string(candidate.emptyArrays) +
                   " invalid_header=" + std::to_string(candidate.invalidHeaders) +
                   " unreadable_nonempty=" + std::to_string(candidate.unreadableNonEmpty) +
                   " readable_nonempty=" + std::to_string(candidate.readableNonEmptyArrays) +
                   " decoded_nonempty=" + std::to_string(candidate.nonEmptyArrays) +
                   " entries=" + std::to_string(candidate.validEntries) +
                   " name_fail=" + std::to_string(candidate.nameFailures) +
                   " duplicate_name=" + std::to_string(candidate.duplicateNames) +
                   " value_fail=" + std::to_string(candidate.valueFailures);
        };
        std::string candidateSummary = " samples=" + std::to_string(enumObjects.size()) +
                                       " scan_start=" + std::to_string(namesStart) +
                                       " step=" + std::to_string(pointerSize) +
                                       " value_offset=" + std::to_string(valueOffset) +
                                       " entry_stride=" + std::to_string(entryStride);
        int32_t sourceOffset = -1;
        for (size_t index = 0; index < candidates.size() && index < 3; ++index)
            candidateSummary += " candidate[" + std::to_string(index) + "]" + describe(candidates[index]);
        if (pointerSize == 4 && schema.ufield.next >= 0)
        {
            sourceOffset = schema.ufield.next + pointerSize * 2 + 2 * static_cast<int32_t>(sizeof(int32_t));
            const auto source = std::find_if(candidates.begin(), candidates.end(), [sourceOffset](const EnumCandidate &candidate)
                                             { return candidate.offset == sourceOffset; });
            if (source != candidates.end())
                candidateSummary += " source_layout" + describe(*source);
        }

        const size_t requiredArrays = std::max<size_t>(4, enumObjects.size() / 2);
        const size_t requiredNonEmptyArrays = std::min<size_t>(4, enumObjects.size());
        const EnumCandidate *selected = candidates.empty() ? nullptr : &candidates[0];
        const bool sourceLayoutRequired = schema.family == EngineFamily::UE4FProperty &&
                                          pointerSize == 4 && sourceOffset >= 0;
        if (sourceLayoutRequired)
        {
            const auto source = std::find_if(candidates.begin(), candidates.end(), [sourceOffset](const EnumCandidate &candidate)
                                             { return candidate.offset == sourceOffset; });
            if (source == candidates.end() || source->validArrays < requiredArrays ||
                source->nonEmptyArrays < requiredNonEmptyArrays || source->validEntries == 0)
            {
                report.failures.push_back("UEnum::Names source-layout candidate was rejected; required_headers=" +
                                          std::to_string(requiredArrays) + " required_nonempty=" +
                                          std::to_string(requiredNonEmptyArrays) + " source_offset=" +
                                          std::to_string(sourceOffset) + " candidate=" +
                                          (source == candidates.end() ? std::string("missing") : describe(*source)) +
                                          candidateSummary);
                return false;
            }
            selected = &*source;
        }
        const bool ambiguous = !sourceLayoutRequired && candidates.size() > 1 &&
                               candidates[0].nonEmptyArrays == candidates[1].nonEmptyArrays &&
                               candidates[0].validEntries == candidates[1].validEntries &&
                               candidates[0].validArrays == candidates[1].validArrays;
        if (selected == nullptr || selected->validArrays < requiredArrays ||
            selected->nonEmptyArrays < requiredNonEmptyArrays || ambiguous)
        {
            report.failures.push_back("UEnum::Names was not resolved from FName/int64 array samples; required_headers=" +
                                      std::to_string(requiredArrays) + " required_nonempty=" +
                                      std::to_string(requiredNonEmptyArrays) + " ambiguous=" +
                                      std::to_string(ambiguous) + candidateSummary);
            return false;
        }
        schema.uenum.names = selected->offset;
        report.evidence.push_back("resolved UEnum::Names from FName/int64 entries; selected=" +
                                  std::to_string(selected->offset) + ";" + candidateSummary);

        // CppForm follows Names; UE4.25 has no EnumFlags, while later UE versions may include it.
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
            if (schema.features.enumHasFlags && enumFlagsAddress &&
                memory_.Read(*enumFlagsAddress, enumFlags) && (enumFlags & ~0x03u) == 0)
                ++flagHits;
        }
        if (formHits >= requiredNonEmptyArrays)
        {
            schema.uenum.cppForm = schema.uenum.names + static_cast<int32_t>(sizeof(uintptr_t) * 2);
            report.evidence.push_back("resolved UEnum::CppForm; hits=" + std::to_string(formHits));
        }
        if (schema.features.enumHasFlags && flagHits >= requiredNonEmptyArrays)
        {
            schema.uenum.flags = schema.uenum.cppForm >= 0 ? schema.uenum.cppForm + 1 : schema.uenum.names + static_cast<int32_t>(sizeof(uintptr_t) * 2) + 1;
            report.evidence.push_back("resolved UEnum::EnumFlags; hits=" + std::to_string(flagHits));
        }
        if (schema.features.enumHasFlags && schema.uenum.flags < 0)
        {
            report.failures.push_back("UEnum::EnumFlags was required by the structure profile but was not resolved; hits=" +
                                      std::to_string(flagHits) + " required=" +
                                      std::to_string(requiredNonEmptyArrays));
            return false;
        }
        if (schema.features.enumHasPackage)
        {
            if (schema.uenum.cppForm < 0)
            {
                report.failures.push_back("UEnum::CppForm was required to locate EnumPackage but was not resolved");
                return false;
            }
            const int32_t pointerAlignment = static_cast<int32_t>(sizeof(uintptr_t));
            const int32_t displayNameFunction = ((schema.uenum.cppForm + 2 + pointerAlignment - 1) /
                                                 pointerAlignment) *
                                                pointerAlignment;
            const int32_t packageOffset = displayNameFunction + pointerAlignment;
            size_t packageHits = 0;
            for (uintptr_t enumObject : enumObjects)
            {
                const auto packageAddress = Add(enumObject, packageOffset);
                const auto outerAddress = Add(enumObject, schema.uobject.outer);
                uintptr_t outer = 0;
                if (!packageAddress || !outerAddress || !memory_.Read(*outerAddress, outer))
                    continue;
                outer = binding_.decode.objectOuter(outer, *outerAddress);
                const auto packageName = names.ReadFName(*packageAddress);
                const auto outerName = outer == 0 ? std::optional<std::string>{} : readObjectName(outer);
                if (packageName && outerName && *packageName == *outerName)
                    ++packageHits;
            }
            if (packageHits < requiredNonEmptyArrays)
            {
                report.failures.push_back("UEnum::EnumPackage was required by the structure profile but was not resolved; offset=" +
                                          std::to_string(packageOffset) + " hits=" + std::to_string(packageHits) +
                                          " required=" + std::to_string(requiredNonEmptyArrays));
                return false;
            }
            schema.uenum.enumPackage = packageOffset;
            report.evidence.push_back("resolved UEnum::EnumPackage; offset=" + std::to_string(packageOffset) +
                                      " hits=" + std::to_string(packageHits));
        }
        schema.validation.enums = true;
        report.evidence.push_back(schema.features.enumUsesFNameData ? "UE5.6 FNameData enum family selected" : "legacy enum container family selected");
        return true;
    }

    SchemaResolutionReport SchemaResolver::Resolve(EngineSchema &schema) const
    {
        SchemaResolutionReport report;
        report.profileId = profile_.id;
        report.profileLabel = profile_.label;
        schema.family = profile_.family;
        schema.features = profile_.features;
        report.evidence.push_back("engine structure profile=" + profile_.id +
                                  " version_range=" + profile_.versionRange +
                                  " use_fproperty=" + std::to_string(schema.features.useFProperty) +
                                  " use_name_pool=" + std::to_string(schema.features.useNamePool) +
                                  " ffield_owner_mask=" + std::to_string(schema.features.fFieldOwnerMask) +
                                  " large_world_coordinates=" + std::to_string(schema.features.largeWorldCoordinates));
        if (!profile_.IsValid())
        {
            report.failures.push_back("engine structure profile is invalid");
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
        schema.validation.profileId = profile_.id;
        schema.validation.profileLabel = profile_.label;
        schema.validation.profileVersionRange = profile_.versionRange;
        schema.validation.familyEvidence = profile_.versionRange;
        if (!schema.validation.properties || !schema.validation.functions || !schema.validation.enums)
        {
            report.failures.push_back("one or more schema semantic probes failed");
            return report;
        }
        report.accepted = schema.IsReadyForReflection();
        if (report.accepted)
        {
            report.score = 100;
            if (schema.uobject.outer == schema.uobject.name + schema.fname.size)
                report.score += 10;
            if (schema.ufield.next == schema.uobject.outer + static_cast<int32_t>(sizeof(uintptr_t)))
                report.score += 10;
            if (schema.uenum.names >= 0 && schema.uenum.cppForm >= 0)
                report.score += 5;
            if (schema.features.enumHasFlags && schema.uenum.flags >= 0)
                report.score += 5;
            if (schema.features.enumHasPackage && schema.uenum.enumPackage >= 0)
                report.score += 5;
        }
        return report;
    }
} // namespace anduefker::ue
