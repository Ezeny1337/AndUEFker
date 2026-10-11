#pragma once

#include <functional>
#include <string>
#include <vector>

#include "anduefker/module/ModuleImage.hpp"
#include "anduefker/binding/BindingBuilder.hpp"
#include "anduefker/binding/RuntimeBinding.hpp"

namespace anduefker::memory
{
    class RemoteMemorySource;
}

namespace anduefker::binding
{
    using ::anduefker::memory::RemoteMemorySource;

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
} // namespace anduefker::binding
