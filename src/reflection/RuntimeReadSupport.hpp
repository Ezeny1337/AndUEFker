#pragma once

#include <algorithm>
#include <cstring>

#include "anduefker/memory/CaptureMemorySource.hpp"
#include "anduefker/ue/ContainerLayout.hpp"

namespace anduefker::reflection::detail
{
    inline std::optional<uintptr_t> Add(uintptr_t address, uint64_t offset)
    {
        if (!address || offset > UINTPTR_MAX || address > UINTPTR_MAX - static_cast<uintptr_t>(offset))
            return std::nullopt;
        return address + static_cast<uintptr_t>(offset);
    }

    inline std::optional<uint64_t> Word(const std::vector<uint8_t> &bytes, int32_t offset, int32_t width)
    {
        if (offset < 0 || width <= 0 || width > 8 || static_cast<size_t>(offset) > bytes.size() ||
            static_cast<size_t>(width) > bytes.size() - static_cast<size_t>(offset))
            return std::nullopt;
        uint64_t result = 0;
        std::memcpy(&result, bytes.data() + offset, static_cast<size_t>(width));
        return result;
    }

    inline int32_t Offset(const ir::StorageLayoutIR &layout, const char *name)
    {
        const auto field = std::find_if(layout.offsets.begin(), layout.offsets.end(),
                                        [name](const auto &value)
                                        { return value.first == name; });
        return field == layout.offsets.end() ? -1 : field->second;
    }

    inline ir::ValueCapture Capture(const memory::CaptureValidation &value)
    {
        ir::ValueCapture result;
        result.observationsStable = value.Stable();
        result.observedRanges = value.observedRanges;
        result.observedBytes = value.observedBytes;
        result.changedRanges = value.changedRanges;
        result.unreadableRanges = value.unreadableRanges;
        result.readFailures = value.readFailures;
        result.limitExceeded = value.limitExceeded;
        result.generationChanged = value.generationChanged;
        for (size_t index = 0; index < std::min<size_t>(value.readFailureSamples.size(), 8); ++index)
        {
            const auto &read = value.readFailureSamples[index];
            result.failures.push_back({read.address, static_cast<int32_t>(read.error), read.requested, read.transferred});
        }
        for (const auto &change : value.changes)
            result.changes.push_back({change.address, change.size, static_cast<int32_t>(change.read.error),
                                      change.read.transferred, change.before, change.after});
        return result;
    }

    inline std::optional<ir::ContainerStorageIR> HeapStorage(ir::PropertyKind kind, int32_t pointerWidth,
                                                             int32_t keySize = 0, int32_t valueSize = 0,
                                                             int32_t keyAlignment = 0, int32_t valueAlignment = 0)
    {
        const auto layout = ue::DescribeHeapContainer(kind, pointerWidth);
        if (!layout)
            return std::nullopt;
        auto result = kind == ir::PropertyKind::Array
                          ? std::optional<ir::ContainerStorageIR>(std::in_place)
                          : ue::DescribeSparseElement(kind, keySize, valueSize, keyAlignment, valueAlignment);
        if (!result)
            return std::nullopt;
        result->allocator = ir::ContainerAllocatorKind::Heap;
        result->allocatorStatus = "source-defined-native";
        result->headerStatus = "source-model";
        result->layoutId = layout->id;
        return result;
    }

} // namespace anduefker::reflection::detail
