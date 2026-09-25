#include "anduefker/ue/ObjectStoreReader.hpp"

#include <limits>

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
    } // namespace

    ObjectStoreReader::ObjectStoreReader(const IMemorySource &memory,
                                         uintptr_t root,
                                         const ObjectContainerLayout &layout,
                                         const DecodePlan &decode)
        : memory_(memory), root_(root), layout_(layout), decode_(decode)
    {
    }

    std::optional<uintptr_t> ObjectStoreReader::ReadPointer(uintptr_t address) const
    {
        uintptr_t value = 0;
        if (!memory_.Read(address, value))
            return std::nullopt;
        return value;
    }

    bool ObjectStoreReader::Initialize()
    {
        initialized_ = false;
        storage_ = 0;
        count_ = 0;

        if (root_ == 0 || !layout_.IsValid())
            return false;

        const auto countAddress = AddOffset(root_, static_cast<uintptr_t>(layout_.numElementsOffset));
        const auto storageAddress = AddOffset(root_, static_cast<uintptr_t>(layout_.objectsOffset));
        if (!countAddress || !storageAddress)
            return false;

        int32_t rawCount = 0;
        uintptr_t rawStorage = 0;
        if (!memory_.Read(*countAddress, rawCount) || !memory_.Read(*storageAddress, rawStorage))
            return false;

        count_ = decode_.objectCount(rawCount, *countAddress);
        storage_ = decode_.objectStorage(rawStorage, *storageAddress);
        if (count_ <= 0 || count_ > 0x08000000 || storage_ == 0)
            return false;

        if (!memory_.IsReadable(storage_, sizeof(uintptr_t)))
            return false;

        initialized_ = true;
        return true;
    }

    std::optional<uintptr_t> ObjectStoreReader::ReadItemAddress(int32_t index) const
    {
        if (!initialized_ || index < 0 || index >= count_)
            return std::nullopt;

        if (layout_.kind == ObjectContainerKind::Fixed)
            return AddScaled(storage_, index, layout_.itemStride);

        if (layout_.elementsPerChunk <= 0)
            return std::nullopt;

        const int32_t chunkIndex = index / layout_.elementsPerChunk;
        const int32_t withinChunk = index % layout_.elementsPerChunk;
        const auto chunkSlot = AddScaled(storage_, chunkIndex, static_cast<int32_t>(sizeof(uintptr_t)));
        if (!chunkSlot)
            return std::nullopt;

        const auto rawChunk = ReadPointer(*chunkSlot);
        if (!rawChunk)
            return std::nullopt;
        const uintptr_t chunk = decode_.objectChunk(*rawChunk, *chunkSlot);
        if (chunk == 0 || !memory_.IsReadable(chunk, sizeof(uintptr_t)))
            return std::nullopt;
        return AddScaled(chunk, withinChunk, layout_.itemStride);
    }

    std::optional<uintptr_t> ObjectStoreReader::ObjectAt(int32_t index) const
    {
        const auto item = ReadItemAddress(index);
        if (!item)
            return std::nullopt;

        const auto objectSlot = AddOffset(*item, static_cast<uintptr_t>(layout_.itemObjectOffset));
        if (!objectSlot)
            return std::nullopt;

        const auto rawObject = ReadPointer(*objectSlot);
        if (!rawObject || *rawObject == 0)
            return std::nullopt;

        const uintptr_t object = decode_.objectPointer(*rawObject, *objectSlot);
        if (object == 0 || !memory_.IsReadable(object, sizeof(uintptr_t)))
            return std::nullopt;
        return object;
    }
} // namespace anduefker::ue
