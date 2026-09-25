#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace anduefker::binding
{
    enum class AddressMeaning
    {
        Direct,
        PointerSlot,
        ResolvedValue,
    };

    struct LocatedAddress
    {
        uintptr_t address = 0;
        AddressMeaning meaning = AddressMeaning::Direct;
        uint8_t confidence = 0;
        std::string source;
    };

    enum class ObjectContainerKind
    {
        Fixed,
        Chunked,
    };

    struct ObjectContainerLayout
    {
        ObjectContainerKind kind = ObjectContainerKind::Chunked;
        int32_t objectsOffset = -1;
        int32_t numElementsOffset = -1;
        int32_t maxElementsOffset = -1;
        int32_t numChunksOffset = -1;
        int32_t maxChunksOffset = -1;
        int32_t elementsPerChunk = -1;
        int32_t itemObjectOffset = -1;
        int32_t itemStride = -1;
        int32_t itemIndexOffset = -1;

        [[nodiscard]] bool IsValid() const;
    };

    enum class NameContainerKind
    {
        Array,
        Pool,
    };

    struct NameArrayLayout
    {
        int32_t chunksOffset = -1;
        int32_t numElementsOffset = -1;
        int32_t elementsPerChunk = -1;
        int32_t entryIndexOffset = -1;
        int32_t entryStringOffset = -1;

        [[nodiscard]] bool IsValid() const;
    };

    struct NamePoolLayout
    {
        int32_t blocksOffset = -1;
        int32_t maxChunkIndexOffset = -1;
        int32_t byteCursorOffset = -1;
        int32_t blocksBit = -1;
        int32_t entryStride = -1;
        int32_t entryHeaderOffset = -1;
        int32_t entryStringOffset = -1;
        int32_t entryLengthShift = -1;
        uint16_t entryWideMask = 0;

        [[nodiscard]] bool IsValid() const;
    };

    struct NameContainerLayout
    {
        NameContainerKind kind = NameContainerKind::Pool;
        NameArrayLayout array;
        NamePoolLayout pool;

        [[nodiscard]] bool IsValid() const;
    };

    struct DecodePlan
    {
        using PointerDecoder = std::function<uintptr_t(uintptr_t value, uintptr_t fieldAddress)>;
        using Int32Decoder = std::function<int32_t(int32_t value, uintptr_t fieldAddress)>;
        using UInt32Decoder = std::function<uint32_t(uint32_t value, uintptr_t fieldAddress)>;
        using UInt16Decoder = std::function<uint16_t(uint16_t value, uintptr_t fieldAddress)>;

        PointerDecoder objectStorage;
        PointerDecoder objectChunk;
        PointerDecoder objectPointer;
        PointerDecoder objectClass;
        PointerDecoder objectOuter;
        PointerDecoder nameBlocks;
        PointerDecoder nameChunks;
        PointerDecoder nameEntry;
        Int32Decoder objectCount;
        Int32Decoder objectFlags;
        Int32Decoder objectIndex;
        Int32Decoder nameIndex;
        UInt32Decoder nameEntryIndex;
        UInt16Decoder nameHeader;

        static DecodePlan Identity();
    };

    struct BindingReport
    {
        bool staticCandidatesFound = false;
        bool objectContainerValidated = false;
        bool nameContainerValidated = false;
        bool semanticValidationPassed = false;
        std::vector<std::string> evidence;
        std::vector<std::string> failures;
    };

    struct RuntimeBinding
    {
        LocatedAddress objectRoot;
        LocatedAddress nameRoot;
        ObjectContainerLayout objects;
        NameContainerLayout names;
        DecodePlan decode = DecodePlan::Identity();
        BindingReport report;

        [[nodiscard]] bool IsValid() const;
    };
} // namespace anduefker::binding
