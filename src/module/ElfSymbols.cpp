#include "anduefker/module/ElfSymbols.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <elf.h>
#include <sys/mman.h>

#include <KittyScanner.hpp>

namespace anduefker::module
{
    namespace
    {
        constexpr size_t kMaxFileSize = 1024 * 1024 * 1024;
        constexpr size_t kMaxSymbols = 4 * 1024 * 1024;
        std::optional<uintptr_t> Add(uintptr_t base, uint64_t offset)
        {
            if (offset > UINTPTR_MAX || base > UINTPTR_MAX - static_cast<uintptr_t>(offset))
                return std::nullopt;
            return base + static_cast<uintptr_t>(offset);
        }
        struct MappedElf
        {
            KittyUtils::Zip::ZipEntryMMap mapping{};
            ~MappedElf()
            {
                if (mapping.mappingBase && mapping.mappingBase != MAP_FAILED && mapping.mappingSize &&
                    munmap(mapping.mappingBase, mapping.mappingSize) != 0)
                    KITTY_LOGE("ELF symbol mapping cleanup failed: %s", std::strerror(errno));
            }
            bool Contains(uint64_t offset, uint64_t size) const
            {
                return offset <= mapping.size && size <= mapping.size - offset;
            }
            template <typename T>
            bool Read(uint64_t offset, T &value) const
            {
                if (!Contains(offset, sizeof(T)))
                    return false;
                std::memcpy(&value, mapping.data + offset, sizeof(T));
                return true;
            }
        };

