#pragma once

#include <string>
#include <vector>

#include "anduefker/memory/MemorySource.hpp"
#include "anduefker/module/ModuleImage.hpp"

namespace anduefker::module
{
    using ::anduefker::memory::IMemorySource;

    class ModuleCatalog
    {
    public:
        [[nodiscard]] static bool Discover(IMemorySource &memory,
                                           const std::vector<std::string> &names,
                                           ModuleImage &out);
    };
} // namespace anduefker::module
