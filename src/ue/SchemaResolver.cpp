#include "anduefker/ue/SchemaResolver.hpp"
#include "anduefker/ue/FunctionSemantics.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
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

        const char *EnumTailLayoutName(EnumTailLayout layout)
        {
            switch (layout)
            {
            case EnumTailLayout::Legacy:
                return "legacy";
            case EnumTailLayout::Flags:
                return "flags";
            case EnumTailLayout::FlagsDisplayNamePackage:
                return "flags-display-package";
            case EnumTailLayout::FlagsPackageDisplayName:
                return "flags-package-display";
            }
            return "unknown";
        }
    } // namespace

    std::shared_ptr<const SchemaProbeBootstrap> CreateSchemaProbeBootstrap(const IMemorySource &memory,
                                                                           const RuntimeBinding &binding,
                                                                           size_t maxSamples)
    {
        auto result = std::make_shared<SchemaProbeBootstrap>();
        auto objects = std::make_shared<ObjectStoreReader>(memory,
                                                           binding.objectRoot.address,
                                                           binding.objects,
                                                           binding.decode);
        if (!objects->Initialize())
        {
            result->failure = "object store could not be initialized for schema bootstrap";
            return result;
        }

        result->objectSamples.reserve(std::min<size_t>(maxSamples, static_cast<size_t>(objects->Count())));
        for (int32_t index = 0; index < objects->Count() && result->objectSamples.size() < maxSamples; ++index)
        {
            const auto object = objects->ReadObject(index);
            if (object.IsValid())
                result->objectSamples.emplace_back(index, object.address);
        }
        if (result->objectSamples.size() < 2)
        {
            result->failure = "not enough live UObject samples for schema bootstrap";
            return result;
        }

        result->objects = std::move(objects);
        return result;
    }

    SchemaResolver::SchemaResolver(const IMemorySource &memory,
                                   const RuntimeBinding &binding,
                                   const EngineProfile &profile,
                                   SchemaProbeNames names,
                                   uintptr_t moduleBase,
                                   uintptr_t moduleEnd,
                                   std::shared_ptr<const SchemaProbeBootstrap> bootstrap)
        : memory_(memory),
          binding_(binding),
          profile_(profile),
          names_(std::move(names)),
          moduleBase_(moduleBase),
          moduleEnd_(moduleEnd),
          bootstrap_(bootstrap ? std::move(bootstrap) : CreateSchemaProbeBootstrap(memory, binding))
    {
    }

    std::optional<uintptr_t> SchemaResolver::FindObjectByName(const EngineSchema &schema,
                                                              const std::string &name) const
    {
        if (name.empty())
            return std::nullopt;
        if (!bootstrap_ || !bootstrap_->IsValid())
            return std::nullopt;
        NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                              schema.fname, schema.features);
        for (int32_t index = 0; index < bootstrap_->objects->Count(); ++index)
        {
            const auto object = bootstrap_->objects->ReadObject(index);
            if (!object.IsValid())
                continue;
            const auto nameAddress = Add(object.address, schema.uobject.name);
            if (!nameAddress)
                continue;
            int32_t rawIndex = 0;
            if (!memory_.Read(*nameAddress, rawIndex))
                continue;
            rawIndex = binding_.decode.nameIndex(rawIndex, *nameAddress);
            const auto objectName = names.ReadName(rawIndex);
            if (objectName && *objectName == name)
                return object.address;
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
        if (!bootstrap_ || !bootstrap_->IsValid())
        {
            report.failures.push_back(bootstrap_ && !bootstrap_->failure.empty()
                                          ? bootstrap_->failure
                                          : "schema bootstrap is unavailable");
            return false;
        }

        const auto &samples = bootstrap_->objectSamples;

        schema.fname.comparisonIndex = 0;
        schema.fname.size = schema.features.outlineNumberName
                                ? (schema.features.casePreservingName ? 0x8 : 0x4)
                                : (schema.features.casePreservingName ? 0xC : 0x8);
        schema.fname.numberLayout = schema.features.outlineNumberName ? FNameNumberLayout::Outlined
                                                                      : FNameNumberLayout::Inline;
        schema.fname.displayLayout = schema.features.fnameDisplayLayout;
        schema.fname.displayIndex = schema.features.casePreservingName
                                        ? (schema.features.outlineNumberName
                                               ? 0x4
                                               : (schema.features.fnameDisplayLayout == FNameDisplayLayout::BeforeNumber ? 0x4 : 0x8))
                                        : -1;
        schema.fname.number = schema.features.outlineNumberName
                                  ? -1
                                  : (schema.features.fnameDisplayLayout == FNameDisplayLayout::BeforeNumber ? 0x8 : 0x4);

        const NameStoreReader bootstrapNames(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
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
            // FName 在 UE5 的 outline-number 配置下可以只有一个 4 字节的 ComparisonIndex
            // 因此候选必须按 uint32 对齐扫描，完整的 Name/Outer/内存关系验证负责排除半字段误报

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
                const auto name = bootstrapNames.ReadName(rawNameIndex);
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

        struct FNameLayoutCandidate
        {
            int32_t size = 0;
            bool casePreserving = false;
            bool outlineNumber = false;
            FNameDisplayLayout displayLayout = FNameDisplayLayout::None;
            int32_t validObjects = 0;
            int32_t sampleHits = 0;
            int32_t nullHits = 0;
            int32_t comparisonHits = 0;
            int32_t displayHits = 0;
            int32_t displayConsistentHits = 0;
            int32_t displayMismatchHits = 0;
            int32_t displayNoneHits = 0;
            int32_t numberReadable = 0;
            int32_t nonZeroNumberHits = 0;
            int32_t uniqueNames = 0;

            [[nodiscard]] FNamePhysicalLayout PhysicalLayout() const
            {
                const int32_t displayIndex = casePreserving
                                                 ? (outlineNumber || displayLayout == FNameDisplayLayout::BeforeNumber ? 0x4 : 0x8)
                                                 : -1;
                const int32_t number = outlineNumber
                                           ? -1
                                           : (displayLayout == FNameDisplayLayout::BeforeNumber ? 0x8 : 0x4);
                return FNamePhysicalLayout{size,
                                           0,
                                           number,
                                           displayIndex,
                                           outlineNumber ? FNameNumberLayout::Outlined : FNameNumberLayout::Inline};
            }
        };
        std::vector<FNameLayoutCandidate> fnameCandidates;
        const std::array<FNameLayoutCandidate, 5> layoutSeeds = {
            FNameLayoutCandidate{0x8, false, false, FNameDisplayLayout::None},
            FNameLayoutCandidate{0xC, true, false, FNameDisplayLayout::BeforeNumber},
            FNameLayoutCandidate{0xC, true, false, FNameDisplayLayout::AfterNumber},
            FNameLayoutCandidate{0x4, false, true, FNameDisplayLayout::None},
            FNameLayoutCandidate{0x8, true, true, FNameDisplayLayout::AfterNumber},
        };
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
        for (FNameLayoutCandidate candidate : layoutSeeds)
        {
            const int32_t nameEnd = schema.uobject.name + candidate.size;
            const int32_t outerOffset = (nameEnd + pointerSize - 1) / pointerSize * pointerSize;
            for (const auto &[index, object] : samples)
            {
                (void)index;
                const auto address = Add(object, outerOffset);
                if (!address)
                    continue;
                uintptr_t value = 0;
                if (!memory_.Read(*address, value))
                    continue;
                value = binding_.decode.objectOuter(value, *address);
                if (value == 0)
                {
                    ++candidate.nullHits;
                    continue;
                }
                if (value != object && IsReadablePointer(memory_, value) && sampleObjects.contains(value))
                {
                    ++candidate.validObjects;
                    ++candidate.sampleHits;
                }
                else if (value != object && IsReadablePointer(memory_, value))
                    ++candidate.validObjects;
            }

            FNameSchema candidateSchema;
            candidateSchema.comparisonIndex = 0;
            candidateSchema.size = candidate.size;
            candidateSchema.numberLayout = candidate.outlineNumber ? FNameNumberLayout::Outlined
                                                                   : FNameNumberLayout::Inline;
            candidateSchema.displayLayout = candidate.displayLayout;
            candidateSchema.displayIndex = candidate.casePreserving
                                               ? (candidate.outlineNumber ||
                                                          candidate.displayLayout == FNameDisplayLayout::BeforeNumber
                                                      ? 0x4
                                                      : 0x8)
                                               : -1;
            candidateSchema.number = candidate.outlineNumber
                                         ? -1
                                         : (candidate.displayLayout == FNameDisplayLayout::BeforeNumber ? 0x8 : 0x4);
            EngineFeatures candidateFeatures = schema.features;
            candidateFeatures.casePreservingName = candidate.casePreserving;
            candidateFeatures.outlineNumberName = candidate.outlineNumber;
            candidateFeatures.fnameDisplayLayout = candidate.displayLayout;
            const NameStoreReader candidateNames(memory_, binding_.nameRoot.address, binding_.names,
                                                 binding_.decode, candidateSchema, candidateFeatures);
            std::unordered_set<std::string> candidateNamesSeen;
            for (const auto &[index, object] : samples)
            {
                (void)index;
                const auto address = Add(object, schema.uobject.name);
                if (!address)
                    continue;

                int32_t rawComparisonIndex = 0;
                if (!memory_.Read(*address, rawComparisonIndex))
                    continue;
                rawComparisonIndex = binding_.decode.nameIndex(rawComparisonIndex, *address);
                const auto comparisonName = candidateNames.ReadName(rawComparisonIndex);
                if (comparisonName && !comparisonName->empty())
                {
                    ++candidate.comparisonHits;
                    candidateNamesSeen.insert(*comparisonName);
                }

                if (candidateSchema.displayIndex >= 0)
                {
                    const auto displayAddress = Add(*address, candidateSchema.displayIndex);
                    int32_t rawDisplayIndex = 0;
                    if (displayAddress && memory_.Read(*displayAddress, rawDisplayIndex))
                    {
                        rawDisplayIndex = binding_.decode.nameIndex(rawDisplayIndex, *displayAddress);
                        const auto displayName = candidateNames.ReadName(rawDisplayIndex);
                        if (displayName && !displayName->empty())
                        {
                            ++candidate.displayHits;
                            if (*displayName == "None")
                                ++candidate.displayNoneHits;
                            else if (displayName == comparisonName)
                                ++candidate.displayConsistentHits;
                            else
                                ++candidate.displayMismatchHits;
                        }
                    }
                }

                if (candidateSchema.number >= 0)
                {
                    const auto numberAddress = Add(*address, candidateSchema.number);
                    uint32_t number = 0;
                    if (numberAddress && memory_.Read(*numberAddress, number))
                    {
                        ++candidate.numberReadable;
                        if (number != 0)
                            ++candidate.nonZeroNumberHits;
                    }
                }
            }
            candidate.uniqueNames = static_cast<int32_t>(candidateNamesSeen.size());
            if (candidate.validObjects >= 3)
                fnameCandidates.push_back(candidate);
        }
        if (fnameCandidates.empty())
        {
            report.failures.push_back("FName layout candidate did not produce a valid UObject::Outer relation");
            return false;
        }
        std::vector<FNameLayoutCandidate> uniqueFNameCandidates;
        for (const FNameLayoutCandidate &candidate : fnameCandidates)
        {
            const auto existing = std::find_if(uniqueFNameCandidates.begin(), uniqueFNameCandidates.end(),
                                               [&](const FNameLayoutCandidate &other)
                                               { return candidate.PhysicalLayout() == other.PhysicalLayout(); });
            if (existing == uniqueFNameCandidates.end())
                uniqueFNameCandidates.push_back(candidate);
            else if (candidate.comparisonHits > existing->comparisonHits ||
                     (candidate.comparisonHits == existing->comparisonHits &&
                      candidate.displayConsistentHits > existing->displayConsistentHits))
                *existing = candidate;
        }
        fnameCandidates = std::move(uniqueFNameCandidates);

        const auto fnameScore = [](const FNameLayoutCandidate &candidate)
        {
            return candidate.validObjects * 100 + candidate.sampleHits * 25 + candidate.comparisonHits * 10 +
                   candidate.uniqueNames * 5 + candidate.displayConsistentHits * 2 + candidate.displayHits +
                   candidate.nullHits;
        };
        const auto betterFNameEvidence = [](const FNameLayoutCandidate &left, const FNameLayoutCandidate &right)
        {
            if (left.comparisonHits != right.comparisonHits)
                return left.comparisonHits > right.comparisonHits;
            if (left.uniqueNames != right.uniqueNames)
                return left.uniqueNames > right.uniqueNames;
            if (left.displayConsistentHits != right.displayConsistentHits)
                return left.displayConsistentHits > right.displayConsistentHits;
            if (left.displayMismatchHits != right.displayMismatchHits)
                return left.displayMismatchHits < right.displayMismatchHits;
            if (left.displayNoneHits != right.displayNoneHits)
                return left.displayNoneHits < right.displayNoneHits;
            if (left.displayHits != right.displayHits)
                return left.displayHits > right.displayHits;
            if (left.numberReadable != right.numberReadable)
                return left.numberReadable > right.numberReadable;
            return left.nonZeroNumberHits > right.nonZeroNumberHits;
        };
        std::sort(fnameCandidates.begin(), fnameCandidates.end(), [&](const FNameLayoutCandidate &left, const FNameLayoutCandidate &right)
                  {
                      const int leftScore = fnameScore(left);
                      const int rightScore = fnameScore(right);
                      return leftScore != rightScore ? leftScore > rightScore : betterFNameEvidence(left, right); });
        const auto hasConsistentDisplayEvidence = [](const FNameLayoutCandidate &candidate)
        {
            return candidate.displayConsistentHits > 0 && candidate.displayMismatchHits == 0 &&
                   candidate.displayNoneHits == 0;
        };
        const auto nonCasePreserving = std::find_if(fnameCandidates.begin(), fnameCandidates.end(),
                                                    [](const FNameLayoutCandidate &candidate)
                                                    { return !candidate.casePreserving; });
        const auto casePreserving = std::find_if(fnameCandidates.begin(), fnameCandidates.end(),
                                                 hasConsistentDisplayEvidence);
        if (nonCasePreserving != fnameCandidates.end() &&
            (casePreserving == fnameCandidates.end() ||
             fnameScore(*nonCasePreserving) >= fnameScore(*casePreserving)))
        {
            const FNameLayoutCandidate selected = *nonCasePreserving;
            fnameCandidates.erase(nonCasePreserving);
            fnameCandidates.insert(fnameCandidates.begin(), selected);
        }
        const auto sameFNameEvidence = [](const FNameLayoutCandidate &left, const FNameLayoutCandidate &right)
        {
            return left.validObjects == right.validObjects && left.sampleHits == right.sampleHits &&
                   left.nullHits == right.nullHits && left.comparisonHits == right.comparisonHits &&
                   left.displayHits == right.displayHits && left.displayConsistentHits == right.displayConsistentHits &&
                   left.displayMismatchHits == right.displayMismatchHits && left.displayNoneHits == right.displayNoneHits &&
                   left.numberReadable == right.numberReadable && left.nonZeroNumberHits == right.nonZeroNumberHits &&
                   left.uniqueNames == right.uniqueNames;
        };
        if (fnameCandidates.size() > 1 && sameFNameEvidence(fnameCandidates[0], fnameCandidates[1]))
        {
            report.failures.push_back("FName layout candidates are ambiguous; top_score=" +
                                      std::to_string(fnameScore(fnameCandidates[0])) +
                                      " distinct_physical_layouts=" + std::to_string(fnameCandidates.size()) +
                                      " evidence_equal=1");
            return false;
        }
        const FNameLayoutCandidate &selectedFNameCandidate = fnameCandidates.front();
        const FNameLayoutCandidate selectedFName = selectedFNameCandidate;
        schema.features.casePreservingName = selectedFName.casePreserving;
        schema.features.outlineNumberName = selectedFName.outlineNumber;
        schema.features.fnameDisplayLayout = selectedFName.displayLayout;
        schema.fname.size = selectedFName.size;
        schema.fname.numberLayout = selectedFName.outlineNumber ? FNameNumberLayout::Outlined
                                                                : FNameNumberLayout::Inline;
        schema.fname.displayLayout = selectedFName.displayLayout;
        schema.fname.displayIndex = selectedFName.casePreserving
                                        ? (selectedFName.outlineNumber || selectedFName.displayLayout == FNameDisplayLayout::BeforeNumber
                                               ? 0x4
                                               : 0x8)
                                        : -1;
        schema.fname.number = selectedFName.outlineNumber
                                  ? -1
                                  : (selectedFName.displayLayout == FNameDisplayLayout::BeforeNumber ? 0x8 : 0x4);
        const NameStoreReader names(memory_, binding_.nameRoot.address, binding_.names, binding_.decode,
                                    schema.fname, schema.features);
        report.evidence.push_back("resolved FName layout; size=" + std::to_string(schema.fname.size) +
                                  " case_preserving=" + std::to_string(schema.features.casePreservingName) +
                                  " outline_number=" + std::to_string(schema.features.outlineNumberName) +
                                  " display_layout=" +
                                  std::to_string(static_cast<int>(schema.features.fnameDisplayLayout)) +
                                  " outer_valid=" + std::to_string(selectedFName.validObjects) +
                                  " outer_samples=" + std::to_string(selectedFName.sampleHits) +
                                  " comparison_hits=" + std::to_string(selectedFName.comparisonHits) +
                                  " display_hits=" + std::to_string(selectedFName.displayHits) +
                                  " display_consistent=" + std::to_string(selectedFName.displayConsistentHits) +
                                  " display_mismatch=" + std::to_string(selectedFName.displayMismatchHits) +
                                  " display_none=" + std::to_string(selectedFName.displayNoneHits) +
                                  " number_readable=" + std::to_string(selectedFName.numberReadable) +
                                  " unique_names=" + std::to_string(selectedFName.uniqueNames));

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

        // Outer 位于完整 FName 之后。ARM32 如果从 FName 内部开始探测
        // 可能把 Number 读成大量空值组成的伪指针，因此评分前排除该区域
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
            for (int32_t index = 0; index < bootstrap_->objects->Count() && flagObjects.size() < 256; ++index)
            {
                const auto object = bootstrap_->objects->ReadObject(index);
                if (object.IsValid())
                    flagObjects.push_back(object.address);
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
                size_t readablePointerCandidates = 0;
                size_t namedCandidates = 0;
                std::string candidateNames;
                int32_t selectedOffset = -1;
                for (int32_t offset = 0x20; offset <= 0x100; offset += 4)
                {
                    uintptr_t field = 0;
                    const auto address = Add(structure, offset);
                    if (!address || !memory_.Read(*address, field) || !IsReadablePointer(memory_, field))
                        continue;
                    ++readablePointerCandidates;
                    const auto fieldName = readObjectName(field);
                    if (fieldName && (*fieldName == "A" || *fieldName == "B" || *fieldName == "C" ||
                                      *fieldName == "D" || *fieldName == "R"))
                    {
                        ++namedCandidates;
                        if (candidateNames.empty())
                            candidateNames = " offset=" + std::to_string(offset) + " name=" + *fieldName;
                        if (selectedOffset < 0)
                            selectedOffset = offset;
                    }
                }
                if (selectedOffset < 0)
                    report.evidence.push_back("UProperty Children probe failed; readable_pointer_candidates=" +
                                              std::to_string(readablePointerCandidates) +
                                              " named_candidates=" + std::to_string(namedCandidates) +
                                              " first_candidate=" + (candidateNames.empty() ? "none" : candidateNames));
                else
                    report.evidence.push_back("UProperty Children probe candidate; readable_pointer_candidates=" +
                                              std::to_string(readablePointerCandidates) +
                                              " named_candidates=" + std::to_string(namedCandidates) +
                                              " selected_candidate=" + std::to_string(selectedOffset) +
                                              " first_candidate=" + candidateNames);
                if (selectedOffset >= 0)
                    return selectedOffset;
                if (selectedOffset < 0)
                    report.evidence.push_back("UProperty Children probe failed; no named candidate selected");
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
        const bool taggedOwner = schema.features.fFieldOwnerEncoding == FFieldOwnerEncoding::TaggedPointer;
        const int32_t ownerStorageSize = taggedOwner ? pointerSize : pointerSize * 2;
        // 在此 ABI 和 Owner 表示形式下，FField 的字段声明顺序依次为：
        // 虚函数表指针、ClassPrivate、Owner、Next 以及 NamePrivate
        // 在此优先采用源码定义的内存布局，然后再尝试有界备选方案
        const std::array<int32_t, 7> nameOffsets = pointerSize == 4
                                                       ? std::array<int32_t, 7>{taggedOwner ? 0x10 : 0x14,
                                                                                taggedOwner ? 0x14 : 0x10, 0x18, 0x20, 0x28, 0x30, 0x38}
                                                       : std::array<int32_t, 7>{taggedOwner ? 0x20 : 0x28,
                                                                                taggedOwner ? 0x28 : 0x20, 0x30, 0x38, 0x18, 0x40, 0x48};
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
                            if (!className || !IsPropertyFieldKind(FieldKindFromRuntimeName(*className, true)))
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
                                (taggedOwner && (rawOwner & 1u) == 0) ||
                                (taggedOwner ? (rawOwner & ~static_cast<uintptr_t>(1)) : rawOwner) != *guid)
                            {
                                ownersValid = false;
                                break;
                            }
                            if (!taggedOwner)
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
                        const int32_t fnameSize = schema.fname.size > 0 ? schema.fname.size : pointerSize;
                        const auto alignOffset = [](int32_t offset, int32_t alignment)
                        {
                            return (offset + alignment - 1) / alignment * alignment;
                        };
                        const int32_t candidateId = alignOffset(classNameOffset + fnameSize, pointerSize);
                        const int32_t candidateCastFlags = candidateId + static_cast<int32_t>(sizeof(uint64_t));
                        const int32_t candidateClassFlags = candidateCastFlags + static_cast<int32_t>(sizeof(uint64_t));
                        const int32_t candidateSuperClass = alignOffset(
                            candidateClassFlags + static_cast<int32_t>(sizeof(uint32_t)), pointerSize);
                        size_t classTailHits = 0;
                        for (uintptr_t current : fields)
                        {
                            uint64_t id = 0;
                            uint64_t castFlags = 0;
                            uint32_t classFlags = 0;
                            uintptr_t superClass = 0;
                            uintptr_t fieldClass = 0;
                            const auto fieldClassAddress = Add(current, classOffset);
                            if (!fieldClassAddress || !memory_.Read(*fieldClassAddress, fieldClass) ||
                                !IsReadablePointer(memory_, fieldClass))
                                continue;
                            const auto idAddress = Add(fieldClass, candidateId);
                            const auto castAddress = Add(fieldClass, candidateCastFlags);
                            const auto flagsAddress = Add(fieldClass, candidateClassFlags);
                            const auto superAddress = Add(fieldClass, candidateSuperClass);
                            if (idAddress && castAddress && flagsAddress && superAddress &&
                                memory_.Read(*idAddress, id) && memory_.Read(*castAddress, castFlags) &&
                                memory_.Read(*flagsAddress, classFlags) && memory_.Read(*superAddress, superClass) &&
                                id != 0 && (castFlags & 0x0000000000008000ull) != 0 &&
                                superClass != 0 &&
                                IsReadablePointer(memory_, superClass))
                                ++classTailHits;
                        }
                        if (classTailHits != fields.size())
                            continue;
                        schema.ffieldClass.id = candidateId;
                        schema.ffieldClass.castFlags = candidateCastFlags;
                        schema.ffieldClass.classFlags = candidateClassFlags;
                        schema.ffieldClass.superClass = candidateSuperClass;
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
                                  std::to_string(taggedOwner) + " readable_roots=" +
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
        // Equal semantic evidence at equal source-layout priority cannot identify an offset.
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

    bool SchemaResolver::ResolvePropertySubtypes(EngineSchema &schema, SchemaResolutionReport &report) const
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

    bool SchemaResolver::ResolveFunctionSchema(EngineSchema &schema, SchemaResolutionReport &report) const
    {
        if (!bootstrap_ || !bootstrap_->IsValid())
        {
            report.failures.push_back("schema bootstrap unavailable for function schema");
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
            FieldKind kind = FieldKind::Unknown;
            std::string className;
            std::string name;
            uint32_t objectFlags = 0;
            bool parameterChainValid = false;
            ProbeSampleState parameterChainState = ProbeSampleState::NotObserved;
            FieldChainStatus chainStatus = FieldChainStatus::Empty;
            std::vector<PropertyMetadata> properties;
        };

        std::vector<FunctionSample> functions;
        ObjectModelReader model(memory_, binding_, schema);
        if (!model.Initialize())
        {
            report.failures.push_back("object model could not be initialized for UFunction parameter validation");
            return false;
        }
        size_t excludedDefaultObjects = 0;
        size_t excludedLoadingObjects = 0;
        size_t unreadableSampleFlags = 0;
        std::unordered_map<std::string, size_t> classSampleCounts;
        const auto sampleLimitFor = [](const std::string &className)
        {
            return NormalizeRuntimeFieldName(className) == "Function" ? size_t{48} : size_t{16};
        };
        for (int32_t index = 0; index < bootstrap_->objects->Count() && functions.size() < 96; ++index)
        {
            const auto object = bootstrap_->objects->ReadObject(index);
            if (!object.IsValid())
                continue;
            const auto className = model.ClassName(object.address);
            if (!className)
                continue;
            const FieldKind kind = FieldKindFromRuntimeName(*className, false);
            if (!IsFunctionFieldKind(kind))
                continue;
            // CDO 属于 UFunction 的实例，但它们并不是已被链接的函数定义
            // 采样资格不可依赖于候选的 FunctionFlags 偏移量
            const auto objectFlags = model.Flags(object.address);
            if (!objectFlags)
            {
                ++unreadableSampleFlags;
                continue;
            }
            if ((*objectFlags & kRFClassDefaultObject) != 0)
            {
                ++excludedDefaultObjects;
                if (excludedDefaultObjects <= 8)
                    report.evidence.push_back("excluded UFunction CDO address=" + std::to_string(object.address) +
                                              " class=" + *className + " object_flags=" + std::to_string(*objectFlags));
                continue;
            }
            if ((*objectFlags & kRFIncompleteLoad) != 0)
            {
                ++excludedLoadingObjects;
                continue;
            }
            const auto objectName = model.Name(object.address);
            if (!objectName)
                continue;
            const size_t limit = sampleLimitFor(*className);
            if (classSampleCounts[*className] >= limit)
                continue;
            ++classSampleCounts[*className];
            functions.push_back(FunctionSample{object.address,
                                               kind,
                                               *className,
                                               *objectName,
                                               *objectFlags,
                                               false,
                                               ProbeSampleState::NotObserved,
                                               FieldChainStatus::Empty,
                                               {}});
        }
        const std::string sampleSummary = " samples=" + std::to_string(functions.size()) +
                                          " excluded_cdo=" + std::to_string(excludedDefaultObjects) +
                                          " excluded_loading=" + std::to_string(excludedLoadingObjects) +
                                          " unreadable_object_flags=" + std::to_string(unreadableSampleFlags);
        report.evidence.push_back("UFunction sample eligibility;" + sampleSummary);
        if (functions.size() < 2)
        {
            report.failures.push_back("not enough UFunction definition samples;" + sampleSummary);
            return false;
        }

        // 通过两个函数对象解析 UField::Next
        // 下一个指针（Next）可能为空，因此在可用时，应利用前两个链表条目中的字段名称来进行解析
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

        // 记录每个函数的参数字段形状
        // 基于 UObject 的字段与 FField 采用不同的 UStruct 属性链表
        // 当前的生效链表是根据已验证的字段系统候选方案动态选择的，而非依赖版本标签
        const int32_t propertyChainOffset = schema.features.useFProperty ? schema.ustruct.childProperties
                                                                         : schema.ustruct.children;
        for (FunctionSample &sample : functions)
        {
            const auto firstAddress = Add(sample.address, propertyChainOffset);
            uintptr_t current = 0;
            if (!firstAddress || !memory_.Read(*firstAddress, current))
            {
                sample.parameterChainState = ProbeSampleState::Unreadable;
                continue;
            }

            if (current == 0)
            {
                sample.parameterChainValid = true;
                sample.parameterChainState = ProbeSampleState::SemanticMatch;
                sample.chainStatus = FieldChainStatus::Empty;
                continue;
            }

            const FieldChainResult chain = model.FieldsWithStatus(current, 256);
            sample.chainStatus = chain.status;
            bool valid = chain.Complete();
            for (const FieldMetadata &field : chain.fields)
            {
                if (schema.features.useFProperty &&
                    (!field.ownerIsUObject || field.ownerAddress == 0 || field.ownerAddress != sample.address))
                {
                    valid = false;
                    sample.parameterChainState = ProbeSampleState::SemanticMismatch;
                    break;
                }
                if (!IsPropertyFieldKind(field.kind))
                    continue;
                const auto property = model.Property(field.address);
                if (!property || property->arrayDim <= 0 || property->elementSize <= 0 || property->offset < 0)
                {
                    valid = false;
                    sample.parameterChainState = ProbeSampleState::SemanticMismatch;
                    break;
                }
                sample.properties.push_back(*property);
            }
            sample.parameterChainValid = valid && chain.Complete();
            if (sample.parameterChainValid)
                sample.parameterChainState = ProbeSampleState::SemanticMatch;
            else if (sample.parameterChainState == ProbeSampleState::NotObserved)
                sample.parameterChainState = ProbeSampleState::Unreadable;
        }
        size_t reportedInvalidChains = 0;
        for (const FunctionSample &sample : functions)
        {
            if (!sample.parameterChainValid && reportedInvalidChains < 8)
            {
                report.evidence.push_back("UFunction parameter chain rejected address=" + std::to_string(sample.address) +
                                          " function=" + sample.name + " class=" + sample.className +
                                          " chain_status=" + std::to_string(static_cast<int>(sample.chainStatus)) +
                                          " sample_state=" + std::to_string(static_cast<int>(sample.parameterChainState)));
                ++reportedInvalidChains;
            }
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
            // 有参数的函数必须预留参数存储空间，没有参数的函数不能声明一个任意的非零大小
            return (numParams == 0 && paramSize == 0) ||
                   (numParams != 0 && paramSize != 0);
        };

        // PropertiesSize 是字段偏移，而不是 sizeof(UStruct)
        // 这里只把它用作下界锚点，后面的参数链一致性检查才是候选字段的语义验证
        const int32_t pointerSize = static_cast<int32_t>(sizeof(uintptr_t));
        int32_t functionDataStart = schema.ustruct.propertiesSizeOffset >= 0
                                        ? schema.ustruct.propertiesSizeOffset + static_cast<int32_t>(sizeof(int32_t))
                                        : 0;
        functionDataStart = (functionDataStart + 3) & ~3;

        // UFunction::Func 紧跟在固定的参数字段之后，且其前方可能会存在可选的 Event-Graph 或 Live-Coding 成员
        // 将此结构保留为一组源码兼容的偏移量增量布局族，并由可执行指针证据在运行时选择生效的变体
        const std::vector<int32_t> nativeFunctionDeltas =
            pointerSize == 4
                ? std::vector<int32_t>{0x14, 0x1C, 0x20, 0x28}
                : std::vector<int32_t>{0x18, 0x20, 0x28, 0x30, 0x38};

        struct FunctionObservation
        {
            bool readable = false;
            bool flagsPlausible = false;
            uint32_t flags = 0;
            uint8_t numParams = 0;
            uint16_t paramSize = 0;
            uint16_t returnOffset = 0;
            FunctionParameterSummary summary;
            bool shapeMatches = false;
            bool returnMatches = false;
        };
        struct FunctionCandidate
        {
            int32_t offset = -1;
            size_t headerReadable = 0;
            size_t flagHits = 0;
            size_t shapeHits = 0;
            size_t returnHits = 0;
            size_t flagFailures = 0;
            size_t shapeFailures = 0;
            size_t returnFailures = 0;
            size_t countMismatches = 0;
            size_t sizeMismatches = 0;
            size_t returnMismatches = 0;
            size_t bothMismatches = 0;
            size_t invalidShapes = 0;
            size_t parameterizedSamples = 0;
            size_t returningSamples = 0;
            std::vector<FunctionObservation> observations;
        };
        FunctionCandidate best;
        FunctionCandidate bestObserved;
        size_t equivalentHeaderCandidates = 0;
        size_t parameterChainSamples = 0;
        for (const FunctionSample &sample : functions)
            parameterChainSamples += sample.parameterChainValid ? 1u : 0u;
        for (const auto &[className, count] : classSampleCounts)
            report.evidence.push_back("UFunction sample class=" + className + " count=" + std::to_string(count));

        const size_t minimumHeaderSamples = std::max<size_t>(4, parameterChainSamples / 2);

        const auto betterHeaderCandidate = [](const FunctionCandidate &left, const FunctionCandidate &right)
        {
            if (left.shapeHits != right.shapeHits)
                return left.shapeHits > right.shapeHits;
            if (left.flagHits != right.flagHits)
                return left.flagHits > right.flagHits;
            if (left.returnHits != right.returnHits)
                return left.returnHits > right.returnHits;
            if (left.invalidShapes != right.invalidShapes)
                return left.invalidShapes < right.invalidShapes;
            if (left.bothMismatches != right.bothMismatches)
                return left.bothMismatches < right.bothMismatches;
            if (left.countMismatches != right.countMismatches)
                return left.countMismatches < right.countMismatches;
            if (left.sizeMismatches != right.sizeMismatches)
                return left.sizeMismatches < right.sizeMismatches;
            if (left.headerReadable != right.headerReadable)
                return left.headerReadable > right.headerReadable;
            return false;
        };
        const auto isHardValidHeaderCandidate = [&](const FunctionCandidate &candidate)
        {
            return candidate.offset >= 0 && candidate.headerReadable >= minimumHeaderSamples &&
                   candidate.flagHits == candidate.headerReadable &&
                   candidate.shapeHits == candidate.headerReadable &&
                   candidate.returnHits == candidate.headerReadable &&
                   candidate.invalidShapes == 0 && candidate.countMismatches == 0 &&
                   candidate.sizeMismatches == 0 && candidate.bothMismatches == 0 &&
                   candidate.returnMismatches == 0;
        };
        for (int32_t offset = functionDataStart; offset <= 0x200; offset += 4)
        {
            FunctionCandidate candidate;
            candidate.offset = offset;
            candidate.observations.resize(functions.size());
            for (size_t index = 0; index < functions.size(); ++index)
            {
                const FunctionSample &sample = functions[index];
                if (!sample.parameterChainValid)
                    continue;

                FunctionObservation &observation = candidate.observations[index];
                const auto flagsAddress = Add(sample.address, offset);
                std::array<uint8_t, 10> header{};
                if (!flagsAddress || !memory_.ReadBytes(*flagsAddress, header.data(), header.size()).Ok())
                    continue;
                observation.readable = true;
                std::memcpy(&observation.flags, header.data(), sizeof(observation.flags));
                observation.numParams = header[4];
                std::memcpy(&observation.paramSize, header.data() + 6, sizeof(observation.paramSize));
                std::memcpy(&observation.returnOffset, header.data() + 8, sizeof(observation.returnOffset));
                const uint32_t flags = observation.flags;
                const uint8_t numParams = observation.numParams;
                const uint16_t paramSize = observation.paramSize;
                observation.flagsPlausible = isPlausibleFunctionFlags(flags);
                observation.summary = AnalyzeFunctionParameters(
                    sample.properties, flags, schema.features.functionDefaultsContinueAfterInitializer);
                ++candidate.headerReadable;
                if (!observation.flagsPlausible)
                {
                    ++candidate.flagFailures;
                    continue;
                }

                ++candidate.flagHits;
                const FunctionParameterSummary &summary = observation.summary;
                const bool countMatches = summary.valid && summary.count <= 0xFF && numParams == summary.count;
                const bool sizeMatches = summary.valid && summary.paramEnd >= 0 &&
                                         summary.paramEnd <= 0xFFFF && paramSize == summary.paramEnd;
                if (!isPlausibleParameterShape(numParams, paramSize))
                {
                    ++candidate.invalidShapes;
                    ++candidate.shapeFailures;
                }
                else if (countMatches && sizeMatches)
                {
                    observation.shapeMatches = true;
                    ++candidate.shapeHits;
                    candidate.parameterizedSamples += summary.count != 0 ? 1u : 0u;
                    candidate.returningSamples += summary.returnOffset >= 0 ? 1u : 0u;
                }
                else if (!countMatches && !sizeMatches)
                {
                    ++candidate.bothMismatches;
                    ++candidate.shapeFailures;
                }
                else if (!countMatches)
                {
                    ++candidate.countMismatches;
                    ++candidate.shapeFailures;
                }
                else
                {
                    ++candidate.sizeMismatches;
                    ++candidate.shapeFailures;
                }

                const uint16_t expectedReturnOffset = summary.returnOffset >= 0
                                                          ? static_cast<uint16_t>(summary.returnOffset)
                                                          : std::numeric_limits<uint16_t>::max();
                observation.returnMatches = summary.valid && observation.returnOffset == expectedReturnOffset;
                if (observation.returnMatches)
                    ++candidate.returnHits;
                else
                {
                    ++candidate.returnMismatches;
                    ++candidate.returnFailures;
                }
            }

            const auto sameHeaderEvidence = [](const FunctionCandidate &left, const FunctionCandidate &right)
            {
                return left.headerReadable == right.headerReadable && left.shapeHits == right.shapeHits &&
                       left.flagHits == right.flagHits &&
                       left.returnHits == right.returnHits && left.invalidShapes == right.invalidShapes &&
                       left.bothMismatches == right.bothMismatches &&
                       left.countMismatches == right.countMismatches &&
                       left.sizeMismatches == right.sizeMismatches &&
                       left.returnMismatches == right.returnMismatches;
            };
            if (candidate.flagHits >= minimumHeaderSamples || candidate.shapeHits >= minimumHeaderSamples)
                report.evidence.push_back("UFunction header candidate offset=" + std::to_string(offset) +
                                          " readable=" + std::to_string(candidate.headerReadable) +
                                          " flags=" + std::to_string(candidate.flagHits) +
                                          " shapes=" + std::to_string(candidate.shapeHits) +
                                          " returns=" + std::to_string(candidate.returnHits) +
                                          " parameterized=" + std::to_string(candidate.parameterizedSamples) +
                                          " returning=" + std::to_string(candidate.returningSamples) +
                                          " hard_valid=" + std::to_string(isHardValidHeaderCandidate(candidate)));
            if (bestObserved.offset < 0 || betterHeaderCandidate(candidate, bestObserved))
                bestObserved = candidate;

            if (!isHardValidHeaderCandidate(candidate))
                continue;

            if (best.offset < 0 || betterHeaderCandidate(candidate, best))
            {
                best = candidate;
                equivalentHeaderCandidates = 1;
            }
            else if (sameHeaderEvidence(candidate, best))
                ++equivalentHeaderCandidates;
        }

        const FunctionCandidate &diagnosticCandidate = best.offset >= 0 ? best : bestObserved;
        const bool parameterSemanticsComplete = isHardValidHeaderCandidate(best);
        const std::string candidateSummary = " selected_flags=" + std::to_string(diagnosticCandidate.flagHits) +
                                             " header_readable=" + std::to_string(diagnosticCandidate.headerReadable) +
                                             " flag_failures=" + std::to_string(diagnosticCandidate.flagFailures) +
                                             " selected_shapes=" + std::to_string(diagnosticCandidate.shapeHits) +
                                             " selected_returns=" + std::to_string(diagnosticCandidate.returnHits) +
                                             " parameterized_samples=" + std::to_string(diagnosticCandidate.parameterizedSamples) +
                                             " returning_samples=" + std::to_string(diagnosticCandidate.returningSamples) +
                                             " selected_offset=" + std::to_string(diagnosticCandidate.offset) +
                                             " hard_valid=" + std::to_string(isHardValidHeaderCandidate(best)) +
                                             " parameter_chain_offset=" + std::to_string(propertyChainOffset) +
                                             " parameter_semantics=" +
                                             std::string(parameterSemanticsComplete ? "complete" : "partial") +
                                             " property_flags_offset=" + std::to_string(schema.property.propertyFlags) +
                                             sampleSummary;
        if (!parameterSemanticsComplete)
        {
            report.evidence.push_back("UFunction structural layout found but parameter semantics were incomplete;" +
                                      candidateSummary + " count_mismatch=" +
                                      std::to_string(diagnosticCandidate.countMismatches) + " size_mismatch=" +
                                      std::to_string(diagnosticCandidate.sizeMismatches) + " both_mismatch=" +
                                      std::to_string(diagnosticCandidate.bothMismatches) + " invalid_shape=" +
                                      std::to_string(diagnosticCandidate.invalidShapes) + " return_mismatch=" +
                                      std::to_string(diagnosticCandidate.returnMismatches));
            size_t reportedParameters = 0;
            for (size_t sampleIndex = 0; sampleIndex < functions.size() && reportedParameters < 8; ++sampleIndex)
            {
                for (const PropertyMetadata &parameter : functions[sampleIndex].properties)
                {
                    if (reportedParameters >= 8)
                        break;
                    report.evidence.push_back("UFunction parameter sample=" + std::to_string(sampleIndex) +
                                              " name=" + parameter.name + " class=" + parameter.className +
                                              " flags=" + std::to_string(parameter.flags) +
                                              " cpf_parm=" + std::to_string((parameter.flags & kCPFParm) != 0) +
                                              " offset=" + std::to_string(parameter.offset) +
                                              " element_size=" + std::to_string(parameter.elementSize) +
                                              " array_dim=" + std::to_string(parameter.arrayDim));
                    ++reportedParameters;
                }
            }
        }
        if (best.offset < 0 || !parameterSemanticsComplete || equivalentHeaderCandidates > 1)
        {
            std::string mismatchSamples;
            std::string returnSamples;
            size_t reportedSamples = 0;
            size_t reportedReturnSamples = 0;
            for (size_t index = 0; index < functions.size() && reportedSamples < 3; ++index)
            {
                const FunctionSample &sample = functions[index];
                if (!sample.parameterChainValid || diagnosticCandidate.offset < 0)
                    continue;
                const FunctionObservation &observation = diagnosticCandidate.observations[index];
                if (!observation.readable || !observation.flagsPlausible || observation.shapeMatches)
                    continue;
                mismatchSamples += " sample_index=" + std::to_string(index) +
                                   " function=" + sample.name + " class=" + sample.className +
                                   " expected_count=" + std::to_string(observation.summary.count) +
                                   " actual_count=" + std::to_string(observation.numParams) +
                                   " expected_size=" + std::to_string(observation.summary.paramEnd) +
                                   " actual_size=" + std::to_string(observation.paramSize);
                ++reportedSamples;
            }
            for (size_t index = 0; index < functions.size() && reportedReturnSamples < 8; ++index)
            {
                const FunctionSample &sample = functions[index];
                if (!sample.parameterChainValid || diagnosticCandidate.offset < 0)
                    continue;
                const FunctionObservation &observation = diagnosticCandidate.observations[index];
                if (observation.readable && observation.flagsPlausible && observation.returnMatches)
                    continue;
                const FunctionParameterSummary &summary = observation.summary;
                const uint16_t expectedReturnOffset = summary.returnOffset >= 0
                                                          ? static_cast<uint16_t>(summary.returnOffset)
                                                          : std::numeric_limits<uint16_t>::max();
                returnSamples += " sample_index=" + std::to_string(index) +
                                 " address=" + std::to_string(sample.address) +
                                 " function=" + sample.name + " class=" + sample.className +
                                 " object_flags=" + std::to_string(sample.objectFlags) +
                                 " readable=" + std::to_string(observation.readable) +
                                 " flags=" + std::to_string(observation.flags) +
                                 " flags_plausible=" + std::to_string(observation.flagsPlausible) +
                                 " scored=" + std::to_string(observation.readable && observation.flagsPlausible) +
                                 " summary_valid=" + std::to_string(summary.valid) +
                                 " properties=" + std::to_string(sample.properties.size()) +
                                 " derived_count=" + std::to_string(summary.count) +
                                 " actual=" + std::to_string(observation.returnOffset) +
                                 " expected=" + std::to_string(expectedReturnOffset);
                ++reportedReturnSamples;
            }
            report.failures.push_back("UFunction layout candidate was rejected; parameter_chain_samples=" +
                                      std::to_string(parameterChainSamples) + " minimum_header_samples=" +
                                      std::to_string(minimumHeaderSamples) + candidateSummary +
                                      " count_mismatch=" + std::to_string(diagnosticCandidate.countMismatches) +
                                      " size_mismatch=" + std::to_string(diagnosticCandidate.sizeMismatches) +
                                      " both_mismatch=" + std::to_string(diagnosticCandidate.bothMismatches) +
                                      " invalid_shape=" + std::to_string(diagnosticCandidate.invalidShapes) +
                                      " return_mismatch=" + std::to_string(diagnosticCandidate.returnMismatches) +
                                      " equivalent_candidates=" + std::to_string(equivalentHeaderCandidates) +
                                      " return_samples=" + returnSamples +
                                      mismatchSamples);
            return false;
        }
        schema.ufunction.functionFlags = best.offset;
        schema.ufunction.numParams = best.offset + 4;
        schema.ufunction.paramSize = best.offset + 6;
        schema.ufunction.returnValueOffset = best.offset + 8;
        report.evidence.push_back("resolved UFunction::FunctionFlags, NumParms and ParmsSize from multiple samples;" + candidateSummary);

        struct NativeCandidate
        {
            int32_t offset = -1;
            size_t nativeSamples = 0;
            size_t moduleExecutableHits = 0;
            size_t outsideModuleExecutableHits = 0;
            size_t missingHits = 0;
            size_t nonNativeSamples = 0;
            size_t nonNativeModuleExecutableHits = 0;
            size_t nonNativeOutsideModuleExecutableHits = 0;
            size_t nonNativeMissingHits = 0;
        };
        NativeCandidate bestNative;
        NativeCandidate bestObservedNative;
        size_t equivalentNativeCandidates = 0;
        const size_t requiredNativeSamples = std::max<size_t>(4, parameterChainSamples / 4);
        const auto isModuleAddress = [&](uintptr_t address)
        {
            return moduleBase_ != 0 && moduleEnd_ > moduleBase_ &&
                   address >= moduleBase_ && address < moduleEnd_;
        };
        const auto recordFunctionPointer = [&](uintptr_t value, size_t &moduleHits,
                                               size_t &outsideModuleHits, size_t &missingHits)
        {
            if (value != 0 && memory_.IsExecutable(value, sizeof(uintptr_t)))
            {
                if (isModuleAddress(value))
                    ++moduleHits;
                else
                    ++outsideModuleHits;
            }
            else
                ++missingHits;
        };
        for (const int32_t delta : nativeFunctionDeltas)
        {
            NativeCandidate candidate;
            candidate.offset = best.offset + delta;
            for (size_t index = 0; index < functions.size(); ++index)
            {
                const FunctionSample &sample = functions[index];
                if (!sample.parameterChainValid)
                    continue;
                const FunctionObservation &observation = best.observations[index];
                const auto nativeAddress = Add(sample.address, candidate.offset);
                if (!observation.readable || !observation.flagsPlausible || !nativeAddress)
                    continue;
                const uint32_t flags = observation.flags;
                uintptr_t native = 0;
                if (!memory_.Read(*nativeAddress, native))
                {
                    if ((flags & kFUNCNative) != 0)
                    {
                        ++candidate.nativeSamples;
                        ++candidate.missingHits;
                    }
                    else
                    {
                        ++candidate.nonNativeSamples;
                        ++candidate.nonNativeMissingHits;
                    }
                    continue;
                }

                if ((flags & kFUNCNative) != 0)
                {
                    ++candidate.nativeSamples;
                    recordFunctionPointer(native, candidate.moduleExecutableHits,
                                          candidate.outsideModuleExecutableHits, candidate.missingHits);
                }
                else
                {
                    ++candidate.nonNativeSamples;
                    recordFunctionPointer(native, candidate.nonNativeModuleExecutableHits,
                                          candidate.nonNativeOutsideModuleExecutableHits,
                                          candidate.nonNativeMissingHits);
                }
            }

            if (bestObservedNative.offset < 0 ||
                candidate.moduleExecutableHits > bestObservedNative.moduleExecutableHits ||
                (candidate.moduleExecutableHits == bestObservedNative.moduleExecutableHits &&
                 candidate.nativeSamples > bestObservedNative.nativeSamples))
                bestObservedNative = candidate;

            const size_t requiredNativeHits = (candidate.nativeSamples * 3 + 3) / 4;
            const bool hardValid = moduleBase_ != 0 && moduleEnd_ > moduleBase_ &&
                                   candidate.nativeSamples >= requiredNativeSamples &&
                                   candidate.moduleExecutableHits >= requiredNativeHits;
            if (!hardValid)
                continue;

            const auto betterNativeCandidate = [](const NativeCandidate &left, const NativeCandidate &right)
            {
                if (left.moduleExecutableHits != right.moduleExecutableHits)
                    return left.moduleExecutableHits > right.moduleExecutableHits;
                if (left.nativeSamples != right.nativeSamples)
                    return left.nativeSamples > right.nativeSamples;
                if (left.outsideModuleExecutableHits != right.outsideModuleExecutableHits)
                    return left.outsideModuleExecutableHits < right.outsideModuleExecutableHits;
                if (left.missingHits != right.missingHits)
                    return left.missingHits < right.missingHits;
                if (left.nonNativeModuleExecutableHits != right.nonNativeModuleExecutableHits)
                    return left.nonNativeModuleExecutableHits > right.nonNativeModuleExecutableHits;
                if (left.nonNativeOutsideModuleExecutableHits != right.nonNativeOutsideModuleExecutableHits)
                    return left.nonNativeOutsideModuleExecutableHits < right.nonNativeOutsideModuleExecutableHits;
                if (left.nonNativeMissingHits != right.nonNativeMissingHits)
                    return left.nonNativeMissingHits < right.nonNativeMissingHits;
                return false;
            };
            const auto sameNativeEvidence = [](const NativeCandidate &left, const NativeCandidate &right)
            {
                return left.nativeSamples == right.nativeSamples &&
                       left.moduleExecutableHits == right.moduleExecutableHits &&
                       left.outsideModuleExecutableHits == right.outsideModuleExecutableHits &&
                       left.missingHits == right.missingHits &&
                       left.nonNativeSamples == right.nonNativeSamples &&
                       left.nonNativeModuleExecutableHits == right.nonNativeModuleExecutableHits &&
                       left.nonNativeOutsideModuleExecutableHits == right.nonNativeOutsideModuleExecutableHits &&
                       left.nonNativeMissingHits == right.nonNativeMissingHits;
            };
            if (bestNative.offset < 0 || betterNativeCandidate(candidate, bestNative))
            {
                bestNative = candidate;
                equivalentNativeCandidates = 1;
            }
            else if (sameNativeEvidence(candidate, bestNative))
            {
                ++equivalentNativeCandidates;
            }
        }
        const NativeCandidate &diagnosticNative = bestNative.offset >= 0 ? bestNative : bestObservedNative;
        const size_t requiredNativeHits = (diagnosticNative.nativeSamples * 3 + 3) / 4;
        if (bestNative.offset < 0 || bestNative.nativeSamples < requiredNativeSamples ||
            bestNative.moduleExecutableHits < requiredNativeHits || equivalentNativeCandidates > 1)
        {
            report.failures.push_back("UFunction::ExecFunction was not resolved after header selection; flags_offset=" +
                                      std::to_string(schema.ufunction.functionFlags) +
                                      " native_offset=" + std::to_string(diagnosticNative.offset) +
                                      " native_samples=" + std::to_string(diagnosticNative.nativeSamples) +
                                      " required_native_samples=" + std::to_string(requiredNativeSamples) +
                                      " module_executable_hits=" + std::to_string(diagnosticNative.moduleExecutableHits) +
                                      " required_executable_hits=" + std::to_string(requiredNativeHits) +
                                      " outside_module_executable_hits=" +
                                      std::to_string(diagnosticNative.outsideModuleExecutableHits) +
                                      " missing=" + std::to_string(diagnosticNative.missingHits) +
                                      " non_native_samples=" + std::to_string(diagnosticNative.nonNativeSamples) +
                                      " non_native_module_executable_hits=" +
                                      std::to_string(diagnosticNative.nonNativeModuleExecutableHits) +
                                      " non_native_outside_module_executable_hits=" +
                                      std::to_string(diagnosticNative.nonNativeOutsideModuleExecutableHits) +
                                      " non_native_missing=" + std::to_string(diagnosticNative.nonNativeMissingHits) +
                                      " equivalent_candidates=" + std::to_string(equivalentNativeCandidates) +
                                      " module_base=" + std::to_string(moduleBase_) +
                                      " module_end=" + std::to_string(moduleEnd_) + candidateSummary);
            return false;
        }
        schema.ufunction.nativeFunction = bestNative.offset;
        report.evidence.push_back("resolved UFunction::ExecFunction from module executable pointers; offset=" +
                                  std::to_string(schema.ufunction.nativeFunction) + " hits=" +
                                  std::to_string(bestNative.moduleExecutableHits) + " native_samples=" +
                                  std::to_string(bestNative.nativeSamples) + " outside_module=" +
                                  std::to_string(bestNative.outsideModuleExecutableHits) + " non_native_samples=" +
                                  std::to_string(bestNative.nonNativeSamples));

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
                if (childClass && IsFunctionFieldKind(FieldKindFromRuntimeName(*childClass, false)))
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
        if (!bootstrap_ || !bootstrap_->IsValid())
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
        ObjectModelReader model(memory_, binding_, schema);
        if (!model.Initialize())
        {
            report.failures.push_back("object model could not be initialized for UEnum samples");
            return false;
        }
        for (int32_t index = 0; index < bootstrap_->objects->Count(); ++index)
        {
            const auto object = bootstrap_->objects->ReadObject(index);
            if (!object.IsValid())
                continue;
            const auto classAddress = Add(object.address, schema.uobject.classPointer);
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
                const auto flags = model.Flags(object.address);
                if (!flags || (*flags & (kRFClassDefaultObject | kRFIncompleteLoad)) != 0)
                    continue;
                enumObjects.push_back(object.address);
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
        // 在 ARM32 架构下，TArray 采用指针对齐
        // 位于 Names 前方的旧版 FString 可能会使其内存偏移落于 +0x2c 处
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
            // 在 ARM32 架构下，UE4 UEnum 遗留/FProperty 族包含一个指针大小的 Next
            // 紧随其后的是一个 FString（包含一个指针和两个 int32）
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

        // Names 是一个 TArray<TPair<FName, int64>> 类型的容器
        // 尾部字段单独生成候选并用运行时语义验证宽度、顺序、显示函数和 EnumPackage
        const int32_t enumNamesArraySize =
            ((pointerSize + static_cast<int32_t>(sizeof(int32_t) * 2) + pointerSize - 1) /
             pointerSize) *
            pointerSize;
        const auto alignOffset = [](int32_t offset, int32_t alignment)
        {
            return (offset + alignment - 1) / alignment * alignment;
        };
        struct EnumTailCandidate
        {
            EnumTailLayout layout = EnumTailLayout::Legacy;
            bool cppFormIsByte = false;
            bool flagsIsByte = false;
            int32_t cppForm = -1;
            int32_t flags = -1;
            int32_t displayName = -1;
            int32_t package = -1;
            size_t formHits = 0;
            size_t flagHits = 0;
            size_t displayHits = 0;
            size_t packageHits = 0;
        };
        const std::array<EnumTailLayout, 4> tailLayouts = {
            EnumTailLayout::Legacy, EnumTailLayout::Flags,
            EnumTailLayout::FlagsDisplayNamePackage, EnumTailLayout::FlagsPackageDisplayName};
        std::vector<EnumTailCandidate> tailCandidates;
        for (const EnumTailLayout layout : tailLayouts)
        {
            if (layout != schema.features.enumTailLayout)
                continue;
            for (const bool cppFormIsByte : {false, true})
            {
                if (cppFormIsByte != schema.features.enumCppFormIsByte)
                    continue;
                const bool hasFlags = layout != EnumTailLayout::Legacy;
                for (const bool flagsIsByte : {false, true})
                {
                    if (!hasFlags && flagsIsByte)
                        continue;
                    if (hasFlags && flagsIsByte != schema.features.enumFlagsIsByte)
                        continue;
                    EnumTailCandidate candidate;
                    candidate.layout = layout;
                    candidate.cppFormIsByte = cppFormIsByte;
                    candidate.flagsIsByte = flagsIsByte;
                    candidate.cppForm = schema.uenum.names + enumNamesArraySize;
                    const int32_t cppFormWidth = cppFormIsByte ? 1 : static_cast<int32_t>(sizeof(uint32_t));
                    const int32_t flagsWidth = flagsIsByte ? 1 : static_cast<int32_t>(sizeof(uint32_t));
                    const int32_t flagsOffset = candidate.cppForm + cppFormWidth;
                    candidate.flags = hasFlags ? flagsOffset : -1;
                    if (layout == EnumTailLayout::FlagsDisplayNamePackage)
                    {
                        candidate.displayName = alignOffset(flagsOffset + flagsWidth, pointerSize);
                        candidate.package = candidate.displayName + pointerSize;
                    }
                    else if (layout == EnumTailLayout::FlagsPackageDisplayName)
                    {
                        candidate.package = alignOffset(flagsOffset + flagsWidth, static_cast<int32_t>(alignof(uint32_t)));
                        candidate.displayName = alignOffset(candidate.package + schema.fname.size, pointerSize);
                    }
                    else
                        candidate.displayName = alignOffset((hasFlags ? flagsOffset + flagsWidth : candidate.cppForm + cppFormWidth),
                                                            pointerSize);

                    for (uintptr_t enumObject : enumObjects)
                    {
                        const auto formAddress = Add(enumObject, candidate.cppForm);
                        uint32_t form = 0;
                        bool formValid = false;
                        if (formAddress)
                        {
                            if (cppFormIsByte)
                            {
                                uint8_t value = 0;
                                formValid = memory_.Read(*formAddress, value);
                                form = value;
                            }
                            else
                                formValid = memory_.Read(*formAddress, form);
                        }
                        if (!formValid || form > 2)
                            continue;
                        ++candidate.formHits;

                        const auto displayAddress = Add(enumObject, candidate.displayName);
                        uintptr_t displayFunction = 0;
                        if (displayAddress && memory_.Read(*displayAddress, displayFunction) &&
                            (displayFunction == 0 || memory_.IsExecutable(displayFunction, sizeof(uintptr_t))))
                            ++candidate.displayHits;

                        if (hasFlags)
                        {
                            const auto flagsAddress = Add(enumObject, candidate.flags);
                            bool validFlags = false;
                            if (flagsAddress)
                            {
                                const uint32_t allowedFlags = profile_.family == EngineFamily::UE4FProperty ? 0x01u : 0x03u;
                                if (flagsIsByte)
                                {
                                    uint8_t value = 0;
                                    validFlags = memory_.Read(*flagsAddress, value) && (value & ~allowedFlags) == 0;
                                }
                                else
                                {
                                    uint32_t value = 0;
                                    validFlags = memory_.Read(*flagsAddress, value) && (value & ~allowedFlags) == 0;
                                }
                            }
                            if (validFlags)
                                ++candidate.flagHits;
                        }
                        if (candidate.package >= 0)
                        {
                            const auto packageAddress = Add(enumObject, candidate.package);
                            const auto outerAddress = Add(enumObject, schema.uobject.outer);
                            uintptr_t outer = 0;
                            if (packageAddress && outerAddress && memory_.Read(*outerAddress, outer))
                            {
                                outer = binding_.decode.objectOuter(outer, *outerAddress);
                                const auto packageName = names.ReadFName(*packageAddress);
                                const auto outerName = outer == 0 ? std::optional<std::string>{} : readObjectName(outer);
                                if (packageName && outerName && *packageName == *outerName)
                                    ++candidate.packageHits;
                            }
                        }
                    }
                    const bool formValid = candidate.formHits >= requiredNonEmptyArrays;
                    const bool flagsValid = !schema.features.enumFlagsRequired ||
                                            (hasFlags && candidate.flagHits >= requiredNonEmptyArrays &&
                                             candidate.displayHits >= requiredNonEmptyArrays);
                    const bool packageValid = !schema.features.enumHasPackage ||
                                              (candidate.package >= 0 && candidate.packageHits >= requiredNonEmptyArrays);
                    if (formValid && flagsValid && packageValid)
                        tailCandidates.push_back(candidate);
                }
            }
        }
        const auto tailScore = [](const EnumTailCandidate &candidate)
        {
            return candidate.formHits * 100 + candidate.flagHits * 50 +
                   candidate.packageHits * 50 + candidate.displayHits * 10;
        };
        std::sort(tailCandidates.begin(), tailCandidates.end(), [&](const EnumTailCandidate &left, const EnumTailCandidate &right)
                  { return tailScore(left) > tailScore(right); });
        if (tailCandidates.empty() ||
            (tailCandidates.size() > 1 && tailScore(tailCandidates[0]) == tailScore(tailCandidates[1])))
        {
            report.failures.push_back("UEnum tail layout candidates were rejected or ambiguous; candidates=" +
                                      std::to_string(tailCandidates.size()));
            return false;
        }
        const EnumTailCandidate &selectedTail = tailCandidates.front();
        schema.features.enumTailLayout = selectedTail.layout;
        schema.features.enumCppFormIsByte = selectedTail.cppFormIsByte;
        schema.features.enumFlagsIsByte = selectedTail.flagsIsByte;
        schema.features.enumHasFlags = selectedTail.layout != EnumTailLayout::Legacy;
        schema.features.enumHasPackage = selectedTail.package >= 0;
        schema.uenum.cppForm = selectedTail.cppForm;
        schema.uenum.flags = selectedTail.flags;
        schema.uenum.enumPackage = selectedTail.package;
        report.evidence.push_back("resolved UEnum tail; cpp_form=" + std::to_string(selectedTail.cppForm) +
                                  " flags=" + std::to_string(selectedTail.flags) +
                                  " package=" + std::to_string(selectedTail.package) +
                                  " form_hits=" + std::to_string(selectedTail.formHits) +
                                  " flag_hits=" + std::to_string(selectedTail.flagHits) +
                                  " display_hits=" + std::to_string(selectedTail.displayHits) +
                                  " package_hits=" + std::to_string(selectedTail.packageHits));
        schema.validation.enums = true;
        report.evidence.push_back(schema.features.enumUsesFNameData ? "FNameData-backed enum family selected" : "legacy enum container family selected");
        return true;
    }

    SchemaResolutionReport SchemaResolver::Resolve(EngineSchema &schema) const
    {
        SchemaResolutionReport report;
        const uint64_t addressSpaceGeneration = memory_.AddressSpaceGeneration();
        report.profileId = profile_.id;
        report.profileLabel = profile_.label;
        schema.family = profile_.family;
        schema.layout = profile_.layout;
        schema.features = profile_.features;
        report.evidence.push_back("engine structure profile=" + profile_.id +
                                  " version_range=" + profile_.versionRange +
                                  " use_fproperty=" + std::to_string(schema.features.useFProperty) +
                                  " use_name_pool=" + std::to_string(schema.features.useNamePool) +
                                  " ffield_owner_encoding=" +
                                  std::to_string(schema.features.fFieldOwnerEncoding == FFieldOwnerEncoding::TaggedPointer) +
                                  " large_world_coordinates=" + std::to_string(schema.features.largeWorldCoordinates) +
                                  " layout=" + SchemaLayoutVariantName(schema.layout) +
                                  " enum_tail=" + EnumTailLayoutName(schema.features.enumTailLayout) +
                                  " enum_cpp_form_byte=" + std::to_string(schema.features.enumCppFormIsByte) +
                                  " enum_flags_required=" + std::to_string(schema.features.enumFlagsRequired));
        if (!profile_.IsValid())
        {
            report.failureStage = "profile";
            report.failures.push_back("engine structure profile is invalid");
            return report;
        }

        report.failureStage = "uobject";
        if (!ResolveUObjectSchema(schema, report) || !ValidateUObjectSchema(schema, report))
            return report;
        report.failureStage = "struct";
        if (!ResolveStructSchema(schema, report))
            return report;
        report.failureStage = "field";
        if (!ResolveFieldSchema(schema, report))
            return report;
        report.failureStage = "property";
        if (!ResolvePropertySchema(schema, report) || !ResolvePropertySubtypes(schema, report))
            return report;
        report.failureStage = "function";
        if (!ResolveFunctionSchema(schema, report))
            return report;
        report.failureStage = "enum";
        if (!ResolveEnumSchema(schema, report))
            return report;

        schema.validation.uobject = true;
        schema.validation.fname = binding_.names.IsValid();
        if (schema.features.useFProperty)
            schema.validation.fields = true;
        schema.validation.profileId = profile_.id;
        schema.validation.profileLabel = profile_.label;
        schema.validation.profileVersionRange = profile_.versionRange;
        schema.validation.familyEvidence = profile_.versionRange;
        if (memory_.AddressSpaceGeneration() != addressSpaceGeneration)
        {
            report.addressSpaceChanged = true;
            report.failureStage = "address-space-generation";
            report.failures.push_back("remote address-space generation changed during schema resolution");
            return report;
        }
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
            if (schema.features.enumFlagsRequired && schema.uenum.flags >= 0)
                report.score += 5;
            if (schema.features.enumHasPackage && schema.uenum.enumPackage >= 0)
                report.score += 5;
        }
        return report;
    }
} // namespace anduefker::ue
