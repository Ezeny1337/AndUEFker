#include "anduefker/analyzer/AnalyzerMemoryAdapter.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#include <KittyMemoryEx.hpp>

namespace anduefker::analyzer
{
    AnalyzerMemoryAdapter::AnalyzerMemoryAdapter(RemoteMemorySource &memory, const ModuleImage &module)
        : memory_(&memory), module_(module)
    {
        RebuildMaps();
    }

    void AnalyzerMemoryAdapter::RebuildMaps()
    {
        regions_.clear();
        if (memory_ == nullptr || !memory_->IsInitialized())
            return;
        const auto maps = KittyMemoryEx::getAllMaps(memory_->ProcessId());
        regions_.reserve(maps.size());
        for (const auto &map : maps)
            regions_.emplace_back(map.pathname, map.startAddress, map.endAddress, map.offset,
                                  map.readable, map.writeable, map.executable);
        std::sort(regions_.begin(), regions_.end(), [](const MemRegionInfo &left, const MemRegionInfo &right)
                  { return left.GetStart() < right.GetStart(); });

        std::vector<MemRegionInfo> segments;
        for (const auto &segment : module_.segments)
            segments.emplace_back(segment.path, segment.start, segment.end, segment.fileOffset,
                                  segment.readable, segment.writable, segment.executable);
        unreal_ = ModuleInfo(module_.name, module_.base, module_.end, module_.base, std::move(segments));
    }

    bool AnalyzerMemoryAdapter::Initialize()
    {
        return memory_ != nullptr && memory_->IsInitialized() && RefreshMemory();
    }

    std::string AnalyzerMemoryAdapter::GetProcessName() const
    {
        return memory_ ? memory_->Manager().processName() : std::string();
    }

    int AnalyzerMemoryAdapter::GetProcessID() const
    {
        return memory_ ? static_cast<int>(memory_->ProcessId()) : 0;
    }

    bool AnalyzerMemoryAdapter::RefreshMemory()
    {
        if (!memory_ || !memory_->RefreshAddressSpace())
            return false;
        RebuildMaps();
        return IsMemoryAccessOk();
    }

    bool AnalyzerMemoryAdapter::IsMemoryAccessOk() const
    {
        return memory_ != nullptr && memory_->IsInitialized() && module_.IsValid() && !regions_.empty();
    }

    bool AnalyzerMemoryAdapter::IsValidAddress(uintptr_t address) const
    {
        return GetAddressRegionInfo(address).IsValid();
    }

    bool AnalyzerMemoryAdapter::IsAddressReadable(uintptr_t address, size_t size) const
    {
        return memory_ && memory_->IsReadable(address, size);
    }

    bool AnalyzerMemoryAdapter::IsAddressWriteable(uintptr_t address, size_t size) const
    {
        if (address == 0 || size == 0 || address > UINTPTR_MAX - (size - 1))
            return false;
        const uintptr_t last = address + size - 1;
        const MemRegionInfo region = GetAddressRegionInfo(address);
        return region.IsValid() && region.Contains(last) && region.IsWriteable();
    }

    bool AnalyzerMemoryAdapter::IsAddressExecutable(uintptr_t address, size_t size) const
    {
        return memory_ && memory_->IsExecutable(address, size);
    }

    MemRegionInfo AnalyzerMemoryAdapter::GetAddressRegionInfo(uintptr_t address) const
    {
        for (const MemRegionInfo &region : regions_)
            if (region.Contains(address))
                return region;
        return {};
    }

    size_t AnalyzerMemoryAdapter::ReadBytes(uintptr_t address, void *buffer, size_t size) const
    {
        if (!memory_)
            return 0;
        return memory_->ReadBytes(address, buffer, size).transferred;
    }

    bool AnalyzerMemoryAdapter::WriteBytes(uintptr_t address, const void *buffer, size_t size) const
    {
        (void)address;
        (void)buffer;
        (void)size;
        return false;
    }

    bool AnalyzerMemoryAdapter::ReadRelocationPointer(uintptr_t slot, uintptr_t &out) const
    {
        if (!memory_ || !memory_->Read(slot, out) || out == 0)
            return false;
        return IsAddressReadable(out);
    }

    ModuleInfo AnalyzerMemoryAdapter::GetModuleInfo(const std::string &moduleName)
    {
        if (moduleName == module_.name || module_.name.find(moduleName) != std::string::npos)
            return unreal_;
        return {};
    }

