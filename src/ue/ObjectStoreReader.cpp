#include "anduefker/ue/ObjectStoreReader.hpp"

#include <limits>
#include <optional>

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
        if (!memory_.ReadFreshBytes(*countAddress, &rawCount, sizeof(rawCount)).Ok() || !memory_.Read(*storageAddress, rawStorage))
            return false;

        count_ = decode_.objectCount(rawCount, *countAddress);
        storage_ = decode_.objectStorage(rawStorage, *storageAddress);
        if (count_ <= 0 || count_ > 0x08000000 || storage_ == 0)
            return false;

        if (!memory_.IsReadable(storage_, sizeof(uintptr_t)))
            return false;

        initialized_ = true;
        const auto boundary = RefreshCount();
        initialized_ = boundary.valid;
        return initialized_;
    }

    ObjectStoreBoundary ObjectStoreReader::RefreshCount()
    {
        ObjectStoreBoundary result;
        const auto countAddress = AddOffset(root_, static_cast<uintptr_t>(layout_.numElementsOffset));
        const auto storageAddress = AddOffset(root_, static_cast<uintptr_t>(layout_.objectsOffset));
        if (!initialized_ || !countAddress || !storageAddress)
        {
            result.reason = "object-store-unavailable";
            return result;
        }
        result.countAddress = *countAddress;
        int32_t rawCount = 0;
        uintptr_t rawStorage = 0;
        if (!memory_.ReadFreshBytes(*countAddress, &rawCount, sizeof(rawCount)).Ok() ||
            !memory_.ReadFreshBytes(*storageAddress, &rawStorage, sizeof(rawStorage)).Ok())
        {
            result.reason = "object-boundary-unreadable";
            return result;
        }
        result.count = decode_.objectCount(rawCount, *countAddress);
        if (result.count <= 0 || result.count > 0x08000000 || result.count < count_ ||
            decode_.objectStorage(rawStorage, *storageAddress) != storage_)
        {
            result.reason = "object-count-decreased-invalid-or-storage-changed";
            return result;
        }
        const auto readCapacity = [&](int32_t offset, int32_t minimum, bool decodeCount)
        {
            if (offset < 0)
                return true;
            const auto address = AddOffset(root_, static_cast<uintptr_t>(offset));
            int32_t value = 0;
            if (!address || !memory_.ReadFreshBytes(*address, &value, sizeof(value)).Ok())
                return false;
            if (decodeCount)
                value = decode_.objectCount(value, *address);
            return value >= minimum && value <= 0x08000000;
        };
        if (!readCapacity(layout_.maxElementsOffset, result.count, true))
        {
            result.reason = "object-capacity-invalid-or-unreadable";
            return result;
        }
        if (layout_.kind == ObjectContainerKind::Chunked)
        {
            const int32_t chunks = (result.count - 1) / layout_.elementsPerChunk + 1;
            if (!readCapacity(layout_.numChunksOffset, chunks, false) || !readCapacity(layout_.maxChunksOffset, chunks, false))
            {
                result.reason = "object-chunk-capacity-invalid-or-unreadable";
                return result;
            }
        }
        else if (result.count != count_)
        {
            result.reason = "fixed-object-count-changed";
            return result;
        }
        count_ = result.count;
        result.valid = true;
        result.reason = "object-boundary-valid";
        return result;
    }

    ObjectReadResult ObjectStoreReader::ReadObject(int32_t index, bool fresh) const
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
            const auto read = fresh ? memory_.ReadFreshBytes(*chunkSlot, &rawChunk, sizeof(rawChunk))
                                    : memory_.ReadBytes(*chunkSlot, &rawChunk, sizeof(rawChunk));
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
        const auto read = fresh ? memory_.ReadFreshBytes(*objectSlot, &rawObject, sizeof(rawObject))
                                : memory_.ReadBytes(*objectSlot, &rawObject, sizeof(rawObject));
        if (!read.Ok())
            return {ObjectReadStatus::UnreadableObject, 0, *objectSlot, read.error};
        rawObject = ::anduefker::binding::UnpackObjectItemPointer(rawObject, layout_.packedPointers);
        if (rawObject == 0)
            return {ObjectReadStatus::Empty, 0, *objectSlot};

        const uintptr_t object = decode_.objectPointer(rawObject, *objectSlot);
        if (object == 0 || !memory_.IsReadable(object, sizeof(uintptr_t)))
            return {ObjectReadStatus::InvalidObject, object, *objectSlot};
        return {ObjectReadStatus::Valid, object, *objectSlot};
    }

    std::optional<uint32_t> ObjectStoreReader::ReadInternalFlags(const ObjectReadResult &object) const
    {
        // Supported FUObjectItem layouts place internal flags after Object/ObjectPtrLow.
        const size_t pointerSize = layout_.packedPointers ? sizeof(uint32_t) : sizeof(uintptr_t);
        if (!object.IsValid() || layout_.itemObjectOffset != 0 ||
            static_cast<size_t>(layout_.itemStride) < pointerSize + sizeof(uint32_t))
            return std::nullopt;
        const auto address = AddOffset(object.readAddress, pointerSize);
        uint32_t flags = 0;
        if (!address || !memory_.ReadFreshBytes(*address, &flags, sizeof(flags)).Ok())
            return std::nullopt;
        return flags;
    }
} // namespace anduefker::ue
