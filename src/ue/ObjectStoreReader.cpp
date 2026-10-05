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

    const char *ObjectReadStatusName(ObjectReadStatus status)
    {
        switch (status)
        {
        case ObjectReadStatus::Valid:
            return "valid";
        case ObjectReadStatus::Empty:
            return "empty";
        case ObjectReadStatus::NotInitialized:
            return "not-initialized";
        case ObjectReadStatus::InvalidIndex:
            return "invalid-index";
        case ObjectReadStatus::AddressOverflow:
            return "address-overflow";
        case ObjectReadStatus::UnreadableChunk:
            return "unreadable-chunk";
        case ObjectReadStatus::InvalidChunk:
            return "invalid-chunk";
        case ObjectReadStatus::UnreadableObject:
            return "unreadable-object";
        case ObjectReadStatus::InvalidObject:
            return "invalid-object";
        }
        return "unknown";
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

    std::optional<uintptr_t> ObjectStoreReader::ObjectAt(int32_t index) const
    {
        const ObjectReadResult result = ReadObject(index);
        return result.IsValid() ? std::optional<uintptr_t>(result.address) : std::nullopt;
    }

    ObjectReadResult ObjectStoreReader::ReadObject(int32_t index) const
    {
        if (!initialized_)
            return {};
        if (index < 0 || index >= count_)
            return {ObjectReadStatus::InvalidIndex};

        std::optional<uintptr_t> item;
        if (layout_.kind == ObjectContainerKind::Fixed)
        {
            item = AddScaled(storage_, index, layout_.itemStride);
        }
        else
        {
            const int32_t chunkIndex = index / layout_.elementsPerChunk;
            const int32_t withinChunk = index % layout_.elementsPerChunk;
            const auto chunkSlot = AddScaled(storage_, chunkIndex, static_cast<int32_t>(sizeof(uintptr_t)));
            if (!chunkSlot)
                return {ObjectReadStatus::AddressOverflow};

            uintptr_t rawChunk = 0;
            const auto read = memory_.ReadBytes(*chunkSlot, &rawChunk, sizeof(rawChunk));
            if (!read.Ok())
                return {ObjectReadStatus::UnreadableChunk, 0, *chunkSlot, read.error};
            const uintptr_t chunk = decode_.objectChunk(rawChunk, *chunkSlot);
            if (chunk == 0 || !memory_.IsReadable(chunk, sizeof(uintptr_t)))
                return {ObjectReadStatus::InvalidChunk, chunk, *chunkSlot};
            item = AddScaled(chunk, withinChunk, layout_.itemStride);
        }
        if (!item)
            return {ObjectReadStatus::AddressOverflow};

        const auto objectSlot = AddOffset(*item, static_cast<uintptr_t>(layout_.itemObjectOffset));
        if (!objectSlot)
            return {ObjectReadStatus::AddressOverflow};

        uintptr_t rawObject = 0;
        const auto read = memory_.ReadBytes(*objectSlot, &rawObject, sizeof(rawObject));
        if (!read.Ok())
            return {ObjectReadStatus::UnreadableObject, 0, *objectSlot, read.error};
        if (rawObject == 0)
            return {ObjectReadStatus::Empty, 0, *objectSlot};

        const uintptr_t object = decode_.objectPointer(rawObject, *objectSlot);
        if (object == 0 || !memory_.IsReadable(object, sizeof(uintptr_t)))
            return {ObjectReadStatus::InvalidObject, object, *objectSlot};
        return {ObjectReadStatus::Valid, object, *objectSlot};
    }
} // namespace anduefker::ue
