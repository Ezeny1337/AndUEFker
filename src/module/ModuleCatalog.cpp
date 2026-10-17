#include "nextdumper/module/ModuleCatalog.hpp"

#include <elf.h>

#include "nextdumper/memory/RemoteMemorySource.hpp"

namespace nextdumper::module
{
    using ::nextdumper::memory::RemoteMemorySource;

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

} // namespace nextdumper::module
