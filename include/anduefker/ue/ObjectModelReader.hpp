#pragma once

#include <cstdint>
#include <optional>
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
        uintptr_t nextAddress = 0;
        std::string name;
        std::string className;
    };

    struct PropertyMetadata : FieldMetadata
    {
        int32_t arrayDim = 0;
        int32_t elementSize = 0;
        int32_t offset = 0;
        uint64_t flags = 0;
        uintptr_t referencedAddress = 0;
        uintptr_t secondaryAddress = 0;
    };

    struct EnumValueMetadata
    {
        std::string name;
        int64_t value = 0;
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
        [[nodiscard]] std::vector<FieldMetadata> Fields(uintptr_t first, size_t maxFields = 65536) const;
        [[nodiscard]] std::optional<PropertyMetadata> Property(uintptr_t field) const;
        [[nodiscard]] std::optional<uintptr_t> StructChildren(uintptr_t structure) const;
        [[nodiscard]] std::optional<uintptr_t> StructProperties(uintptr_t structure) const;
        [[nodiscard]] std::optional<uintptr_t> StructSuper(uintptr_t structure) const;
        [[nodiscard]] std::optional<int32_t> StructSize(uintptr_t structure) const;
        [[nodiscard]] std::vector<EnumValueMetadata> EnumValues(uintptr_t enumeration,
                                                                size_t maxValues = 65536) const;

    private:
        [[nodiscard]] std::optional<uintptr_t> ReadPointer(uintptr_t address) const;
        [[nodiscard]] std::optional<std::string> NameField(uintptr_t object) const;
        [[nodiscard]] bool IsReadableObject(uintptr_t object) const;

        const IMemorySource &memory_;
        const RuntimeBinding &binding_;
        const EngineSchema &schema_;
        ObjectStoreReader objects_;
        NameStoreReader names_;
        bool initialized_ = false;
    };
} // namespace anduefker::ue
