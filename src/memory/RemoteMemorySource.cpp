#include "anduefker/memory/RemoteMemorySource.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace anduefker::memory
{
    namespace
    {
        bool IsOverflow(uintptr_t address, size_t size)
        {
            return size == 0 || address > std::numeric_limits<uintptr_t>::max() - (size - 1);
        }

        uintptr_t PageAddress(uintptr_t address)
        {
            return address & ~(RemoteMemorySource::kPageSize - 1);
        }

        size_t PageOffset(uintptr_t address)
        {
            return static_cast<size_t>(address & (RemoteMemorySource::kPageSize - 1));
        }
    } // namespace

    RemoteMemorySource::RemoteMemorySource(size_t cacheSize) : maxCacheSize_(cacheSize)
    {
    }

    bool RemoteMemorySource::Initialize(pid_t pid)
    {
        stats_ = {};
        ClearCache();

        if (!manager_.initialize(pid, EK_MEM_OP_SYSCALL, false) &&
            !manager_.initialize(pid, EK_MEM_OP_IO, false))
        {
            return false;
        }

        validator_.setPID(pid);
        validator_.setUseCache(true);
        return !validator_.cachedRegions().empty();
    }

    bool RemoteMemorySource::IsInitialized() const
    {
        return manager_.isMemValid();
    }

    pid_t RemoteMemorySource::ProcessId() const
    {
        return manager_.processID();
    }

    bool RemoteMemorySource::RefreshAddressSpace()
    {
        if (!IsInitialized())
            return false;

        validator_.refreshRegionCache();
        ClearCache();
        return !validator_.cachedRegions().empty();
    }

    bool RemoteMemorySource::IsReadable(uintptr_t address, size_t size) const
    {
        return IsInitialized() && address != 0 && !IsOverflow(address, size) &&
               validator_.isPtrReadable(address, size);
    }

    bool RemoteMemorySource::IsExecutable(uintptr_t address, size_t size) const
    {
        return IsInitialized() && address != 0 && !IsOverflow(address, size) &&
               validator_.isPtrExecutable(address, size);
    }

    const uint8_t *RemoteMemorySource::GetCached(uintptr_t address, size_t size) const
    {
        if (!cacheEnabled_ || size == 0 || size > kPageSize)
            return nullptr;

        const uintptr_t page = PageAddress(address);
        const size_t offset = PageOffset(address);
        if (offset > kPageSize - size)
            return nullptr;

        const auto it = pages_.find(page);
        if (it == pages_.end())
            return nullptr;

        for (size_t index = 0; index < size; ++index)
        {
            if (!it->second.valid.test(offset + index))
                return nullptr;
        }

        Touch(const_cast<CachePage &>(it->second));
        return it->second.bytes.data() + offset;
    }

    void RemoteMemorySource::PutCached(uintptr_t address, const uint8_t *data, size_t size) const
    {
        if (!cacheEnabled_ || data == nullptr || size == 0 || size > kPageSize)
            return;

        const uintptr_t page = PageAddress(address);
        const size_t offset = PageOffset(address);
        if (offset > kPageSize - size)
            return;

        auto it = pages_.find(page);
        if (it == pages_.end())
        {
            EvictIfNeeded();

            CachePage entry;
            entry.address = page;
            lru_.push_front(page);
            entry.lru = lru_.begin();
            it = pages_.emplace(page, std::move(entry)).first;
            cacheSize_ += kPageSize;
        }
        else
        {
            Touch(it->second);
        }

        std::memcpy(it->second.bytes.data() + offset, data, size);
        for (size_t index = 0; index < size; ++index)
            it->second.valid.set(offset + index);
    }

    void RemoteMemorySource::Touch(CachePage &page) const
    {
        lru_.erase(page.lru);
        lru_.push_front(page.address);
        page.lru = lru_.begin();
    }

    void RemoteMemorySource::EvictIfNeeded() const
    {
        while (cacheSize_ + kPageSize > maxCacheSize_ && !lru_.empty())
        {
            const uintptr_t page = lru_.back();
            lru_.pop_back();
            pages_.erase(page);
            cacheSize_ -= kPageSize;
        }
    }

    void RemoteMemorySource::ClearCache() const
    {
        pages_.clear();
        lru_.clear();
        cacheSize_ = 0;
    }

    ReadResult RemoteMemorySource::ReadBytes(uintptr_t address, void *buffer, size_t size) const
    {
        ReadResult result{ReadError::None, address, size, 0};
        ++stats_.operations;
        stats_.requestedBytes += size;

        if (!IsInitialized())
        {
            result.error = ReadError::NotInitialized;
            ++stats_.failures;
            return result;
        }
        if (buffer == nullptr || address == 0 || IsOverflow(address, size))
        {
            result.error = ReadError::InvalidArgument;
            ++stats_.failures;
            return result;
        }
        if (!IsReadable(address, size))
        {
            result.error = ReadError::UnreadableRange;
            ++stats_.failures;
            return result;
        }

        if (const uint8_t *cached = GetCached(address, size))
        {
            std::memcpy(buffer, cached, size);
            result.transferred = size;
            stats_.transferredBytes += size;
            return result;
        }

        result.transferred = manager_.readMem(address, buffer, size);
        stats_.transferredBytes += result.transferred;
        if (result.transferred != size)
        {
            result.error = result.transferred == 0 ? ReadError::BackendFailure : ReadError::PartialRead;
            ++stats_.failures;
            return result;
        }

        PutCached(address, static_cast<const uint8_t *>(buffer), size);
        return result;
    }
} // namespace anduefker::memory
