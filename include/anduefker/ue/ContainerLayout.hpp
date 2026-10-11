#pragma once

#include <optional>
#include <utility>

#include "anduefker/ir/ReflectionIR.hpp"

namespace anduefker::ue
{
    struct ContainerLayoutDescription
    {
        std::string id;
        int32_t size = 0;
        int32_t alignment = 0;
        std::vector<std::pair<std::string, int32_t>> offsets;
    };

    // 以源码为依据的普通脚本容器，而非宿主 C++ 对象布局
    [[nodiscard]] std::optional<ContainerLayoutDescription> DescribeHeapContainer(ir::PropertyKind kind,
                                                                                  int32_t pointerWidth);
    void DecodeSparseLayout(ir::ContainerStorageObservation &observation, ir::PropertyKind kind);
    [[nodiscard]] std::optional<int32_t> PropertyStorageAlignment(const ir::TypeReferenceIR &reference,
                                                                  const std::unordered_map<uintptr_t, const ir::TypeIR *> &types,
                                                                  int32_t pointerWidth, int32_t nameSize, size_t depth = 0);
    void ValidateSparseLayout(ir::ContainerStorageIR &storage, ir::PropertyKind kind,
                              int32_t keySize, int32_t valueSize, int32_t keyAlignment, int32_t valueAlignment);
} // namespace anduefker::ue
