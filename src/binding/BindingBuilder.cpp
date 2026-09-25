#include "anduefker/binding/BindingBuilder.hpp"

#include <algorithm>

namespace anduefker::binding
{
    namespace
    {
        const char *ObjectKindName(ObjectContainerKind kind)
        {
            return kind == ObjectContainerKind::Chunked ? "chunked" : "fixed";
        }

        const char *NameKindName(NameContainerKind kind)
        {
            return kind == NameContainerKind::Pool ? "pool" : "array";
        }

        std::string DescribeObjectLayout(const ObjectContainerLayout &layout)
        {
            return "kind=" + std::string(ObjectKindName(layout.kind)) +
                   " objects_offset=" + std::to_string(layout.objectsOffset) +
                   " count_offset=" + std::to_string(layout.numElementsOffset) +
                   " max_elements_offset=" + std::to_string(layout.maxElementsOffset) +
                   " max_chunks_offset=" + std::to_string(layout.maxChunksOffset) +
                   " elements_per_chunk=" + std::to_string(layout.elementsPerChunk) +
                   " item_object_offset=" + std::to_string(layout.itemObjectOffset) +
                   " item_stride=" + std::to_string(layout.itemStride) +
                   " item_index_offset=" + std::to_string(layout.itemIndexOffset);
        }

        std::string DescribeNameLayout(const NameContainerLayout &layout)
        {
            if (layout.kind == NameContainerKind::Array)
            {
                return "kind=" + std::string(NameKindName(layout.kind)) +
                       " chunks_offset=" + std::to_string(layout.array.chunksOffset) +
                       " elements_per_chunk=" + std::to_string(layout.array.elementsPerChunk) +
                       " entry_index_offset=" + std::to_string(layout.array.entryIndexOffset) +
                       " entry_string_offset=" + std::to_string(layout.array.entryStringOffset);
            }

            return "kind=" + std::string(NameKindName(layout.kind)) +
                   " blocks_offset=" + std::to_string(layout.pool.blocksOffset) +
                   " blocks_bit=" + std::to_string(layout.pool.blocksBit) +
                   " entry_stride=" + std::to_string(layout.pool.entryStride) +
                   " entry_header_offset=" + std::to_string(layout.pool.entryHeaderOffset) +
                   " entry_string_offset=" + std::to_string(layout.pool.entryStringOffset) +
                   " entry_length_shift=" + std::to_string(layout.pool.entryLengthShift) +
                   " entry_wide_mask=" + std::to_string(layout.pool.entryWideMask);
        }
    } // namespace

    std::vector<uintptr_t> BindingBuilder::ResolveRoots(const std::vector<LocatedAddress> &candidates) const
    {
        std::vector<uintptr_t> roots;
        for (const LocatedAddress &candidate : candidates)
        {
            if (candidate.address == 0)
                continue;

            auto add = [&roots](uintptr_t address)
            {
                if (address == 0 || std::find(roots.begin(), roots.end(), address) != roots.end())
                    return;
                roots.push_back(address);
            };

            if (candidate.meaning == AddressMeaning::Direct || candidate.meaning == AddressMeaning::ResolvedValue)
                add(candidate.address);

            if (candidate.meaning == AddressMeaning::PointerSlot || candidate.meaning == AddressMeaning::Direct)
            {
                uintptr_t value = 0;
                if (memory_.Read(candidate.address, value))
                    add(value);
            }
        }
        return roots;
    }

