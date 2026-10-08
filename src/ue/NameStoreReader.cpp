#include "anduefker/ue/NameStoreReader.hpp"

#include <limits>
#include <vector>

namespace anduefker::ue
{
    namespace
    {
        std::optional<uintptr_t> AddOffset(uintptr_t address, uintptr_t offset)
        {
            if (address > std::numeric_limits<uintptr_t>::max() - offset)
                return std::nullopt;
            return address + offset;
        }

        std::optional<uintptr_t> AddScaled(uintptr_t address, int32_t index, int32_t stride)
        {
            if (index < 0 || stride <= 0)
                return std::nullopt;
            const uintptr_t uIndex = static_cast<uintptr_t>(index);
            const uintptr_t uStride = static_cast<uintptr_t>(stride);
            if (uIndex != 0 && uStride > std::numeric_limits<uintptr_t>::max() / uIndex)
                return std::nullopt;
            return AddOffset(address, uIndex * uStride);
        }

        void AppendUtf8(std::string &out, uint32_t codePoint)
        {
            if (codePoint <= 0x7F)
                out.push_back(static_cast<char>(codePoint));
            else if (codePoint <= 0x7FF)
            {
                out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else if (codePoint <= 0xFFFF)
            {
                out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else if (codePoint <= 0x10FFFF)
            {
                out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        }
    } // namespace

    NameStoreReader::NameStoreReader(const IMemorySource &memory,
                                     uintptr_t root,
                                     const NameContainerLayout &layout,
                                     const DecodePlan &decode,
                                     const FNameSchema &fname,
                                     const EngineFeatures &features)
        : memory_(memory), root_(root), layout_(layout), decode_(decode), fname_(fname), features_(features)
    {
    }

    std::optional<uintptr_t> NameStoreReader::ReadPointer(uintptr_t address) const
    {
        uintptr_t value = 0;
        if (!memory_.Read(address, value))
            return std::nullopt;
        return value;
    }

    std::optional<uintptr_t> NameStoreReader::EntryAt(int32_t index) const
    {
        const auto location = LocateEntry(index);
        return location ? std::optional<uintptr_t>(location->address) : std::nullopt;
    }

    std::optional<NameStoreReader::EntryLocation> NameStoreReader::LocateEntry(int32_t index) const
    {
        if (root_ == 0 || index < 0 || !layout_.IsValid())
            return std::nullopt;

        if (layout_.kind == NameContainerKind::Array)
        {
            const NameArrayLayout &array = layout_.array;
            if (array.elementsPerChunk <= 0)
                return std::nullopt;
            const int32_t chunkIndex = index / array.elementsPerChunk;
            if (chunkIndex >= 128)
                return std::nullopt;
            const int32_t withinChunk = index % array.elementsPerChunk;
            const auto chunksAddress = AddOffset(root_, static_cast<uintptr_t>(array.chunksOffset));
            if (!chunksAddress)
                return std::nullopt;
            const auto chunkSlot = AddScaled(*chunksAddress, chunkIndex, static_cast<int32_t>(sizeof(uintptr_t)));
            if (!chunkSlot)
                return std::nullopt;
            const auto rawChunk = ReadPointer(*chunkSlot);
            if (!rawChunk)
                return std::nullopt;
            const uintptr_t chunk = decode_.nameChunks(*rawChunk, *chunkSlot);
            if (chunk == 0 || !memory_.IsReadable(chunk, sizeof(uintptr_t)))
                return std::nullopt;
            const auto entrySlot = AddScaled(chunk, withinChunk, static_cast<int32_t>(sizeof(uintptr_t)));
            if (!entrySlot)
                return std::nullopt;
            const auto rawEntry = ReadPointer(*entrySlot);
            if (!rawEntry)
                return std::nullopt;
            const uintptr_t entry = decode_.nameEntry(*rawEntry, *entrySlot);
            return entry != 0 && memory_.IsReadable(entry, sizeof(uint32_t)) ? std::optional<EntryLocation>({entry, 2048}) : std::nullopt;
        }

        const NamePoolLayout &pool = layout_.pool;
        const int32_t blockIndex = index >> pool.blocksBit;
        if (blockIndex >= 8192)
            return std::nullopt;
        const int32_t entryIndex = index & ((1 << pool.blocksBit) - 1);
        const size_t blockSize = (size_t{1} << pool.blocksBit) * static_cast<size_t>(pool.entryStride);
        const size_t offset = static_cast<size_t>(entryIndex) * static_cast<size_t>(pool.entryStride);
        size_t usedBytes = blockSize;
        if (pool.maxChunkIndexOffset >= 0)
        {
            const uint64_t generation = memory_.AddressSpaceGeneration();
            if (generation != boundaryGeneration_)
            {
                currentBlock_.reset();
                boundaryGeneration_ = generation;
            }
            // The pool appends names. Reuse a published boundary until a newer name requires extending it.
            if (!currentBlock_ || static_cast<uint32_t>(blockIndex) > *currentBlock_ ||
                (static_cast<uint32_t>(blockIndex) == *currentBlock_ && offset >= byteCursor_))
            {
                const auto currentAddress = AddOffset(root_, static_cast<uintptr_t>(pool.maxChunkIndexOffset));
                uint32_t current = 0;
                uint32_t cursor = static_cast<uint32_t>(blockSize);
                if (!currentAddress || !memory_.ReadFreshBytes(*currentAddress, &current, sizeof(current)).Ok() || current >= 8192)
                    return std::nullopt;
                if (pool.byteCursorOffset >= 0)
                {
                    const auto cursorAddress = AddOffset(root_, static_cast<uintptr_t>(pool.byteCursorOffset));
                    if (!cursorAddress || !memory_.ReadFreshBytes(*cursorAddress, &cursor, sizeof(cursor)).Ok() || cursor > blockSize)
                        return std::nullopt;
                }
                currentBlock_ = current;
                byteCursor_ = cursor;
            }
            if (static_cast<uint32_t>(blockIndex) > *currentBlock_)
                return std::nullopt;
            if (static_cast<uint32_t>(blockIndex) == *currentBlock_)
                usedBytes = byteCursor_;
        }
        if (offset >= usedBytes || usedBytes - offset < static_cast<size_t>(pool.entryStringOffset))
            return std::nullopt;
        const auto blocksAddress = AddOffset(root_, static_cast<uintptr_t>(pool.blocksOffset));
        if (!blocksAddress)
            return std::nullopt;
        const auto blockSlot = AddScaled(*blocksAddress, blockIndex, static_cast<int32_t>(sizeof(uintptr_t)));
        if (!blockSlot)
            return std::nullopt;
        const auto rawBlock = ReadPointer(*blockSlot);
        if (!rawBlock)
            return std::nullopt;
        const uintptr_t block = decode_.nameBlocks(*rawBlock, *blockSlot);
        if (block == 0)
            return std::nullopt;
        const auto entryOffset = AddScaled(block, entryIndex, pool.entryStride);
        if (!entryOffset || !memory_.IsReadable(*entryOffset, sizeof(uint16_t)))
            return std::nullopt;
        return EntryLocation{decode_.nameEntry(*entryOffset, *entryOffset), usedBytes - offset};
    }

    std::optional<std::string> NameStoreReader::ReadBytesAsUtf8(uintptr_t address, size_t length) const
    {
        if (length == 0 || length > 1024)
            return std::nullopt;
        std::vector<uint8_t> bytes(length);
        if (!memory_.ReadBytes(address, bytes.data(), bytes.size()).Ok())
            return std::nullopt;
        size_t end = 0;
        while (end < bytes.size() && bytes[end] != 0)
            ++end;
        return std::string(reinterpret_cast<const char *>(bytes.data()), end);
    }

    std::optional<std::string> NameStoreReader::ReadUtf16AsUtf8(uintptr_t address, size_t length) const
    {
        if (length == 0 || length > 1024)
            return std::nullopt;
        std::vector<char16_t> chars(length);
        if (!memory_.ReadBytes(address, chars.data(), chars.size() * sizeof(char16_t)).Ok())
            return std::nullopt;

        std::string result;
        for (size_t index = 0; index < chars.size() && chars[index] != 0; ++index)
        {
            uint32_t codePoint = chars[index];
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
            {
                if (index + 1 >= chars.size())
                    return std::nullopt;
                const uint32_t low = chars[index + 1];
                if (low < 0xDC00 || low > 0xDFFF)
                    return std::nullopt;
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
            else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
                return std::nullopt;
            AppendUtf8(result, codePoint);
        }
        return result;
    }

    std::optional<std::string> NameStoreReader::ReadEntry(uintptr_t entry, size_t depth, size_t available) const
    {
        if (entry == 0 || depth >= 32)
            return std::nullopt;

        if (layout_.kind == NameContainerKind::Array)
        {
            const NameArrayLayout &array = layout_.array;
            uint32_t rawIndex = 0;
            const auto indexAddress = AddOffset(entry, static_cast<uintptr_t>(array.entryIndexOffset));
            const auto stringAddress = AddOffset(entry, static_cast<uintptr_t>(array.entryStringOffset));
            if (!indexAddress || !stringAddress || !memory_.Read(*indexAddress, rawIndex))
                return std::nullopt;
            const uint32_t indexField = decode_.nameEntryIndex(rawIndex, *indexAddress);
            const bool wide = (indexField & 1u) != 0;
            if (wide)
                return ReadUtf16AsUtf8(*stringAddress, 1024);
            return ReadBytesAsUtf8(*stringAddress, 1024);
        }

        const NamePoolLayout &pool = layout_.pool;
        uint16_t rawHeader = 0;
        const auto headerAddress = AddOffset(entry, static_cast<uintptr_t>(pool.entryHeaderOffset));
        const auto stringAddress = AddOffset(entry, static_cast<uintptr_t>(pool.entryStringOffset));
        if (!headerAddress || !stringAddress || !memory_.Read(*headerAddress, rawHeader))
            return std::nullopt;
        const uint16_t header = decode_.nameHeader(rawHeader, *headerAddress);
        const int32_t length = static_cast<int32_t>(header >> pool.entryLengthShift);
        if (length > 1024)
            return std::nullopt;
        if (length == 0)
        {
            const int32_t indexOffset = pool.entryStringOffset + (pool.entryStringOffset == 6 ? 2 : 0);
            const auto indexAddress = AddOffset(entry, static_cast<uintptr_t>(indexOffset));
            const auto numberAddress = indexAddress ? AddOffset(*indexAddress, sizeof(int32_t)) : std::nullopt;
            if (!indexAddress || !numberAddress || static_cast<size_t>(indexOffset) + 2 * sizeof(int32_t) > available)
                return std::nullopt;
            int32_t rawIndex = 0;
            uint32_t number = 0;
            if (!memory_.Read(*indexAddress, rawIndex) || !memory_.Read(*numberAddress, number))
                return std::nullopt;
            const int32_t index = decode_.nameIndex(rawIndex, *indexAddress);
            const auto baseEntry = LocateEntry(index);
            auto base = baseEntry ? ReadEntry(baseEntry->address, depth + 1, baseEntry->available) : std::nullopt;
            if (!base)
                return std::nullopt;
            if (number > 0)
                *base += "_" + std::to_string(number - 1);
            return base;
        }
        const bool wide = (header & pool.entryWideMask) != 0;
        const size_t stringOffset = static_cast<size_t>(pool.entryStringOffset);
        const size_t bytes = static_cast<size_t>(length) * (wide ? 2u : 1u);
        if (stringOffset > available || bytes > available - stringOffset)
            return std::nullopt;
        if (wide)
            return ReadUtf16AsUtf8(*stringAddress, static_cast<size_t>(length));
        return ReadBytesAsUtf8(*stringAddress, static_cast<size_t>(length));
    }

    std::optional<std::string> NameStoreReader::ReadName(int32_t index) const
    {
        const auto entry = LocateEntry(index);
        return entry ? ReadEntry(entry->address, 0, entry->available) : std::nullopt;
    }

    std::optional<std::string> NameStoreReader::ReadFName(uintptr_t fnameAddress) const
    {
        auto name = ReadComparisonName(fnameAddress);
        if (!name)
            return std::nullopt;

        if (fname_.displayIndex >= 0)
        {
            const auto displayAddress = AddOffset(fnameAddress, static_cast<uintptr_t>(fname_.displayIndex));
            int32_t rawDisplayIndex = 0;
            if (!displayAddress || !memory_.Read(*displayAddress, rawDisplayIndex))
                return std::nullopt;
            const int32_t displayIndex = decode_.nameIndex(rawDisplayIndex, *displayAddress);
            const auto displayName = ReadName(displayIndex);
            if (!displayName || displayName->empty())
                return std::nullopt;
            *name = *displayName;
        }

        if (fname_.numberLayout == FNameNumberLayout::Inline && fname_.number >= 0)
        {
            const auto numberAddress = AddOffset(fnameAddress, static_cast<uintptr_t>(fname_.number));
            uint32_t number = 0;
            if (!numberAddress || !memory_.Read(*numberAddress, number))
                return std::nullopt;
            if (number > 0)
                *name += "_" + std::to_string(number - 1);
        }
        return name;
    }

    std::optional<std::string> NameStoreReader::ReadComparisonName(uintptr_t fnameAddress) const
    {
        if (fnameAddress == 0 || fname_.comparisonIndex < 0)
            return std::nullopt;
        const auto indexAddress = AddOffset(fnameAddress, static_cast<uintptr_t>(fname_.comparisonIndex));
        if (!indexAddress)
            return std::nullopt;
        int32_t rawIndex = 0;
        if (!memory_.Read(*indexAddress, rawIndex))
            return std::nullopt;
        const int32_t index = decode_.nameIndex(rawIndex, *indexAddress);
        return ReadName(index);
    }
} // namespace anduefker::ue
