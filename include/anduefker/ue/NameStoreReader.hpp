#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "anduefker/ue/EngineSchema.hpp"
#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/binding/RuntimeBinding.hpp"

namespace anduefker::ue
{
    using ::anduefker::binding::DecodePlan;
    using ::anduefker::binding::NameArrayLayout;
    using ::anduefker::binding::NameContainerKind;
    using ::anduefker::binding::NameContainerLayout;
    using ::anduefker::binding::NamePoolLayout;
    using ::anduefker::binding::RuntimeBinding;
    using ::anduefker::memory::IMemorySource;

    class NameStoreReader
    {
    public:
        NameStoreReader(const IMemorySource &memory,
                        uintptr_t root,
                        const NameContainerLayout &layout,
                        const DecodePlan &decode,
                        const FNameSchema &fname,
                        const EngineFeatures &features);

        [[nodiscard]] std::optional<uintptr_t> EntryAt(int32_t index) const;
        [[nodiscard]] std::optional<std::string> ReadName(int32_t index) const;
        [[nodiscard]] std::optional<std::string> ReadFName(uintptr_t fnameAddress) const;

    private:
        [[nodiscard]] std::optional<std::string> ReadEntry(uintptr_t entry) const;
        [[nodiscard]] std::optional<std::string> ReadBytesAsUtf8(uintptr_t address, size_t length) const;
        [[nodiscard]] std::optional<std::string> ReadUtf16AsUtf8(uintptr_t address, size_t length) const;
        [[nodiscard]] std::optional<uintptr_t> ReadPointer(uintptr_t address) const;

        const IMemorySource &memory_;
        uintptr_t root_ = 0;
        NameContainerLayout layout_;
        const DecodePlan &decode_;
        FNameSchema fname_;
        EngineFeatures features_;
    };
} // namespace anduefker::ue
