#pragma once

#include <string>
#include <vector>

#include "anduefker/analyzer/AnalyzerMemoryAdapter.hpp"
#include "anduefker/binding/BindingBuilder.hpp"
#include "anduefker/binding/RuntimeBinding.hpp"

namespace anduefker::binding
{
    using ::anduefker::app::ModuleImage;
    using ::anduefker::memory::RemoteMemorySource;

    class GlobalLocator
    {
    public:
        GlobalLocator(RemoteMemorySource &memory, const ModuleImage &module) : memory_(memory), module_(module) {}

        [[nodiscard]] BindingCandidates Locate(const std::vector<std::string> &objectSymbols,
                                               const std::vector<std::string> &nameSymbols) const;

    private:
        [[nodiscard]] LocatedAddress SymbolCandidate(const std::string &symbol) const;

        RemoteMemorySource &memory_;
        const ModuleImage &module_;
    };
} // namespace anduefker::binding
