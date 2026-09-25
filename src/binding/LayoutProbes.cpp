#include "anduefker/binding/LayoutProbes.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>

namespace anduefker::binding
{
    namespace
    {
        std::optional<uintptr_t> Add(uintptr_t base, uintptr_t offset)
        {
            if (base > UINTPTR_MAX - offset)
                return std::nullopt;
            return base + offset;
        }

        std::optional<uintptr_t> Scaled(uintptr_t base, int32_t index, int32_t stride)
        {
            if (index < 0 || stride <= 0)
                return std::nullopt;
            const uintptr_t uIndex = static_cast<uintptr_t>(index);
            const uintptr_t uStride = static_cast<uintptr_t>(stride);
            if (uIndex != 0 && uStride > UINTPTR_MAX / uIndex)
                return std::nullopt;
            return Add(base, uIndex * uStride);
        }

        bool ReadPointer(const IMemorySource &memory, uintptr_t address, uintptr_t &value)
        {
            return memory.Read(address, value);
        }
    } // namespace

    bool ObjectLayoutProbe::IsLikelyObject(uintptr_t address) const
    {
        if (address == 0 || !memory_.IsReadable(address, sizeof(uintptr_t)))
            return false;

        uintptr_t vtable = 0;
        if (!memory_.Read(address, vtable) || vtable == 0)
            return false;
        if (!memory_.IsReadable(vtable, sizeof(uintptr_t)))
            return false;

        uintptr_t firstFunction = 0;
        if (!memory_.Read(vtable, firstFunction) || firstFunction == 0)
            return false;
        return memory_.IsExecutable(firstFunction, sizeof(uintptr_t));
    }

    bool ObjectLayoutProbe::ReadItemAddress(uintptr_t root,
                                            const ObjectContainerLayout &layout,
                                            const DecodePlan &decode,
                                            int32_t index,
                                            uintptr_t &item) const
    {
        const auto storageAddress = Add(root, static_cast<uintptr_t>(layout.objectsOffset));
        if (!storageAddress)
            return false;

        uintptr_t rawStorage = 0;
        if (!ReadPointer(memory_, *storageAddress, rawStorage))
            return false;
        const uintptr_t storage = decode.objectStorage(rawStorage, *storageAddress);

        if (layout.kind == ObjectContainerKind::Fixed)
        {
            const auto itemAddress = Scaled(storage, index, layout.itemStride);
            if (!itemAddress)
                return false;
            item = *itemAddress;
            return true;
        }

        if (layout.elementsPerChunk <= 0)
            return false;
        const int32_t chunkIndex = index / layout.elementsPerChunk;
        const int32_t withinChunk = index % layout.elementsPerChunk;
        const auto chunkSlot = Scaled(storage, chunkIndex, static_cast<int32_t>(sizeof(uintptr_t)));
        if (!chunkSlot)
            return false;

        uintptr_t rawChunk = 0;
        if (!ReadPointer(memory_, *chunkSlot, rawChunk))
            return false;
        const uintptr_t chunk = decode.objectChunk(rawChunk, *chunkSlot);
        if (chunk == 0 || !memory_.IsReadable(chunk, sizeof(uintptr_t)))
            return false;
        const auto itemAddress = Scaled(chunk, withinChunk, layout.itemStride);
        if (!itemAddress)
            return false;
        item = *itemAddress;
        return true;
    }

    LayoutProbeReport ObjectLayoutProbe::Validate(uintptr_t root,
                                                  const ObjectContainerLayout &layout,
                                                  const DecodePlan &decode,
                                                  int32_t maxSamples) const
    {
        LayoutProbeReport report;
        if (root == 0 || !layout.IsValid() || maxSamples <= 0)
        {
            report.failures.push_back("invalid object layout probe input");
            return report;
        }

        const auto countAddress = Add(root, static_cast<uintptr_t>(layout.numElementsOffset));
        if (!countAddress)
        {
            report.failures.push_back("object count address overflow");
            return report;
        }

        int32_t rawCount = 0;
        if (!memory_.Read(*countAddress, rawCount))
        {
            report.failures.push_back("object count is unreadable");
            return report;
        }
        const int32_t count = decode.objectCount(rawCount, *countAddress);
        if (count <= 0 || count > 0x08000000)
        {
            report.failures.push_back("object count is outside the supported range");
            return report;
        }

        constexpr std::array<int32_t, 16> samples = {0, 1, 2, 4, 8, 16, 32, 64,
                                                     128, 256, 512, 1024, 2048, 4096, 8192, 16384};
        const int32_t stride = std::max(1, count / std::max(1, maxSamples));

        auto validateIndex = [&](int32_t index)
        {
            if (index < 0 || index >= count)
                return;
            ++report.tested;

            uintptr_t item = 0;
            if (!ReadItemAddress(root, layout, decode, index, item))
                return;

            uintptr_t rawObject = 0;
            const auto objectSlot = Add(item, static_cast<uintptr_t>(layout.itemObjectOffset));
            if (!objectSlot || !ReadPointer(memory_, *objectSlot, rawObject))
                return;
            const uintptr_t object = decode.objectPointer(rawObject, *objectSlot);

            // 允许存在空槽，这不违背容器的布局规则
            if (object == 0)
                return;
            if (!IsLikelyObject(object))
                return;

            const auto indexAddress = Add(object, static_cast<uintptr_t>(layout.itemIndexOffset));
            if (!indexAddress)
                return;
            int32_t reportedIndex = 0;
            if (!memory_.Read(*indexAddress, reportedIndex))
                return;
            reportedIndex = decode.objectIndex(reportedIndex, *indexAddress);
            if (reportedIndex == index)
                ++report.valid;
        };

        for (int32_t index : samples)
            validateIndex(index);
        for (int32_t index = 0; index < count && report.tested < maxSamples; index += stride)
            validateIndex(index);

        report.confidence = report.tested > 0 ? static_cast<double>(report.valid) / report.tested : 0.0;
        report.accepted = report.valid >= 5 && report.confidence >= 0.80;
        report.evidence.push_back("empty object slots were treated as holes, not layout failures");
        report.evidence.push_back("validated non-empty slots through UObject internal index");
        if (!report.accepted)
            report.failures.push_back("insufficient self-consistent UObject samples");
        return report;
    }

