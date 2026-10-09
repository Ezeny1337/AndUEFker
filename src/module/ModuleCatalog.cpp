#include "anduefker/module/ModuleCatalog.hpp"

#include <algorithm>
#include <elf.h>

#include "anduefker/memory/RemoteMemorySource.hpp"
#include "anduefker/module/ElfSymbols.hpp"

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
    using ::anduefker::memory::RemoteMemorySource;

    namespace
    {
        ModuleArchitecture DetectArchitecture(const auto &header, uint8_t &pointerWidth)
        {
            pointerWidth = header.e_ident[EI_CLASS] == ELFCLASS32 ? 4 : header.e_ident[EI_CLASS] == ELFCLASS64 ? 8
                                                                                                               : 0;
            if (header.e_ident[EI_CLASS] == ELFCLASS32 && header.e_machine == EM_ARM)
                return ModuleArchitecture::Arm32;
            if (header.e_ident[EI_CLASS] == ELFCLASS64 && header.e_machine == EM_AARCH64)
                return ModuleArchitecture::Arm64;
            return ModuleArchitecture::Unknown;
        }
    } // namespace

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
            out.architecture = DetectArchitecture(elf.header(), out.pointerWidth);
            if (out.architecture == ModuleArchitecture::Unknown || out.pointerWidth != sizeof(uintptr_t))
                continue;
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
        ModuleImage module;
        if (!Discover(memory, {moduleName}, module))
            return 0;
        const auto result = QueryElfSymbols(memory, module, elf, {symbolName});
        if (const auto found = result.exported.find(symbolName); found != result.exported.end())
            return found->second;
        const auto found = result.debug.find(symbolName);
        return found == result.debug.end() ? 0 : found->second;
    }
} // namespace anduefker::module
