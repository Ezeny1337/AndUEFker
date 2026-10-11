#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "anduefker/ir/ReflectionIR.hpp"

namespace anduefker::ir
{
    [[nodiscard]] inline bool IsValidPropertyBounds(const PropertyIR &property, int32_t bound, int64_t &end)
    {
        if (property.offset < 0 || property.elementSize <= 0 || property.arrayDim <= 0 || bound < 0)
            return false;
        const int64_t total = static_cast<int64_t>(property.elementSize) * property.arrayDim;
        end = static_cast<int64_t>(property.offset) + total;
        return total > 0 && end >= property.offset && end <= bound;
    }

    [[nodiscard]] inline bool IsValidPropertyBoolLayout(const PropertyIR &property)
    {
        return property.type.kind == PropertyKind::Bool && property.boolean.fieldSize == property.elementSize &&
               (property.boolean.fieldSize == 1 || property.boolean.fieldSize == 2 ||
                property.boolean.fieldSize == 4 || property.boolean.fieldSize == 8) &&
               property.boolean.byteOffset < property.boolean.fieldSize && property.boolean.byteMask != 0 &&
               (property.boolean.byteMask & (property.boolean.byteMask - 1)) == 0 &&
               (property.boolean.fieldMask == property.boolean.byteMask ||
                (property.boolean.fieldMask == 0xFF && property.boolean.byteMask == 1 && property.boolean.byteOffset == 0));
    }

    [[nodiscard]] inline uint64_t PropertyBoolMask(const PropertyIR &property)
    {
        return static_cast<uint64_t>(property.boolean.fieldMask) << (property.boolean.byteOffset * 8u);
    }

    // 在所有反射类型收集完成后运行，因此继承范围基于完整类型图，而绝非基于最后反射的字段
    void AnalyzeReflectionLayouts(ReflectionIR &reflection);

    struct TypeDeclarationIR
    {
        LayoutRepresentation representation = LayoutRepresentation::SequentialMembers;
        bool dependencyBlocked = false;
        bool inheritsBase = false;
        int32_t size = 0;
    };

    struct TypeDeclarationPlan
    {
        std::unordered_map<uintptr_t, TypeDeclarationIR> types;
        std::vector<size_t> order;
    };

    [[nodiscard]] TypeDeclarationPlan PlanTypeDeclarations(const ReflectionIR &reflection);
    [[nodiscard]] bool HasStorageRepresentationGap(const TypeReferenceIR &reference,
                                                   const TypeDeclarationPlan &plan, size_t depth = 0);
} // namespace anduefker::ir
