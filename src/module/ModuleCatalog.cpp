#include "anduefker/module/ModuleCatalog.hpp"

#include <algorithm>

#include "anduefker/memory/RemoteMemorySource.hpp"

namespace anduefker::app
{
    bool ModuleImage::IsExecutable(uintptr_t address, size_t size) const
    {
        if (size == 0 || address > UINTPTR_MAX - (size - 1))
            return false;
        const uintptr_t last = address + size - 1;
        for (const Segment &segment : segments)
        {
            if (segment.executable && address >= segment.start && last < segment.end)
                return true;
        }
        return false;
    }

    bool ModuleImage::IsWritable(uintptr_t address, size_t size) const
    {
        if (size == 0 || address > UINTPTR_MAX - (size - 1))
            return false;
        const uintptr_t last = address + size - 1;
        for (const Segment &segment : segments)
        {
            if (segment.writable && address >= segment.start && last < segment.end)
                return true;
        }
        return false;
    }
} // namespace anduefker::app

namespace anduefker::module
{
    bool ModuleCatalog::Discover(IMemorySource &memory,
                                 const std::vector<std::string> &names,
                                 ModuleImage &out)
    {
        auto *remote = dynamic_cast<RemoteMemorySource *>(&memory);
        if (remote == nullptr)
            return false;

        for (const std::string &name : names)
        {
            auto elf = remote->Manager().elfScanner.findElf(name);
            if (!elf.isValid())
                continue;

            out = {};
            out.name = name;
            out.base = elf.base();
            out.end = elf.end();
            for (const auto &segment : elf.segments())
            {
                out.segments.push_back(ModuleImage::Segment{
                    segment.startAddress,
                    segment.endAddress,
                    segment.offset,
                    segment.readable,
                    segment.writeable,
                    segment.executable,
                    segment.is_private,
                    segment.pathname,
                });
            }
            return out.IsValid();
        }
        return false;
    }

    uintptr_t ModuleCatalog::FindSymbol(IMemorySource &memory,
                                        const std::string &moduleName,
                                        const std::string &symbolName)
    {
        auto *remote = dynamic_cast<RemoteMemorySource *>(&memory);
        if (remote == nullptr || symbolName.empty())
            return 0;
        auto elf = remote->Manager().elfScanner.findElf(moduleName);
        if (!elf.isValid())
            return 0;
        uintptr_t symbol = elf.findSymbol(symbolName);
        if (symbol == 0)
            symbol = elf.findDebugSymbol(symbolName);
        return symbol;
    }
} // namespace anduefker::module
