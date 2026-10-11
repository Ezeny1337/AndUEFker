#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "anduefker/ir/ReflectionIR.hpp"

namespace anduefker::ue
{
    using StorageLayoutDescription = ir::StorageLayoutIR;

    // 枚举源码结构族，版本标签只用于说明依据，不作为运行时选择条件
    [[nodiscard]] std::vector<StorageLayoutDescription> ValueStorageCandidates(
        const ir::TypeReferenceIR &reference, int32_t pointerWidth, int32_t nameSize);
    [[nodiscard]] std::optional<StorageLayoutDescription> CommonValueStorage(
        const ir::TypeReferenceIR &reference, int32_t pointerWidth, int32_t nameSize);

    enum class MemoryImagePointerEncoding
    {
        RelativeShift1,
        PackedOffset40,
    };

    struct MemoryImagePointer
    {
        uint64_t address = 0;
        int32_t typeIndex = -1;
        bool frozen = false;
    };

    // 使用显式符号扩展与检查过的地址运算，不依赖宿主的 signed shift
    [[nodiscard]] std::optional<MemoryImagePointer> DecodeMemoryImagePointer(
        uint64_t raw, uint64_t fieldAddress, int32_t pointerWidth, MemoryImagePointerEncoding encoding);
} // namespace anduefker::ue
