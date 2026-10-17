#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "nextdumper/ue/EngineSchema.hpp"
#include "nextdumper/memory/MemorySource.hpp"
#include "nextdumper/binding/RuntimeBinding.hpp"

namespace nextdumper::ue
{
    using ::nextdumper::binding::DecodePlan;
    using ::nextdumper::binding::NameArrayLayout;
    using ::nextdumper::binding::NameContainerKind;
    using ::nextdumper::binding::NameContainerLayout;
    using ::nextdumper::binding::NamePoolLayout;
    using ::nextdumper::binding::RuntimeBinding;
    using ::nextdumper::memory::IMemorySource;

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
        [[nodiscard]] std::optional<std::string> ReadComparisonName(uintptr_t fnameAddress) const;

    private:
        struct EntryLocation
        {
            uintptr_t address = 0;
            size_t available = 0;
        };
        [[nodiscard]] std::optional<EntryLocation> LocateEntry(int32_t index) const;
        [[nodiscard]] std::optional<std::string> ReadEntry(uintptr_t entry, size_t depth, size_t available) const;
        [[nodiscard]] std::optional<std::string> ReadBytesAsUtf8(uintptr_t address, size_t length) const;
        [[nodiscard]] std::optional<std::string> ReadUtf16AsUtf8(uintptr_t address, size_t length) const;
        [[nodiscard]] std::optional<uintptr_t> ReadPointer(uintptr_t address) const;

        const IMemorySource &memory_;
        uintptr_t root_ = 0;
        NameContainerLayout layout_;
        const DecodePlan &decode_;
        FNameSchema fname_;
        EngineFeatures features_;
        mutable uint64_t boundaryGeneration_ = 0;
        mutable std::optional<uint32_t> currentBlock_;
        mutable uint32_t byteCursor_ = 0;
    };
} // namespace nextdumper::ue
