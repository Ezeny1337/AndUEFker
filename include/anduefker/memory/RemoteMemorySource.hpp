#pragma once

#include <array>
#include <bitset>
#include <list>
#include <unordered_map>

#include <KittyMemoryMgr.hpp>
#include <KittyPtrValidator.hpp>

#include "anduefker/memory/MemorySource.hpp"

namespace anduefker::memory
{
    class RemoteMemorySource final : public IMemorySource
    {
    public:
        static constexpr size_t kPageSize = 4096;
        static constexpr size_t kDefaultCacheSize = 16 * 1024 * 1024;

        explicit RemoteMemorySource(size_t cacheSize = kDefaultCacheSize);
        ~RemoteMemorySource() override = default;

        RemoteMemorySource(const RemoteMemorySource &) = delete;
        RemoteMemorySource &operator=(const RemoteMemorySource &) = delete;

        bool Initialize(pid_t pid);
        [[nodiscard]] bool IsInitialized() const override;
        [[nodiscard]] pid_t ProcessId() const override;
        [[nodiscard]] bool RefreshAddressSpace() override;
        [[nodiscard]] bool IsReadable(uintptr_t address, size_t size) const override;
        [[nodiscard]] bool IsExecutable(uintptr_t address, size_t size) const override;
        [[nodiscard]] ReadResult ReadBytes(uintptr_t address, void *buffer, size_t size) const override;
        [[nodiscard]] const ReadStats &Stats() const override { return stats_; }

        void ClearCache() const;
        void EnableCache(bool enabled) const { cacheEnabled_ = enabled; }
        [[nodiscard]] KittyMemoryMgr &Manager() { return manager_; }
        [[nodiscard]] const KittyMemoryMgr &Manager() const { return manager_; }

    private:
        struct CachePage
        {
            uintptr_t address = 0;
            std::array<uint8_t, kPageSize> bytes{};
            std::bitset<kPageSize> valid;
            std::list<uintptr_t>::iterator lru;
        };

        [[nodiscard]] const uint8_t *GetCached(uintptr_t address, size_t size) const;
        void PutCached(uintptr_t address, const uint8_t *data, size_t size) const;
        void Touch(CachePage &page) const;
        void EvictIfNeeded() const;

        KittyMemoryMgr manager_;
        mutable KittyPtrValidator validator_;
        mutable ReadStats stats_;
        mutable std::unordered_map<uintptr_t, CachePage> pages_;
        mutable std::list<uintptr_t> lru_;
        mutable size_t cacheSize_ = 0;
        size_t maxCacheSize_ = kDefaultCacheSize;
        mutable bool cacheEnabled_ = true;
    };
} // namespace anduefker::memory
