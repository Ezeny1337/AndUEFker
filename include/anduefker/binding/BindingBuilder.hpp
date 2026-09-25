#pragma once

#include <optional>
#include <vector>

#include "anduefker/binding/LayoutDiscovery.hpp"
#include "anduefker/binding/LayoutProbes.hpp"

namespace anduefker::binding
{
    using ::anduefker::memory::IMemorySource;

    struct BindingCandidates
    {
        std::vector<LocatedAddress> objectRoots;
        std::vector<LocatedAddress> nameRoots;
    };

    class BindingBuilder
    {
    public:
        explicit BindingBuilder(const IMemorySource &memory) : memory_(memory) {}

        [[nodiscard]] std::optional<RuntimeBinding> Build(const BindingCandidates &candidates,
                                                          const DecodePlan &decode = DecodePlan::Identity()) const;

    private:
        [[nodiscard]] std::vector<uintptr_t> ResolveRoots(const std::vector<LocatedAddress> &candidates) const;

        const IMemorySource &memory_;
    };
} // namespace anduefker::binding