    uintptr_t AnalyzerMemoryAdapter::FindModuleSymbol(const std::string &moduleName, const std::string &symbolName)
    {
        if (!memory_ || moduleName != module_.name)
            return 0;
        auto elf = memory_->Manager().elfScanner.findElf(moduleName);
        if (!elf.isValid())
            return 0;
        uintptr_t address = elf.findSymbol(symbolName);
        return address != 0 ? address : elf.findDebugSymbol(symbolName);
    }

    ModuleInfo AnalyzerMemoryAdapter::GetUnrealModule()
    {
        return unreal_;
    }

    uintptr_t AnalyzerMemoryAdapter::FindUnrealSymbol(const std::string &symbolName)
    {
        return FindModuleSymbol(module_.name, symbolName);
    }

    bool AnalyzerMemoryAdapter::RangeEnd(uintptr_t start, size_t range, uintptr_t &end) const
    {
        if (range == 0 || start > UINTPTR_MAX - range)
            return false;
        end = start + range;
        return true;
    }

    std::vector<uintptr_t> AnalyzerMemoryAdapter::FindAllPatternInRange(uintptr_t start, size_t range,
                                                                        const std::string &pattern, int step,
                                                                        size_t maxHits) const
    {
        (void)step;
        uintptr_t end = 0;
        if (!memory_ || pattern.empty() || !RangeEnd(start, range, end))
            return {};
        auto hits = memory_->Manager().memScanner.findIdaPatternAll(start, end, pattern);
        if (maxHits != 0 && hits.size() > maxHits)
            hits.resize(maxHits);
        return hits;
    }

    uintptr_t AnalyzerMemoryAdapter::FindPatternInRange(uintptr_t start, size_t range,
                                                        const std::string &pattern, int step,
                                                        uint32_t skipCount) const
    {
        const auto hits = FindAllPatternInRange(start, range, pattern, step, 0);
        return skipCount < hits.size() ? hits[skipCount] : 0;
    }

    std::vector<uintptr_t> AnalyzerMemoryAdapter::FindAllAlignedValuesInRange(uintptr_t value,
                                                                              int32_t alignment,
                                                                              uintptr_t start,
                                                                              size_t range,
                                                                              size_t maxHits) const
    {
        std::vector<uintptr_t> hits;
        uintptr_t end = 0;
        if (alignment <= 0 || !RangeEnd(start, range, end))
            return hits;
        const uintptr_t step = static_cast<uintptr_t>(alignment);
        if (start % step != 0)
            start += step - (start % step);
        for (uintptr_t cursor = start; cursor + sizeof(uintptr_t) <= end; cursor += step)
        {
            uintptr_t stored = 0;
            if (!memory_->Read(cursor, stored) || stored != value)
                continue;
            hits.push_back(cursor);
            if (maxHits != 0 && hits.size() >= maxHits)
                break;
        }
        return hits;
    }

    uintptr_t AnalyzerMemoryAdapter::FindAlignedValueInRange(uintptr_t value, int32_t alignment,
                                                             uintptr_t start, size_t range) const
    {
        const auto hits = FindAllAlignedValuesInRange(value, alignment, start, range, 1);
        return hits.empty() ? 0 : hits.front();
    }

    std::vector<uintptr_t> AnalyzerMemoryAdapter::FindAllRawDataInRange(const void *data,
                                                                        size_t dataSize,
                                                                        uintptr_t start,
                                                                        size_t range,
                                                                        size_t maxHits) const
    {
        std::vector<uintptr_t> hits;
        uintptr_t end = 0;
        if (!memory_ || !data || dataSize == 0 || !RangeEnd(start, range, end) || end < start + dataSize)
            return hits;

        constexpr size_t chunkSize = 1024 * 1024;
        std::vector<uint8_t> buffer;
        uintptr_t cursor = start;
        while (cursor + dataSize <= end)
        {
            const size_t wanted = std::min(chunkSize, static_cast<size_t>(end - cursor));
            buffer.resize(wanted);
            const size_t received = ReadBytes(cursor, buffer.data(), wanted);
            if (received < dataSize)
                break;
            for (size_t offset = 0; offset + dataSize <= received; ++offset)
            {
                if (std::memcmp(buffer.data() + offset, data, dataSize) != 0)
                    continue;
                hits.push_back(cursor + offset);
                if (maxHits != 0 && hits.size() >= maxHits)
                    return hits;
            }
            cursor += received - dataSize + 1;
        }
        return hits;
    }

    uintptr_t AnalyzerMemoryAdapter::FindRawDataInRange(const void *data, size_t dataSize,
                                                        uintptr_t start, size_t range) const
    {
        const auto hits = FindAllRawDataInRange(data, dataSize, start, range, 1);
        return hits.empty() ? 0 : hits.front();
    }
} // namespace anduefker::analyzer
