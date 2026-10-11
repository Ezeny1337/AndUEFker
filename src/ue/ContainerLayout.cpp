#include "anduefker/ue/ContainerLayout.hpp"
#include "anduefker/ue/StorageLayout.hpp"

#include <algorithm>
#include <cstring>

namespace anduefker::ue
{
    std::optional<ContainerLayoutDescription> DescribeHeapContainer(ir::PropertyKind kind, int32_t pointerWidth)
    {
        if (pointerWidth != 4 && pointerWidth != 8)
            return std::nullopt;
        const auto align = [pointerWidth](int32_t size)
        { return (size + pointerWidth - 1) / pointerWidth * pointerWidth; };
        ContainerLayoutDescription result;
        result.alignment = pointerWidth;
        const int32_t arraySize = pointerWidth + 8;
        if (kind == ir::PropertyKind::Array)
        {
            result.id = "heap-array-p" + std::to_string(pointerWidth);
            result.size = arraySize;
            result.offsets = {{"data", 0}, {"num", pointerWidth}, {"max", pointerWidth + 4}};
            return result;
        }
        if (kind != ir::PropertyKind::Map && kind != ir::PropertyKind::Set)
            return std::nullopt;
        // allocation bits 使用 TInlineAllocator<4>，hash buckets 使用 TInlineAllocator<1>
        const int32_t bits = arraySize;
        const int32_t bitsSecondary = bits + 16;
        const int32_t bitsNum = bitsSecondary + pointerWidth;
        const int32_t bitsMax = bitsNum + 4;
        const int32_t firstFree = bitsMax + 4;
        const int32_t freeCount = firstFree + 4;
        const int32_t hash = align(freeCount + 4);
        const int32_t hashSecondary = hash + align(4);
        const int32_t hashSize = hashSecondary + pointerWidth;
        result.id = "heap-sparse-set-p" + std::to_string(pointerWidth);
        result.size = align(hashSize + 4);
        result.offsets = {{"data", 0}, {"data_num", pointerWidth}, {"data_max", pointerWidth + 4}, {"allocation_bits_inline", bits}, {"allocation_bits_secondary", bitsSecondary}, {"allocation_bits_num", bitsNum}, {"allocation_bits_max", bitsMax}, {"first_free_index", firstFree}, {"num_free_indices", freeCount}, {"hash_inline", hash}, {"hash_secondary", hashSecondary}, {"hash_size", hashSize}};
        return result;
    }

    std::optional<ContainerLayoutDescription> DescribeMemoryImageContainer(ir::PropertyKind kind, int32_t pointerWidth)
    {
        if (pointerWidth != 4 && pointerWidth != 8)
            return std::nullopt;
        // Android 上 MemoryImage pointer 固定占 8 字节，不复用 heap 的 inline allocator
        ContainerLayoutDescription result;
        result.alignment = 8;
        if (kind == ir::PropertyKind::Array)
        {
            result.id = "memory-image-array-p" + std::to_string(pointerWidth);
            result.size = 16;
            result.offsets = {{"data", 0}, {"num", 8}, {"max", 12}};
        }
        else if (kind == ir::PropertyKind::Map || kind == ir::PropertyKind::Set)
        {
            result.id = "memory-image-sparse-set-p" + std::to_string(pointerWidth);
            result.size = 56;
            result.offsets = {{"data", 0}, {"data_num", 8}, {"data_max", 12}, {"allocation_bits_secondary", 16}, {"allocation_bits_num", 24}, {"allocation_bits_max", 28}, {"first_free_index", 32}, {"num_free_indices", 36}, {"hash_secondary", 40}, {"hash_size", 48}};
        }
        else
            return std::nullopt;
        return result;
    }

