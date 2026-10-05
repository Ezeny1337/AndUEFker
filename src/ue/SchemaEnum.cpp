#include "ProbeContext.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace anduefker::ue::schema_probe
{
    bool SchemaProbeContext::ResolveEnumSchema(EngineSchema &schema, SchemaResolutionReport &report) const
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

} // namespace anduefker::ue::schema_probe
