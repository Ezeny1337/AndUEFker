#pragma once

#include <string>
#include <vector>

#include "nextdumper/memory/MemorySource.hpp"
#include "nextdumper/module/ModuleImage.hpp"

namespace nextdumper::module
{
    using ::nextdumper::memory::IMemorySource;

    class ModuleCatalog
    {
    public:
        [[nodiscard]] static bool Discover(IMemorySource &memory,
                                           const std::vector<std::string> &names,
                                           ModuleImage &out);
    };
} // namespace nextdumper::module
