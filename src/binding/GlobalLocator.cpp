#include "anduefker/binding/GlobalLocator.hpp"

#include <algorithm>
#include <memory>

#include "Architecture/IArchDecoder.h"
#include "UEAnalyzer/UEAnalyzer.h"

namespace anduefker::binding
{
    using ::anduefker::analyzer::AnalyzerMemoryAdapter;

    namespace
    {
        EArch ToAnalyzerArchitecture(ModuleArchitecture architecture)
        {
            switch (architecture)
            {
            case ModuleArchitecture::Arm32:
                return EArch::Arm32;
            case ModuleArchitecture::Arm64:
                return EArch::Arm64;
            case ModuleArchitecture::Unknown:
                break;
            }
            return EArch::Unknown;
        }
    } // namespace

    std::vector<LocatedAddress> GlobalLocator::SymbolCandidates(ElfScanner &elf,
                                                                const std::string &symbol) const
    {
        std::vector<LocatedAddress> result;
        uintptr_t address = elf.findSymbol(symbol);
        if (address == 0)
            address = elf.findDebugSymbol(symbol);
        if (address == 0)
            return result;

        if (symbol == "GUObjectArray" || symbol == "NamePoolData")
        {
            result.push_back({address, AddressMeaning::Direct, 80, "symbol:" + symbol});
            return result;
        }

        // 调试器辅助符号和旧别名在引擎分支和游戏版本之间有所不同
        // 保留两种解释，并让经过验证的对象/名称布局探测选择可用的根
        result.push_back({address, AddressMeaning::Direct, 70, "symbol:" + symbol + ":direct"});
        result.push_back({address, AddressMeaning::PointerSlot, 70, "symbol:" + symbol + ":pointer-slot"});
        return result;
    }

    BindingCandidates GlobalLocator::Locate(const std::vector<std::string> &objectSymbols,
                                            const std::vector<std::string> &nameSymbols,
                                            const std::function<void(const std::string &)> &progress) const
    {
        BindingCandidates result;
        if (progress)
            progress("locator: resolving exported and debug symbols");
        auto elf = memory_.Manager().elfScanner.findElf(module_.name);
        if (elf.isValid())
        {
            if (progress)
                progress("locator: resolving module symbols from one ELF scan");
            for (const std::string &symbol : objectSymbols)
            {
                const std::vector<LocatedAddress> candidates = SymbolCandidates(elf, symbol);
                result.objectRoots.insert(result.objectRoots.end(), candidates.begin(), candidates.end());
            }
            for (const std::string &symbol : nameSymbols)
            {
                const std::vector<LocatedAddress> candidates = SymbolCandidates(elf, symbol);
                result.nameRoots.insert(result.nameRoots.end(), candidates.begin(), candidates.end());
            }
            if (progress)
                progress("locator: module symbol resolution complete");
        }

        AnalyzerMemoryAdapter adapter(memory_, module_);
        if (!adapter.Initialize())
            return result;
        const std::unique_ptr<IArchDecoder> decoder = CreateArchDecoder(ToAnalyzerArchitecture(module_.architecture));
        if (!decoder)
            return result;

        anduefker::analyzer::AnalyzerOptions options;
        options.ThreadMode = anduefker::analyzer::EThreadMode::Single;
        options.Progress = progress;
        options.Targets = {anduefker::analyzer::Targets::Names,
                           anduefker::analyzer::Targets::GUObjectArray,
                           anduefker::analyzer::Targets::ObjObjects};
        anduefker::analyzer::UEAnalyzer analyzer = anduefker::analyzer::UEAnalyzer::Analyze(&adapter, decoder.get(), options);
        if (!analyzer.IsValid())
            return result;

        auto add = [](std::vector<LocatedAddress> &out, const anduefker::analyzer::LocateResult &located,
                      const char *label)
        {
            for (const anduefker::analyzer::Candidate &candidate : located.Candidates)
            {
                if (candidate.Address == 0)
                    continue;
                if (std::any_of(out.begin(), out.end(), [&](const LocatedAddress &item)
                                { return item.address == static_cast<uintptr_t>(candidate.Address); }))
                    continue;
                const float confidence = std::clamp(candidate.Confidence, 0.0f, 1.0f);
                out.push_back({static_cast<uintptr_t>(candidate.Address), AddressMeaning::Direct,
                               static_cast<uint8_t>(confidence * 100.0f), std::string("analyzer:") + label});
            }
        };

        // 符号可以是直接对象、指针槽、调试器助手，也可以是过时/部分导出
        // 绝不能仅因其存在而忽略二进制分析的候选对象
        if (progress)
            progress("locator: resolving GUObjectArray anchors");
        add(result.objectRoots, analyzer.Find(anduefker::analyzer::Targets::GUObjectArray), "GUObjectArray");
        if (progress)
            progress("locator: resolving ObjObjects anchors");
        add(result.objectRoots, analyzer.Find(anduefker::analyzer::Targets::ObjObjects), "ObjObjects");
        if (progress)
            progress("locator: resolving name anchors");
        add(result.nameRoots, analyzer.Find(anduefker::analyzer::Targets::Names), "Names");
        return result;
    }
} // namespace anduefker::binding
