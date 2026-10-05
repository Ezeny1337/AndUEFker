#pragma once

#include <unordered_map>
#include <vector>

#include "anduefker/memory/MemorySource.hpp"

namespace anduefker::memory
{
    struct CaptureValidation
    {
        size_t observedRanges = 0;
        size_t observedBytes = 0;
        size_t changedRanges = 0;
        size_t unreadableRanges = 0;
        bool limitExceeded = false;
        bool generationChanged = false;
        std::vector<uintptr_t> failedAddresses;

        [[nodiscard]] bool Stable() const
        {
            return !limitExceeded && !generationChanged && changedRanges == 0 && unreadableRanges == 0;
        }
    };

    // 单线程采集视图，只冻结已消费的字节；验证稳定不等于获得原子快照。
    class CaptureMemorySource final : public IMemorySource
    {
    public:
        explicit CaptureMemorySource(IMemorySource &source) : source_(source) {}

        void Reset() const;
        void Observe(bool enabled) const { observing_ = enabled; }
        [[nodiscard]] bool IsObserving() const { return observing_; }
        [[nodiscard]] bool LimitExceeded() const { return limitExceeded_; }
        void Invalidate() const { changedDuringRead_ = true; }
        [[nodiscard]] CaptureValidation Validate() const;
        [[nodiscard]] bool IsInitialized() const override { return source_.IsInitialized(); }
        [[nodiscard]] pid_t ProcessId() const override { return source_.ProcessId(); }
        [[nodiscard]] uint64_t AddressSpaceGeneration() const override { return source_.AddressSpaceGeneration(); }
        [[nodiscard]] bool RefreshAddressSpace() override;
        [[nodiscard]] bool IsReadable(uintptr_t address, size_t size) const override { return source_.IsReadable(address, size); }
        [[nodiscard]] bool IsExecutable(uintptr_t address, size_t size) const override { return source_.IsExecutable(address, size); }
        [[nodiscard]] ReadResult ReadBytes(uintptr_t address, void *buffer, size_t size) const override;
        [[nodiscard]] ReadResult ReadFreshBytes(uintptr_t address, void *buffer, size_t size) const override
        {
            return source_.ReadFreshBytes(address, buffer, size);
        }
        [[nodiscard]] const ReadStats &Stats() const override { return source_.Stats(); }

    private:
        static constexpr size_t kMaxRanges = 2 * 1024 * 1024;
        static constexpr size_t kMaxBytes = 64 * 1024 * 1024;
        static constexpr size_t kMaxReadSize = 2048;

        IMemorySource &source_;
        mutable std::unordered_map<uintptr_t, std::vector<uint8_t>> observations_;
        mutable size_t observedBytes_ = 0;
        mutable bool observing_ = false;
        mutable bool limitExceeded_ = false;
        mutable bool changedDuringRead_ = false;
        mutable uint64_t generation_ = 0;
    };

    class CaptureObservationScope
    {
    public:
        explicit CaptureObservationScope(const CaptureMemorySource &memory)
            : memory_(memory), previous_(memory.IsObserving()) { memory_.Observe(true); }
        ~CaptureObservationScope() { memory_.Observe(previous_); }
        CaptureObservationScope(const CaptureObservationScope &) = delete;
        CaptureObservationScope &operator=(const CaptureObservationScope &) = delete;

    private:
        const CaptureMemorySource &memory_;
        bool previous_ = false;
    };
} // namespace anduefker::memory