    void DecodeSparseLayout(ir::ContainerStorageObservation &observation, ir::PropertyKind kind)
    {
        auto &storage = observation.storage;
        storage.layoutStatus = "layout-unreadable";
        const size_t start = kind == ir::PropertyKind::Map ? 1 : 0;
        if (!observation.readable || observation.bytes.size() < (start + 5) * sizeof(int32_t))
            return;
        const auto word = [&](size_t index)
        {
            int32_t value = 0;
            std::memcpy(&value, observation.bytes.data() + index * sizeof(value), sizeof(value));
            return value;
        };
        storage.valueOffset = start == 1 ? word(0) : 0;
        storage.hashNextOffset = word(start);
        storage.hashIndexOffset = word(start + 1);
        storage.setElementSize = word(start + 2);
        storage.elementAlignment = word(start + 3);
        storage.elementStride = word(start + 4);
        const int64_t alignment = storage.elementAlignment;
        const int64_t payloadEnd = start == 1 ? static_cast<int64_t>(storage.valueOffset) + observation.valueSize
                                              : observation.innerSize;
        const bool pairFits = start == 0 || storage.valueOffset >= observation.innerSize;
        const auto align = [](int64_t size, int64_t value)
        { return (size + value - 1) / value * value; };
        bool matches = false;
        if (observation.innerSize > 0 && (start == 0 || observation.valueSize > 0) && pairFits &&
            alignment >= 4 && (alignment & (alignment - 1)) == 0)
        {
            const int64_t hashNext = align(payloadEnd, start == 0 ? 4 : alignment);
            const int64_t hashIndex = hashNext + 4;
            const int64_t setSize = align(hashIndex + 4, alignment);
            const int64_t sparseSize = std::max<int64_t>(setSize, 8);
            matches = sparseSize <= INT32_MAX && storage.hashNextOffset == hashNext &&
                      storage.hashIndexOffset == hashIndex && storage.setElementSize == setSize &&
                      storage.elementStride == sparseSize;
        }
        observation.sparseFormulaMatches = matches;
        storage.layoutStatus = matches ? "conditional-formula-match" : "layout-formula-mismatch";
    }