    std::optional<RuntimeBinding> BindingBuilder::Build(const BindingCandidates &candidates,
                                                        const DecodePlan &decode) const
    {
        const auto objectRoots = ResolveRoots(candidates.objectRoots);
        const auto nameRoots = ResolveRoots(candidates.nameRoots);
        if (objectRoots.empty() || nameRoots.empty())
            return std::nullopt;

        ObjectLayoutDiscovery objectDiscovery(memory_);
        NameLayoutDiscovery nameDiscovery(memory_);
        ObjectLayoutProbe objectProbe(memory_);
        NameLayoutProbe nameProbe(memory_);
        std::optional<RuntimeBinding> bestBinding;
        double bestObjectScore = -1.0;
        double bestNameScore = -1.0;

        for (uintptr_t objectRoot : objectRoots)
        {
            const auto objectLayouts = objectDiscovery.Discover(objectRoot, decode);
            for (const auto &[objectLayout, objectScore] : objectLayouts)
            {
                const LayoutProbeReport objectReport = objectProbe.Validate(objectRoot, objectLayout, decode);
                if (!objectReport.accepted)
                    continue;

                for (uintptr_t nameRoot : nameRoots)
                {
                    const auto nameLayouts = nameDiscovery.Discover(nameRoot, decode);
                    for (const auto &[nameLayout, nameScore] : nameLayouts)
                    {
                        const LayoutProbeReport nameReport = nameProbe.Validate(nameRoot, nameLayout, decode);
                        if (!nameReport.accepted)
                            continue;

                        RuntimeBinding binding;
                        binding.objectRoot = LocatedAddress{objectRoot, AddressMeaning::ResolvedValue, 90, "runtime-probe"};
                        binding.nameRoot = LocatedAddress{nameRoot, AddressMeaning::ResolvedValue, 90, "runtime-probe"};
                        binding.objects = objectLayout;
                        binding.names = nameLayout;
                        binding.decode = decode;
                        binding.report.staticCandidatesFound = true;
                        binding.report.objectContainerValidated = true;
                        binding.report.nameContainerValidated = true;
                        binding.report.semanticValidationPassed = true;
                        binding.report.evidence.push_back("object_root=" + std::to_string(objectRoot));
                        binding.report.evidence.push_back("name_root=" + std::to_string(nameRoot));
                        binding.report.evidence.push_back("object_layout_candidates=" + std::to_string(objectLayouts.size()));
                        binding.report.evidence.push_back("selected_object_layout: " + DescribeObjectLayout(objectLayout));
                        binding.report.evidence.push_back("object layout score=" + std::to_string(objectScore));
                        binding.report.evidence.push_back("object_probe tested=" + std::to_string(objectReport.tested) +
                                                          " valid=" + std::to_string(objectReport.valid) +
                                                          " confidence=" + std::to_string(objectReport.confidence));
                        binding.report.evidence.push_back("name_layout_candidates=" + std::to_string(nameLayouts.size()));
                        for (size_t candidateIndex = 0; candidateIndex < nameLayouts.size(); ++candidateIndex)
                        {
                            binding.report.evidence.push_back(
                                "name_layout_candidate[" + std::to_string(candidateIndex) + "] score=" +
                                std::to_string(nameLayouts[candidateIndex].second) + " " +
                                DescribeNameLayout(nameLayouts[candidateIndex].first));
                        }
                        binding.report.evidence.push_back("selected_name_layout: " + DescribeNameLayout(nameLayout));
                        binding.report.evidence.push_back("name layout score=" + std::to_string(nameScore));
                        binding.report.evidence.push_back("name_probe tested=" + std::to_string(nameReport.tested) +
                                                          " valid=" + std::to_string(nameReport.valid) +
                                                          " confidence=" + std::to_string(nameReport.confidence));
                        binding.report.evidence.insert(binding.report.evidence.end(), objectReport.evidence.begin(), objectReport.evidence.end());
                        binding.report.evidence.insert(binding.report.evidence.end(), nameReport.evidence.begin(), nameReport.evidence.end());
                        if (!bestBinding || objectScore > bestObjectScore ||
                            (objectScore == bestObjectScore && nameScore > bestNameScore))
                        {
                            bestObjectScore = objectScore;
                            bestNameScore = nameScore;
                            bestBinding = std::move(binding);
                        }
                    }
                }
            }
        }
        return bestBinding;
    }
} // namespace anduefker::binding
