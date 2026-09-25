#pragma once

#include <cstdint>
#include <optional>

#include "anduefker/ue/EngineSchema.hpp"
#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/binding/RuntimeBinding.hpp"

namespace anduefker::ue
{
    using ::anduefker::binding::DecodePlan;
    using ::anduefker::binding::ObjectContainerKind;
    using ::anduefker::binding::ObjectContainerLayout;
    using ::anduefker::memory::IMemorySource;

    class ObjectStoreReader
    {
    public:
        ObjectStoreReader(const IMemorySource &memory,
                          uintptr_t root,
                          const ObjectContainerLayout &layout,
                          const DecodePlan &decode);

        [[nodiscard]] bool Initialize();
        [[nodiscard]] bool IsInitialized() const { return initialized_; }
        [[nodiscard]] int32_t Count() const { return count_; }
        [[nodiscard]] std::optional<uintptr_t> ObjectAt(int32_t index) const;

    private:
        [[nodiscard]] std::optional<uintptr_t> ReadItemAddress(int32_t index) const;
        [[nodiscard]] std::optional<uintptr_t> ReadPointer(uintptr_t address) const;

        const IMemorySource &memory_;
        uintptr_t root_ = 0;
        ObjectContainerLayout layout_;
        const DecodePlan &decode_;
        uintptr_t storage_ = 0;
        int32_t count_ = 0;
        bool initialized_ = false;
    };
} // namespace anduefker::ue
