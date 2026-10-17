#pragma once

#include <functional>
#include <string>
#include <vector>

#include "nextdumper/module/ModuleImage.hpp"
#include "nextdumper/binding/BindingBuilder.hpp"
#include "nextdumper/binding/RuntimeBinding.hpp"

namespace nextdumper::memory
{
    class RemoteMemorySource;
}

namespace nextdumper::binding
{
    using ::nextdumper::memory::RemoteMemorySource;

    class GlobalLocator
    {
    public:
        GlobalLocator(RemoteMemorySource &memory, const module::ModuleImage &module) : memory_(memory), module_(module) {}

        [[nodiscard]] BindingCandidates LocateSymbols(const std::vector<std::string> &objectSymbols,
                                                      const std::vector<std::string> &nameSymbols,
                                                      const std::function<void(const std::string &)> &progress = {}) const;
        [[nodiscard]] BindingCandidates LocateAnalysis(const BindingCandidates &existing,
                                                       const std::function<void(const std::string &)> &progress = {}) const;

    private:
        [[nodiscard]] std::vector<LocatedAddress> SymbolCandidates(uintptr_t address,
                                                                   const std::string &symbol) const;

        RemoteMemorySource &memory_;
        const module::ModuleImage &module_;
    };
} // namespace nextdumper::binding