        // 返回 false 会将未解析的名称判定为未完成状态
        // 它不会擦除已从任意符号表中解析出的名称
        bool DebugSymbols(const app::ModuleImage &module, const ElfScanner &elf,
                          const std::vector<std::string> &names, SymbolQueryResult &result)
        {
            const auto fail = [&](const char *reason)
            { result.diagnostics.push_back(reason); return false; };
            MappedElf file;
            if (elf.isZipped())
            {
                if (!KittyUtils::Zip::mmapEntryByDataOffset(elf.filePath(), elf.baseSegment().offset, &file.mapping))
                    return fail("debug-file-unavailable");
            }
            else
            {
                KittyIOFile input(elf.filePath(), O_RDONLY);
                if (!input.open())
                    return fail("debug-file-unavailable");
                const auto size = input.info().st_size;
                if (size <= 0 || static_cast<uint64_t>(size) > kMaxFileSize)
                    return fail("debug-file-size-limit");
                file.mapping.mappingSize = static_cast<size_t>(size);
                file.mapping.mappingBase = mmap(nullptr, file.mapping.mappingSize, PROT_READ, MAP_PRIVATE, input.fd(), 0);
                file.mapping.data = static_cast<uint8_t *>(file.mapping.mappingBase);
                file.mapping.size = file.mapping.mappingSize;
            }
            if (!file.mapping.data || file.mapping.mappingBase == MAP_FAILED || file.mapping.size > kMaxFileSize)
                return fail("debug-file-mapping-failed-or-limited");
            KT_ElfW(Ehdr) header{};
            if (!file.Read(0, header) || std::memcmp(header.e_ident, "\177ELF", 4) != 0 ||
                header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_CLASS] != KT_ELF_EICLASS ||
                header.e_machine != elf.header().e_machine || header.e_shentsize != sizeof(KT_ElfW(Shdr)) ||
                !header.e_shnum || !file.Contains(header.e_shoff, uint64_t{header.e_shnum} * sizeof(KT_ElfW(Shdr))))
                return fail("debug-section-table-unavailable-or-invalid");
            size_t remaining = 0;
            for (const auto &name : names)
                if (!result.exported.contains(name))
                    ++remaining;
            size_t visited = 0;
            for (uint32_t sectionIndex = 0; sectionIndex < header.e_shnum; ++sectionIndex)
            {
                KT_ElfW(Shdr) section{};
                if (!file.Read(header.e_shoff + uint64_t{sectionIndex} * sizeof(section), section))
                    return fail("debug-section-unreadable");
                if (section.sh_type != SHT_SYMTAB)
                    continue;
                KT_ElfW(Shdr) strings{};
                if (section.sh_entsize != sizeof(KT_ElfW(Sym)) || section.sh_size % sizeof(KT_ElfW(Sym)) ||
                    section.sh_size / sizeof(KT_ElfW(Sym)) > kMaxSymbols || section.sh_link >= header.e_shnum ||
                    !file.Contains(section.sh_offset, section.sh_size) ||
                    !file.Read(header.e_shoff + uint64_t{section.sh_link} * sizeof(strings), strings) ||
                    strings.sh_type != SHT_STRTAB || !file.Contains(strings.sh_offset, strings.sh_size))
                    return fail("invalid-debug-symbol-table");
                for (uint64_t index = 0; index < section.sh_size / sizeof(KT_ElfW(Sym)); ++index)
                {
                    if (++visited > kMaxSymbols)
                        return fail("debug-symbol-limit");
                    KT_ElfW(Sym) symbol{};
                    if (!file.Read(section.sh_offset + index * sizeof(symbol), symbol))
                        return fail("debug-symbol-unreadable");
                    if (symbol.st_shndx == SHN_UNDEF ||
                        (KT_ELF_ST_TYPE(symbol.st_info) != STT_OBJECT && KT_ELF_ST_TYPE(symbol.st_info) != STT_FUNC))
                        continue;
                    if (symbol.st_name >= strings.sh_size)
                        return fail("debug-string-offset-out-of-range");
                    for (const auto &name : names)
                    {
                        if (result.exported.contains(name) || result.debug.contains(name))
                            continue;
                        const size_t length = name.size() + 1;
                        const size_t available = std::min<uint64_t>(length, strings.sh_size - symbol.st_name);
                        const auto *text = file.mapping.data + strings.sh_offset + symbol.st_name;
                        if (available != length)
                        {
                            if (std::memcmp(text, name.data(), available) == 0)
                                return fail("debug-string-not-terminated-in-table");
                            continue;
                        }
                        if (std::memcmp(text, name.c_str(), length) != 0)
                            continue;
                        const auto address = symbol.st_shndx == SHN_ABS ? std::optional<uintptr_t>(symbol.st_value) : Add(elf.loadBias(), symbol.st_value);
                        if (!address || !module.Contains(*address))
                            return fail("debug-symbol-address-out-of-range");
                        result.debug.emplace(name, *address);
                        result.status[name] = SymbolStatus::Debug;
                        if (--remaining == 0)
                            return true;
                    }
                }
            }
            return true;
        }
    }

    SymbolQueryResult QueryElfSymbols(const memory::IMemorySource &memory, const app::ModuleImage &module,
                                      const ElfScanner &elf, const std::vector<std::string> &requested)
    {
        if (!elf.isValid())
            return {{}, {}, {"invalid-elf"}, {}};
        DynamicSymbolTables tables{elf.loadBias(), elf.symbolTable(), elf.stringTable(), elf.stringTableSize(), elf.symbolEntrySize(), 0, 0};
        const auto relocate = [&](uintptr_t address)
        {
            if (module.Contains(address))
                return address;
            const auto value = Add(elf.loadBias(), address);
            return value && module.Contains(*value) ? *value : uintptr_t{0};
        };
        for (const auto &dynamic : elf.dynamics())
        {
            if (dynamic.d_tag == DT_GNU_HASH)
                tables.gnuHash = relocate(dynamic.d_un.d_ptr);
            if (dynamic.d_tag == DT_HASH)
                tables.sysvHash = relocate(dynamic.d_un.d_ptr);
        }
        auto result = QueryDynamicElfSymbols(memory, module, tables, requested);
        std::vector<std::string> names;
        for (const auto &[name, status] : result.status)
        {
            (void)status;
            names.push_back(name);
        }
        if (std::any_of(names.begin(), names.end(), [&](const auto &name)
                        { return !result.exported.contains(name); }) &&
            !DebugSymbols(module, elf, names, result))
            for (auto &[name, status] : result.status)
                if (!result.exported.contains(name) && !result.debug.contains(name))
                    status = SymbolStatus::Incomplete;
        return result;
    }
}
