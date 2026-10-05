#pragma once

#include "anduefker/ue/SchemaResolver.hpp"

namespace anduefker::ue::schema_probe
{
    inline std::optional<uintptr_t> Add(uintptr_t base, int32_t offset)
    {
        if (offset < 0 || base > UINTPTR_MAX - static_cast<uintptr_t>(offset))
            return std::nullopt;
        return base + static_cast<uintptr_t>(offset);
    }

    inline bool ReadPointer(const IMemorySource &memory, uintptr_t address, uintptr_t &value)
    {
        return memory.Read(address, value);
    }

    inline bool IsReadablePointer(const IMemorySource &memory, uintptr_t address)
    {
        return address != 0 && memory.IsReadable(address, sizeof(uintptr_t));
    }

    inline std::optional<std::string> ReadObjectName(const IMemorySource &memory,
                                                     const RuntimeBinding &binding,
                                                     const EngineSchema &schema,
                                                     const NameStoreReader &names,
                                                     uintptr_t object)
    {
        const auto address = Add(object, schema.uobject.name);
        if (!address)
            return std::nullopt;
        int32_t rawName = 0;
        if (!memory.Read(*address, rawName))
            return std::nullopt;
        rawName = binding.decode.nameIndex(rawName, *address);
        return names.ReadName(rawName);
    }

    inline std::optional<std::string> ReadFieldName(const IMemorySource &memory,
                                                    const RuntimeBinding &binding,
                                                    const NameStoreReader &names,
                                                    uintptr_t field,
                                                    int32_t nameOffset)
    {
        const auto address = Add(field, nameOffset);
        if (!address)
            return std::nullopt;
        int32_t rawName = 0;
        if (!memory.Read(*address, rawName))
            return std::nullopt;
        rawName = binding.decode.nameIndex(rawName, *address);
        return names.ReadName(rawName);
    }
} // namespace anduefker::ue::schema_probe
