#pragma once

#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/ue/ContainerLayout.hpp"

namespace anduefker::ue
{
    using ir::ContainerElementSample;
    using ir::ContainerValue;

    struct ContainerReadLimits
    {
        size_t maxSamples = 8;
        size_t previewBytes = 64;
    };

    // 只读、有限预算的遍历器，不调用目标代码，不重建 hash，不解析元素中的指针
    [[nodiscard]] ContainerValue ReadContainerValue(const memory::IMemorySource &memory, uintptr_t address,
                                                    ir::PropertyKind kind, const ir::ContainerStorageIR &storage,
                                                    int32_t pointerWidth, int32_t arrayStride = 0,
                                                    ContainerReadLimits limits = {});
} // namespace anduefker::ue
