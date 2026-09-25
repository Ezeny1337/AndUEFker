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
        if (root_ == 0 || index < 0 || !layout_.IsValid())
            return std::nullopt;

        if (layout_.kind == NameContainerKind::Array)
        {
            const NameArrayLayout &array = layout_.array;
            if (array.elementsPerChunk <= 0)
                return std::nullopt;
            const int32_t chunkIndex = index / array.elementsPerChunk;
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
            const auto entrySlot = AddScaled(chunk, withinChunk, static_cast<int32_t>(sizeof(uintptr_t)));
            if (!entrySlot)
                return std::nullopt;
            const auto rawEntry = ReadPointer(*entrySlot);
            if (!rawEntry)
                return std::nullopt;
            const uintptr_t entry = decode_.nameEntry(*rawEntry, *entrySlot);
            return entry != 0 && memory_.IsReadable(entry, sizeof(uint32_t)) ? std::optional<uintptr_t>(entry) : std::nullopt;
        }

        const NamePoolLayout &pool = layout_.pool;
        const int32_t blockIndex = index >> pool.blocksBit;
        const int32_t entryIndex = index & ((1 << pool.blocksBit) - 1);
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
        const auto entryOffset = AddScaled(block, entryIndex, pool.entryStride);
        if (!entryOffset || !memory_.IsReadable(*entryOffset, sizeof(uint16_t)))
            return std::nullopt;
        return decode_.nameEntry(*entryOffset, *entryOffset);
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
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index + 1 < chars.size())
            {
                const uint32_t low = chars[index + 1];
                if (low >= 0xDC00 && low <= 0xDFFF)
                {
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    ++index;
                }
            }
            AppendUtf8(result, codePoint);
        }
        return result;
    }

    std::optional<std::string> NameStoreReader::ReadEntry(uintptr_t entry) const
    {
        if (entry == 0)
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
        if (length == 0)
        {
            const int32_t indexOffset = pool.entryStringOffset + (pool.entryStringOffset == 6 ? 2 : 0);
            const auto indexAddress = AddOffset(entry, static_cast<uintptr_t>(indexOffset));
            const auto numberAddress = indexAddress ? AddOffset(*indexAddress, sizeof(int32_t)) : std::nullopt;
            if (!indexAddress || !numberAddress)
                return std::nullopt;
            int32_t rawIndex = 0;
            int32_t number = 0;
            if (!memory_.Read(*indexAddress, rawIndex) || !memory_.Read(*numberAddress, number))
                return std::nullopt;
            const int32_t index = decode_.nameIndex(rawIndex, *indexAddress);
            auto base = ReadName(index);
            if (!base)
                return std::nullopt;
            if (number > 0)
                *base += "_" + std::to_string(number - 1);
            return base;
        }
        if (length <= 0 || length > 1024)
            return std::nullopt;
        const bool wide = (header & pool.entryWideMask) != 0;
        if (wide)
            return ReadUtf16AsUtf8(*stringAddress, static_cast<size_t>(length));
        return ReadBytesAsUtf8(*stringAddress, static_cast<size_t>(length));
    }

    std::optional<std::string> NameStoreReader::ReadName(int32_t index) const
    {
        const auto entry = EntryAt(index);
        return entry ? ReadEntry(*entry) : std::nullopt;
    }

    std::optional<std::string> NameStoreReader::ReadFName(uintptr_t fnameAddress) const
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
        auto name = ReadName(index);
        if (!name)
            return std::nullopt;

        if (!features_.outlineNumberName && fname_.number >= 0)
        {
            const auto numberAddress = AddOffset(fnameAddress, static_cast<uintptr_t>(fname_.number));
            if (numberAddress)
            {
                uint32_t number = 0;
                if (memory_.Read(*numberAddress, number) && number > 0)
                    *name += "_" + std::to_string(number - 1);
            }
        }
        return name;
    }
} // namespace anduefker::ue
