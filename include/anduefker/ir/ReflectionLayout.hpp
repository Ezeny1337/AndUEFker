#pragma once

#include <cstdint>

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

    // Runs after all reflected types have been collected so inherited ranges are
    // based on the complete type graph, never on the last reflected field.
    void AnalyzeReflectionLayouts(ReflectionIR &reflection);
} // namespace anduefker::ir
