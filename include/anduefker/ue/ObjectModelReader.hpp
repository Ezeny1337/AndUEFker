#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include "anduefker/ue/EngineSchema.hpp"
#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/ue/NameStoreReader.hpp"
#include "anduefker/ue/ObjectStoreReader.hpp"

namespace anduefker::ue
{
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::memory::IMemorySource;

    inline constexpr uint32_t kRFClassDefaultObject = 0x00000010u;
    inline constexpr uint32_t kRFIncompleteLoad =
        0x00000200u | // RF_NeedInitialization
        0x00000400u | // RF_NeedLoad
        0x00001000u | // RF_NeedPostLoad
        0x00002000u;  // RF_NeedPostLoadSubobjects
    inline constexpr uint32_t kRFUnavailableDefinition = kRFIncompleteLoad | 0x00004000u | 0x00008000u | 0x00010000u;

    struct ObjectMetadata
    {
        uintptr_t address = 0;
        uintptr_t classAddress = 0;
        uintptr_t outerAddress = 0;
        int32_t internalIndex = -1;
        uint32_t flags = 0;
        std::string name;
        std::string className;
        std::string fullName;
    };

    struct FieldMetadata
    {
        uintptr_t address = 0;
        uintptr_t classAddress = 0;
        uintptr_t ownerAddress = 0;
        bool ownerIsUObject = false;
        uintptr_t nextAddress = 0;
        std::string name;
        std::string className;
        std::string normalizedClassName;
        FieldKind kind = FieldKind::Unknown;
    };

    enum class FieldChainStatus
    {
        Empty,
        Complete,
        Unreadable,
        CycleDetected,
        LimitExceeded,
    };

    struct FieldChainResult
    {
        std::vector<FieldMetadata> fields;
        FieldChainStatus status = FieldChainStatus::Empty;

        [[nodiscard]] bool Complete() const
        {
            return status == FieldChainStatus::Empty || status == FieldChainStatus::Complete;
        }
    };

    struct PropertyMetadata : FieldMetadata
    {
        int32_t arrayDim = 0;
        int32_t elementSize = 0;
        int32_t offset = 0;
        uint64_t flags = 0;
        uintptr_t referencedAddress = 0;
        uintptr_t secondaryAddress = 0;
        enum class DetailsStatus
        {
            Complete,
            UnsupportedLayout,
            Unreadable,
            InvalidReference
        };
        DetailsStatus detailsStatus = DetailsStatus::Complete;
        std::array<uint8_t, 4> boolLayout{};
    };

    struct EnumValueMetadata
    {
        std::string name;
        int64_t value = 0;
    };

    enum class DefinitionKind
    {
        Other,
        Class,
        Struct,
        Enum
    };
    enum class EnumReadStatus
    {
        Complete,
        Unreadable,
        InvalidHeader,
        LimitExceeded
    };

    struct EnumReadResult
    {
        EnumReadStatus status = EnumReadStatus::Unreadable;
        int32_t expectedCount = -1;
        std::vector<EnumValueMetadata> values;

        [[nodiscard]] bool Complete() const { return status == EnumReadStatus::Complete; }
    };

    class ObjectModelReader
    {
    public:
        ObjectModelReader(const IMemorySource &memory,
                          const RuntimeBinding &binding,
                          const EngineSchema &schema);

        [[nodiscard]] bool Initialize();
        [[nodiscard]] int32_t Count() const { return objects_.Count(); }
        [[nodiscard]] std::optional<uintptr_t> ObjectAt(int32_t index) const { return objects_.ObjectAt(index); }
        [[nodiscard]] const ObjectStoreReader &Objects() const { return objects_; }
        [[nodiscard]] const NameStoreReader &Names() const { return names_; }

        [[nodiscard]] std::optional<uintptr_t> Class(uintptr_t object) const;
        [[nodiscard]] std::optional<uintptr_t> Outer(uintptr_t object) const;
        [[nodiscard]] std::optional<int32_t> InternalIndex(uintptr_t object) const;
        [[nodiscard]] std::optional<uint32_t> Flags(uintptr_t object) const;
        [[nodiscard]] std::optional<std::string> Name(uintptr_t object) const;
        [[nodiscard]] std::optional<std::string> ClassName(uintptr_t object) const;
        [[nodiscard]] std::optional<std::string> FullName(uintptr_t object, size_t maxDepth = 64) const;
        [[nodiscard]] std::optional<ObjectMetadata> Metadata(uintptr_t object) const;

        [[nodiscard]] std::optional<FieldMetadata> Field(uintptr_t field) const;
        [[nodiscard]] std::optional<FieldMetadata> UField(uintptr_t field) const;
        [[nodiscard]] FieldChainResult FieldsWithStatus(uintptr_t first, size_t maxFields = 65536) const;
        [[nodiscard]] FieldChainResult UFieldsWithStatus(uintptr_t first, size_t maxFields = 65536) const;
        [[nodiscard]] std::vector<FieldMetadata> Fields(uintptr_t first, size_t maxFields = 65536) const;
        [[nodiscard]] std::optional<PropertyMetadata> Property(uintptr_t field) const;
        [[nodiscard]] std::optional<DefinitionKind> DefinitionKindForClass(uintptr_t classAddress) const;
        [[nodiscard]] std::optional<uintptr_t> StructChildren(uintptr_t structure) const;
        [[nodiscard]] std::optional<uintptr_t> StructProperties(uintptr_t structure) const;
        [[nodiscard]] std::optional<uintptr_t> StructSuper(uintptr_t structure) const;
        [[nodiscard]] std::optional<int32_t> StructSize(uintptr_t structure) const;
        [[nodiscard]] std::vector<EnumValueMetadata> EnumValues(uintptr_t enumeration,
                                                                size_t maxValues = 65536) const;
        [[nodiscard]] EnumReadResult ReadEnumValues(uintptr_t enumeration, size_t maxValues = 65536) const;

    private:
        [[nodiscard]] std::optional<uintptr_t> ReadPointer(uintptr_t address) const;
        [[nodiscard]] std::optional<std::pair<uintptr_t, bool>> DecodeFieldOwner(uintptr_t field) const;
        [[nodiscard]] FieldKind ResolveFFieldKind(uintptr_t classAddress, FieldKind fallback) const;
        [[nodiscard]] FieldKind ResolveUFieldKind(uintptr_t classAddress, FieldKind fallback) const;
        [[nodiscard]] std::optional<std::string> NameField(uintptr_t object) const;
        [[nodiscard]] bool IsReadableObject(uintptr_t object) const;
        [[nodiscard]] FieldChainResult ReadFieldChain(uintptr_t first, size_t maxFields, bool ufield) const;

        const IMemorySource &memory_;
        const RuntimeBinding &binding_;
        const EngineSchema &schema_;
        ObjectStoreReader objects_;
        NameStoreReader names_;
        bool initialized_ = false;
    };
} // namespace anduefker::ue