    std::optional<int32_t> PropertyStorageAlignment(const ir::TypeReferenceIR &reference,
                                                    const std::unordered_map<uintptr_t, const ir::TypeIR *> &types,
                                                    int32_t pointerWidth, int32_t nameSize, size_t depth)
    {
        if (!reference.detailsResolved || reference.elementSize <= 0 || reference.arrayDim <= 0 ||
            depth >= 32 || (pointerWidth != 4 && pointerWidth != 8))
            return std::nullopt;
        const auto scalar = [&](int32_t size) -> std::optional<int32_t>
        { return reference.elementSize == size ? std::optional<int32_t>(size) : std::nullopt; };
        if (const auto layout = CommonValueStorage(reference, pointerWidth, nameSize))
            return layout->alignment;
        switch (reference.kind)
        {
        case ir::PropertyKind::Bool:
        case ir::PropertyKind::Byte:
        case ir::PropertyKind::Int8:
            return scalar(1);
        case ir::PropertyKind::Int16:
        case ir::PropertyKind::UInt16:
            return scalar(2);
        case ir::PropertyKind::Int32:
        case ir::PropertyKind::UInt32:
        case ir::PropertyKind::Float:
            return scalar(4);
        case ir::PropertyKind::Int64:
        case ir::PropertyKind::UInt64:
        case ir::PropertyKind::Double:
            return scalar(8);
        case ir::PropertyKind::Name:
            if (reference.elementSize == nameSize && (nameSize == 4 || nameSize == 8 || nameSize == 12))
                return 4;
            return std::nullopt;
        case ir::PropertyKind::Object:
        case ir::PropertyKind::Class:
            return scalar(pointerWidth);
        case ir::PropertyKind::WeakObject:
            if (reference.elementSize == 8)
                return 4;
            return std::nullopt;
        case ir::PropertyKind::MulticastDelegate:
            if (reference.delegateStorage == ir::DelegateStorageKind::SparseMulticast && reference.elementSize == 1)
                return 1;
            return std::nullopt;
        case ir::PropertyKind::String:
            if (reference.elementSize == pointerWidth + 8)
                return pointerWidth;
            return std::nullopt;
        case ir::PropertyKind::Interface:
            if (reference.elementSize == 2 * pointerWidth)
                return pointerWidth;
            return std::nullopt;
        case ir::PropertyKind::Enum:
            if (reference.inner && reference.inner->elementSize == reference.elementSize)
                return PropertyStorageAlignment(*reference.inner, types, pointerWidth, nameSize, depth + 1);
            return std::nullopt;
        case ir::PropertyKind::Struct:
        {
            const auto type = types.find(reference.referencedObject);
            if (type != types.end() && type->second->size == reference.elementSize && type->second->minAlignment > 0)
                return type->second->minAlignment;
            return std::nullopt;
        }
        case ir::PropertyKind::Array:
        case ir::PropertyKind::Map:
        case ir::PropertyKind::Set:
            if (reference.containerStorage && !reference.containerStorage->layoutId.empty())
                return reference.containerStorage->allocator == ir::ContainerAllocatorKind::MemoryImage ? 8 : pointerWidth;
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    std::optional<ir::ContainerStorageIR> DescribeSparseElement(ir::PropertyKind kind,
                                                                int32_t keySize, int32_t valueSize, int32_t keyAlignment, int32_t valueAlignment)
    {
        if ((kind != ir::PropertyKind::Map && kind != ir::PropertyKind::Set) ||
            keySize <= 0 || keyAlignment <= 0 || (keyAlignment & (keyAlignment - 1)) != 0 ||
            (kind == ir::PropertyKind::Map &&
             (valueSize <= 0 || valueAlignment <= 0 || (valueAlignment & (valueAlignment - 1)) != 0)))
            return std::nullopt;
        const auto align = [](int64_t size, int64_t alignment)
        { return (size + alignment - 1) / alignment * alignment; };
        const bool map = kind == ir::PropertyKind::Map;
        const int64_t valueOffset = map ? align(keySize, valueAlignment) : 0;
        const int64_t payloadAlignment = map ? std::max(keyAlignment, valueAlignment) : keyAlignment;
        const int64_t payloadSize = map ? align(valueOffset + valueSize, payloadAlignment) : keySize;
        const int64_t alignment = std::max<int64_t>(payloadAlignment, 4);
        const int64_t hashNext = align(payloadSize, 4);
        const int64_t hashIndex = hashNext + 4;
        const int64_t setSize = align(hashIndex + 4, alignment);
        const int64_t stride = std::max<int64_t>(setSize, 8);
        if (stride > INT32_MAX)
            return std::nullopt;
        ir::ContainerStorageIR result;
        result.valueOffset = static_cast<int32_t>(valueOffset);
        result.hashNextOffset = static_cast<int32_t>(hashNext);
        result.hashIndexOffset = static_cast<int32_t>(hashIndex);
        result.setElementSize = static_cast<int32_t>(setSize);
        result.elementAlignment = static_cast<int32_t>(alignment);
        result.elementStride = static_cast<int32_t>(stride);
        return result;
    }

    void ValidateSparseLayout(ir::ContainerStorageIR &storage, ir::PropertyKind kind,
                              int32_t keySize, int32_t valueSize, int32_t keyAlignment, int32_t valueAlignment)
    {
        const auto expected = DescribeSparseElement(kind, keySize, valueSize, keyAlignment, valueAlignment);
        if (!expected || storage.elementStride < 0)
            return;
        const bool matches = storage.valueOffset == expected->valueOffset &&
                             storage.hashNextOffset == expected->hashNextOffset &&
                             storage.hashIndexOffset == expected->hashIndexOffset &&
                             storage.setElementSize == expected->setElementSize &&
                             storage.elementAlignment == expected->elementAlignment &&
                             storage.elementStride == expected->elementStride;
        storage.layoutStatus = matches ? "independent-formula-match" : "independent-formula-mismatch";
        if (!matches)
        {
            storage.layoutId.clear();
            storage.headerStatus = "element-layout-mismatch";
        }
    }
} // namespace anduefker::ue
