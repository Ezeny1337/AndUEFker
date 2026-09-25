#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/binding/RuntimeBinding.hpp"

namespace anduefker::binding
{
    using ::anduefker::memory::IMemorySource;

    struct LayoutProbeReport
    {
        bool accepted = false;
        double confidence = 0.0;
        int32_t tested = 0;
        int32_t valid = 0;
        std::vector<std::string> evidence;
        std::vector<std::string> failures;
    };

    class ObjectLayoutProbe
    {
    public:
        explicit ObjectLayoutProbe(const IMemorySource &memory) : memory_(memory) {}

        [[nodiscard]] LayoutProbeReport Validate(uintptr_t root,
                                                 const ObjectContainerLayout &layout,
                                                 const DecodePlan &decode,
                                                 int32_t maxSamples = 200) const;

    private:
        [[nodiscard]] bool IsLikelyObject(uintptr_t address) const;
        [[nodiscard]] bool ReadItemAddress(uintptr_t root,
                                           const ObjectContainerLayout &layout,
                                           const DecodePlan &decode,
                                           int32_t index,
                                           uintptr_t &item) const;

        const IMemorySource &memory_;
    };

    class NameLayoutProbe
    {
    public:
        explicit NameLayoutProbe(const IMemorySource &memory) : memory_(memory) {}

        [[nodiscard]] LayoutProbeReport Validate(uintptr_t root,
                                                 const NameContainerLayout &layout,
                                                 const DecodePlan &decode) const;

    private:
        [[nodiscard]] bool ReadString(uintptr_t address, int32_t length, bool wide, std::string &out) const;
        [[nodiscard]] bool ReadEntry(uintptr_t entry,
                                     const NameContainerLayout &layout,
                                     const DecodePlan &decode,
                                     std::string &out) const;

        const IMemorySource &memory_;
    };
} // namespace anduefker::binding
