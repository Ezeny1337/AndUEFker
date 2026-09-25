#include "anduefker/binding/GlobalLocator.hpp"

#include <algorithm>
#include <memory>

#include "Architecture/IArchDecoder.h"
#include "UEAnalyzer/UEAnalyzer.h"

namespace anduefker::binding
{
    using ::anduefker::analyzer::AnalyzerMemoryAdapter;

    LocatedAddress GlobalLocator::SymbolCandidate(const std::string &symbol) const
    {
        auto elf = memory_.Manager().elfScanner.findElf(module_.name);
        if (!elf.isValid())
            return {};
        uintptr_t address = elf.findSymbol(symbol);
        if (address == 0)
            address = elf.findDebugSymbol(symbol);
        if (address == 0)
            return {};
        return {address, AddressMeaning::PointerSlot, 80, "symbol:" + symbol};
    }

    BindingCandidates GlobalLocator::Locate(const std::vector<std::string> &objectSymbols,
                                            const std::vector<std::string> &nameSymbols) const
    {
        BindingCandidates result;
        for (const std::string &symbol : objectSymbols)
        {
            const LocatedAddress candidate = SymbolCandidate(symbol);
            if (candidate.address != 0)
                result.objectRoots.push_back(candidate);
        }
        for (const std::string &symbol : nameSymbols)
        {
            const LocatedAddress candidate = SymbolCandidate(symbol);
            if (candidate.address != 0)
                result.nameRoots.push_back(candidate);
        }

        AnalyzerMemoryAdapter adapter(memory_, module_);
        if (!adapter.Initialize())
            return result;
        const std::unique_ptr<IArchDecoder> decoder = CreateArchDecoder(EArch::Arm64);
        if (!decoder)
            return result;

        anduefker::analyzer::AnalyzerOptions options;
        options.ThreadMode = anduefker::analyzer::EThreadMode::Single;
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

        if (result.objectRoots.empty())
        {
            add(result.objectRoots, analyzer.Find(anduefker::analyzer::Targets::GUObjectArray), "GUObjectArray");
            add(result.objectRoots, analyzer.Find(anduefker::analyzer::Targets::ObjObjects), "ObjObjects");
        }
        if (result.nameRoots.empty())
            add(result.nameRoots, analyzer.Find(anduefker::analyzer::Targets::Names), "Names");
        return result;
    }
} // namespace anduefker::binding
