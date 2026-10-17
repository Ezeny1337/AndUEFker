#include "nextdumper/module/ModuleImage.hpp"

namespace nextdumper::module
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
} // namespace nextdumper::module