    bool NameLayoutProbe::ReadString(uintptr_t address, int32_t length, bool wide, std::string &out) const
    {
        if (length <= 0 || length > 1024)
            return false;
        if (!wide)
        {
            std::vector<char> bytes(static_cast<size_t>(length));
            if (!memory_.ReadBytes(address, bytes.data(), bytes.size()).Ok())
                return false;
            out.assign(bytes.data(), bytes.data() + bytes.size());
            return true;
        }

        std::vector<char16_t> chars(static_cast<size_t>(length));
        if (!memory_.ReadBytes(address, chars.data(), chars.size() * sizeof(char16_t)).Ok())
            return false;
        out.clear();
        for (char16_t character : chars)
        {
            if (character == 0)
                break;
            if (character < 0x80)
                out.push_back(static_cast<char>(character));
            else
                out.push_back('?');
        }
        return true;
    }

    bool NameLayoutProbe::ReadEntry(uintptr_t entry,
                                    const NameContainerLayout &layout,
                                    const DecodePlan &decode,
                                    std::string &out) const
    {
        if (entry == 0)
            return false;
        uint16_t rawHeader = 0;

        if (layout.kind == NameContainerKind::Array)
        {
            uint32_t rawIndex = 0;
            const uintptr_t indexAddress = entry + static_cast<uintptr_t>(layout.array.entryIndexOffset);
            const uintptr_t stringAddress = entry + static_cast<uintptr_t>(layout.array.entryStringOffset);
            if (!memory_.Read(indexAddress, rawIndex))
                return false;
            const uint32_t index = decode.nameEntryIndex(rawIndex, indexAddress);
            return ReadString(stringAddress, 4, (index & 1u) != 0, out);
        }

        const uintptr_t headerAddress = entry + static_cast<uintptr_t>(layout.pool.entryHeaderOffset);
        const uintptr_t stringAddress = entry + static_cast<uintptr_t>(layout.pool.entryStringOffset);
        if (!memory_.Read(headerAddress, rawHeader))
            return false;
        const uint16_t header = decode.nameHeader(rawHeader, headerAddress);
        const int32_t length = static_cast<int32_t>(header >> layout.pool.entryLengthShift);
        const bool wide = (header & layout.pool.entryWideMask) != 0;
        return ReadString(stringAddress, length, wide, out);
    }

    LayoutProbeReport NameLayoutProbe::Validate(uintptr_t root,
                                                const NameContainerLayout &layout,
                                                const DecodePlan &decode) const
    {
        LayoutProbeReport report;
        if (root == 0 || !layout.IsValid())
        {
            report.failures.push_back("invalid name layout probe input");
            return report;
        }

        uintptr_t entry0 = 0;
        if (layout.kind == NameContainerKind::Array)
        {
            const uintptr_t chunks = root + static_cast<uintptr_t>(layout.array.chunksOffset);
            uintptr_t rawChunk = 0;
            if (!memory_.Read(chunks, rawChunk))
            {
                report.failures.push_back("name array chunk table is unreadable");
                return report;
            }
            const uintptr_t chunk = decode.nameChunks(rawChunk, chunks);
            if (!memory_.Read(chunk, entry0))
            {
                report.failures.push_back("name array entry zero is unreadable");
                return report;
            }
            entry0 = decode.nameEntry(entry0, chunk);
        }
        else
        {
            const uintptr_t blocks = root + static_cast<uintptr_t>(layout.pool.blocksOffset);
            uintptr_t rawBlock = 0;
            if (!memory_.Read(blocks, rawBlock))
            {
                report.failures.push_back("name pool block table is unreadable");
                return report;
            }
            const uintptr_t block = decode.nameBlocks(rawBlock, blocks);
            entry0 = decode.nameEntry(block, block);
        }

        std::string none;
        if (!ReadEntry(entry0, layout, decode, none) || none != "None")
        {
            report.failures.push_back("name entry zero did not decode to None");
            return report;
        }

        report.tested = 1;
        report.valid = 1;
        report.confidence = 1.0;
        report.accepted = true;
        report.evidence.push_back("name entry zero decoded to None");
        return report;
    }
} // namespace anduefker::binding
