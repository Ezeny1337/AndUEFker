#include "nextdumper/ue/ContainerReader.hpp"
#include "nextdumper/ue/StorageLayout.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace nextdumper::ue
{
    namespace
    {
        constexpr int32_t kMaxSlots = 65536;

        std::optional<uintptr_t> Add(uintptr_t address, uint64_t offset)
        {
            if (offset > UINTPTR_MAX || address > UINTPTR_MAX - static_cast<uintptr_t>(offset))
                return std::nullopt;
            return address + static_cast<uintptr_t>(offset);
        }
    }

    ContainerValue ReadContainerValue(const memory::IMemorySource &memory, uintptr_t address,
                                      ir::PropertyKind kind, const ir::ContainerStorageIR &storage,
                                      int32_t pointerWidth, int32_t arrayStride, ContainerReadLimits limits)
    {
        ContainerValue result;
        result.address = address;
        const bool memoryImage = storage.allocator == ir::ContainerAllocatorKind::MemoryImage;
        const auto layout = memoryImage ? DescribeMemoryImageContainer(kind, pointerWidth)
                                        : DescribeHeapContainer(kind, pointerWidth);
        if (!layout || storage.allocator == ir::ContainerAllocatorKind::Unknown || layout->id != storage.layoutId ||
            (memoryImage && pointerWidth == 4) || address == 0 ||
            limits.maxSamples == 0 || limits.maxSamples > 64 || limits.previewBytes == 0 || limits.previewBytes > 128)
        {
            result.status = "unsupported-or-unselected-storage";
            return result;
        }
        result.layoutId = layout->id;
        result.header.resize(static_cast<size_t>(layout->size));
        if (!memory.IsReadable(address, result.header.size()) ||
            !memory.ReadBytes(address, result.header.data(), result.header.size()).Ok())
        {
            result.header.clear();
            result.status = "header-unreadable";
            return result;
        }
        const auto offset = [&](const char *name) -> int32_t
        {
            const auto field = std::find_if(layout->offsets.begin(), layout->offsets.end(),
                                            [name](const auto &value)
                                            { return value.first == name; });
            return field == layout->offsets.end() ? -1 : field->second;
        };
        const auto raw = [&](int32_t at, int32_t width) -> uint64_t
        {
            uint64_t value = 0;
            if (at >= 0 && width > 0 && width <= 8 && static_cast<size_t>(at) <= result.header.size() &&
                static_cast<size_t>(width) <= result.header.size() - static_cast<size_t>(at))
                std::memcpy(&value, result.header.data() + at, static_cast<size_t>(width));
            return value;
        };
        const auto word = [&](const char *name) -> int32_t
        {
            const uint32_t bits = static_cast<uint32_t>(raw(offset(name), 4));
            int32_t value = 0;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        };
        const bool array = kind == ir::PropertyKind::Array;
        result.slots = word(array ? "num" : "data_num");
        result.capacity = word(array ? "max" : "data_max");
        if (result.slots < 0 || result.capacity < result.slots || result.slots > kMaxSlots)
        {
            result.status = "invalid-counts-or-slot-budget";
            return result;
        }
        const int32_t stride = array ? arrayStride : storage.elementStride;
        if (stride <= 0)
        {
            result.status = "element-stride-unavailable";
            return result;
        }
        const int32_t freeCount = array ? 0 : word("num_free_indices");
        const int32_t firstFree = array ? -1 : word("first_free_index");
        // 4.23-5.6 的 MoveAssign 会把 moved-from 的 FirstFreeIndex 设为 0
        // NumFreeIndices == 0 时该成员不参与遍历，不能强制要求 INDEX_NONE
        if (freeCount < 0 || freeCount > result.slots || (!array && (word("allocation_bits_num") != result.slots || word("allocation_bits_max") < result.slots || (freeCount > 0 && (firstFree < 0 || firstFree >= result.slots)))))
        {
            result.status = "invalid-sparse-header";
            return result;
        }
        result.count = result.slots - freeCount;
        const int32_t pointerSize = memoryImage ? 8 : pointerWidth;
        const uint64_t dataRaw = raw(offset("data"), pointerSize);
        result.dataFrozen = memoryImage && (dataRaw & 1) != 0;
        const auto decoded = [&](int32_t at, MemoryImagePointerEncoding encoding) -> std::optional<uintptr_t>
        {
            const auto location = Add(address, static_cast<uint64_t>(at));
            if (at < 0 || !location)
                return std::nullopt;
            if (!memoryImage)
                return static_cast<uintptr_t>(raw(at, pointerWidth));
            const auto pointer = DecodeMemoryImagePointer(raw(at, 8), *location, pointerWidth, encoding);
            if (!pointer || pointer->address > UINTPTR_MAX)
                return std::nullopt;
            return static_cast<uintptr_t>(pointer->address);
        };
        struct Pointers
        {
            uintptr_t data = 0, bits = 0, hash = 0;
        };
        std::optional<Pointers> selected;
        bool frozen = result.dataFrozen;
        size_t accepted = 0;
        for (auto encoding : {MemoryImagePointerEncoding::RelativeShift1, MemoryImagePointerEncoding::PackedOffset40})
        {
            const auto data = decoded(offset("data"), encoding);
            if (!data || (result.slots != 0 && (*data == 0 || !memory.IsReadable(*data, 1))))
                continue;
            Pointers candidate{*data, 0, 0};
            if (!array)
            {
                const auto bits = decoded(offset("allocation_bits_secondary"), encoding);
                const auto hash = decoded(offset("hash_secondary"), encoding);
                if (!bits || !hash)
                    continue;
                const auto inlineBits = Add(address, static_cast<uint64_t>(std::max(0, offset("allocation_bits_inline"))));
                const auto inlineHash = Add(address, static_cast<uint64_t>(std::max(0, offset("hash_inline"))));
                if (!inlineBits || !inlineHash)
                    continue;
                candidate.bits = *bits ? *bits : memoryImage ? 0
                                                             : *inlineBits;
                candidate.hash = *hash ? *hash : memoryImage ? 0
                                                             : *inlineHash;
                const int32_t hashSize = word("hash_size");
                if ((!memoryImage && *bits == 0 && word("allocation_bits_max") > 128) ||
                    (!memoryImage && *hash == 0 && hashSize > 1) || hashSize < 0 ||
                    hashSize > 2 * kMaxSlots || (result.count > 0 && hashSize == 0) ||
                    (hashSize > 0 && ((hashSize & (hashSize - 1)) != 0 || candidate.hash == 0 ||
                                      !memory.IsReadable(candidate.hash, 4))) ||
                    (result.slots > 0 && (candidate.bits == 0 || !memory.IsReadable(candidate.bits, 4))))
                    continue;
                frozen = frozen || (memoryImage && ((raw(offset("allocation_bits_secondary"), 8) |
                                                     raw(offset("hash_secondary"), 8)) &
                                                    1) != 0);
            }
            if (selected && (selected->data != candidate.data || selected->bits != candidate.bits ||
                             selected->hash != candidate.hash))
            {
                result.status = "memory-image-pointer-candidate-disagreement";
                return result;
            }
            selected = candidate;
            ++accepted;
            result.pointerEncoding = encoding == MemoryImagePointerEncoding::RelativeShift1 ? "relative-shift-1" : "packed-offset-40";
        }
        if (!selected)
        {
            result.status = "pointer-targets-unreadable-or-invalid";
            return result;
        }
        result.data = selected->data;
        if (!memoryImage)
            result.pointerEncoding = "native";
        else if (!frozen)
            result.pointerEncoding = "unfrozen-native";
        else if (accepted > 1)
            result.pointerEncoding = "source-encoding-address-consensus";
        if (result.dataFrozen && result.count != 0)
        {
            // frozen 元素可能使用 FPlatformTypeLayoutParameters 重排，不能套用反射的 native stride
            result.status = "frozen-element-layout-unverified";
            return result;
        }
        std::vector<uint32_t> bits;
        if (!array && result.slots > 0)
        {
            bits.resize((static_cast<size_t>(result.slots) + 31) / 32);
            for (size_t start = 0; start < bits.size(); start += 256)
            {
                const size_t words = std::min<size_t>(256, bits.size() - start);
                const auto location = Add(selected->bits, start * sizeof(uint32_t));
                if (!location || !memory.IsReadable(*location, words * sizeof(uint32_t)) ||
                    !memory.ReadBytes(*location, bits.data() + start, words * sizeof(uint32_t)).Ok())
                {
                    result.status = "allocation-bits-unreadable";
                    return result;
                }
            }
            if (result.slots % 32)
                bits.back() &= (uint32_t{1} << (result.slots % 32)) - 1;
            int32_t allocated = 0;
            for (uint32_t value : bits)
                allocated += std::popcount(value);
            if (allocated != result.count || (freeCount > 0 &&
                                              (bits[static_cast<size_t>(firstFree) / 32] & (uint32_t{1} << (firstFree % 32))) != 0))
            {
                result.status = "allocation-bits-count-mismatch";
                return result;
            }
        }
        for (int32_t index = 0; index < result.slots; ++index)
        {
            if (!array && (bits[static_cast<size_t>(index) / 32] & (uint32_t{1} << (index % 32))) == 0)
                continue;
            if (result.elements.size() == limits.maxSamples)
                break;
            const auto location = Add(result.data, static_cast<uint64_t>(index) * static_cast<uint32_t>(stride));
            if (!location)
            {
                result.status = "element-address-overflow";
                return result;
            }
            ContainerElementSample sample{index, *location, std::vector<uint8_t>(std::min<size_t>(stride, limits.previewBytes))};
            if (!memory.IsReadable(*location, sample.bytes.size()) ||
                !memory.ReadBytes(*location, sample.bytes.data(), sample.bytes.size()).Ok())
            {
                result.status = "element-unreadable";
                return result;
            }
            result.elements.push_back(std::move(sample));
        }
        result.indicesComplete = result.elements.size() == static_cast<size_t>(result.count);
        result.elementsComplete = result.indicesComplete &&
                                  (result.count == 0 || static_cast<size_t>(stride) <= limits.previewBytes);
        result.status = result.count == 0 ? "empty-header-observed" : "allocated-elements-sampled";
        return result;
    }
} // namespace nextdumper::ue
