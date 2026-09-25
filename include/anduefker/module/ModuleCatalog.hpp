#pragma once

#include <string>
#include <vector>

#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/app/RuntimeContext.hpp"

namespace anduefker::module
{
    using ::anduefker::app::ModuleImage;
    using ::anduefker::memory::IMemorySource;

    class ModuleCatalog
    {
    public:
        [[nodiscard]] static bool Discover(IMemorySource &memory,
                                           const std::vector<std::string> &names,
                                           ModuleImage &out);
        [[nodiscard]] static uintptr_t FindSymbol(IMemorySource &memory,
                                                  const std::string &moduleName,
                                                  const std::string &symbolName);
    };
} // namespace anduefker::module
