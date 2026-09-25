#include "anduefker/binding/RuntimeBinding.hpp"

namespace anduefker::binding
{
    bool ObjectContainerLayout::IsValid() const
    {
        if (objectsOffset < 0 || numElementsOffset < 0 || itemObjectOffset < 0 || itemStride <= 0 || itemIndexOffset < 0)
            return false;
        if (kind == ObjectContainerKind::Chunked)
            return elementsPerChunk > 0;
        return true;
    }

    bool NameArrayLayout::IsValid() const
    {
        return chunksOffset >= 0 && elementsPerChunk > 0 && entryIndexOffset >= 0 && entryStringOffset >= 0;
    }

    bool NamePoolLayout::IsValid() const
    {
        return blocksOffset >= 0 && blocksBit >= 1 && blocksBit < 31 && entryStride > 0 &&
               entryHeaderOffset >= 0 && entryStringOffset >= 0 && entryLengthShift >= 0 && entryLengthShift < 16;
    }

    bool NameContainerLayout::IsValid() const
    {
        return kind == NameContainerKind::Pool ? pool.IsValid() : array.IsValid();
    }

    DecodePlan DecodePlan::Identity()
    {
        DecodePlan result;
        result.objectStorage = [](uintptr_t value, uintptr_t)
        { return value; };
        result.objectChunk = [](uintptr_t value, uintptr_t)
        { return value; };
        result.objectPointer = [](uintptr_t value, uintptr_t)
        { return value; };
        result.objectClass = [](uintptr_t value, uintptr_t)
        { return value; };
        result.objectOuter = [](uintptr_t value, uintptr_t)
        { return value; };
        result.nameBlocks = [](uintptr_t value, uintptr_t)
        { return value; };
        result.nameChunks = [](uintptr_t value, uintptr_t)
        { return value; };
        result.nameEntry = [](uintptr_t value, uintptr_t)
        { return value; };
        result.objectCount = [](int32_t value, uintptr_t)
        { return value; };
        result.objectFlags = [](int32_t value, uintptr_t)
        { return value; };
        result.objectIndex = [](int32_t value, uintptr_t)
        { return value; };
        result.nameIndex = [](int32_t value, uintptr_t)
        { return value; };
        result.nameEntryIndex = [](uint32_t value, uintptr_t)
        { return value; };
        result.nameHeader = [](uint16_t value, uintptr_t)
        { return value; };
        return result;
    }

    bool RuntimeBinding::IsValid() const
    {
        return objectRoot.address != 0 && nameRoot.address != 0 && objects.IsValid() && names.IsValid() &&
               report.objectContainerValidated && report.nameContainerValidated && report.semanticValidationPassed;
    }
} // namespace anduefker::binding
