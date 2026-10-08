#include "anduefker/memory/CaptureMemorySource.hpp"

#include <algorithm>
#include <cstring>

namespace anduefker::memory
{
    void CaptureMemorySource::Reset() const
    {
        observations_.clear();
        observedBytes_ = 0;
        observing_ = false;
        limitExceeded_ = false;
        changedDuringRead_ = false;
        changesDuringRead_.clear();
        generation_ = source_.AddressSpaceGeneration();
        readFailures_ = 0;
        readFailureSamples_.clear();
    }

    ReadResult CaptureMemorySource::ReadBytes(uintptr_t address, void *buffer, size_t size) const
    {
        const auto readFresh = [&]()
        { return ReadFreshBytes(address, buffer, size); };
        if (buffer == nullptr || size == 0)
            return readFresh();

        if (!observing_)
            return readFresh();
        auto previous = observations_.find(address);
        if (previous != observations_.end() && previous->second.size() >= size)
        {
            std::memcpy(buffer, previous->second.data(), size);
            return {ReadError::None, address, size, size};
        }
        const ReadResult read = readFresh();
        if (!read.Ok())
            return read;

        const size_t previousSize = previous == observations_.end() ? 0 : previous->second.size();
        if (size > kMaxReadSize || size - previousSize > kMaxBytes - observedBytes_ ||
            (previous == observations_.end() && observations_.size() >= kMaxRanges))
        {
            limitExceeded_ = true;
            return read;
        }
        const auto *bytes = static_cast<const uint8_t *>(buffer);
        // A different start address must not silently freeze inconsistent overlapping bytes.
        const uintptr_t first = address >= kMaxReadSize ? address - kMaxReadSize : 0;
        for (auto overlap = observations_.lower_bound(first); overlap != observations_.end(); ++overlap)
        {
            if (overlap->first >= address && overlap->first - address >= size)
                break;
            const uintptr_t start = std::max(address, overlap->first);
            const size_t oldOffset = static_cast<size_t>(start - overlap->first);
            const size_t newOffset = static_cast<size_t>(start - address);
            if (oldOffset >= overlap->second.size() || newOffset >= size)
                continue;
            const size_t length = std::min(overlap->second.size() - oldOffset, size - newOffset);
            if (std::memcmp(bytes + newOffset, overlap->second.data() + oldOffset, length) != 0)
            {
                changedDuringRead_ = true;
                if (changesDuringRead_.size() < 8)
                {
                    const size_t sampleSize = std::min<size_t>(length, 16);
                    CaptureValidation::Change change;
                    change.address = start;
                    change.size = length;
                    change.read = {ReadError::None, start, length, length};
                    change.before.assign(overlap->second.data() + oldOffset, overlap->second.data() + oldOffset + sampleSize);
                    change.after.assign(bytes + newOffset, bytes + newOffset + sampleSize);
                    changesDuringRead_.push_back(std::move(change));
                }
            }
        }
        observations_[address] = std::vector<uint8_t>(bytes, bytes + size);
        observedBytes_ += size - previousSize;
        return read;
    }

    bool CaptureMemorySource::RefreshAddressSpace()
    {
        const bool refreshed = source_.RefreshAddressSpace();
        Reset();
        return refreshed;
    }

    CaptureValidation CaptureMemorySource::Validate() const
    {
        CaptureValidation result;
        result.observedRanges = observations_.size();
        result.observedBytes = observedBytes_;
        result.limitExceeded = limitExceeded_;
        result.generationChanged = generation_ != source_.AddressSpaceGeneration();
        result.changedRanges = changedDuringRead_ ? 1 : 0;
        result.changes = changesDuringRead_;
        result.readFailures = readFailures_;
        result.readFailureSamples = readFailureSamples_;
        constexpr size_t maxBatchSize = 64 * 1024;
        constexpr size_t maxGap = 64;
        std::vector<uint8_t> current(maxBatchSize);
        const auto compare = [&](uintptr_t address, const std::vector<uint8_t> &bytes,
                                 const uint8_t *data, const ReadResult &read)
        {
            if (!read.Ok())
                ++result.unreadableRanges;
            else if (std::memcmp(data, bytes.data(), bytes.size()) != 0)
                ++result.changedRanges;
            else
                return;
            if (result.failedAddresses.size() < 8)
                result.failedAddresses.push_back(address);
            if (result.changes.size() < 8)
            {
                const size_t sampleSize = std::min<size_t>(bytes.size(), 16);
                CaptureValidation::Change change;
                change.address = address;
                change.size = bytes.size();
                change.read = read;
                change.before.assign(bytes.begin(), bytes.begin() + sampleSize);
                if (read.Ok())
                    change.after.assign(data, data + sampleSize);
                result.changes.push_back(std::move(change));
            }
        };
        for (auto first = observations_.begin(); first != observations_.end();)
        {
            auto last = std::next(first);
            const uintptr_t start = first->first;
            size_t span = first->second.size();
            while (last != observations_.end())
            {
                const uintptr_t offset = last->first - start;
                if (offset > maxBatchSize || last->second.size() > maxBatchSize - offset ||
                    (offset > span && offset - span > maxGap))
                    break;
                span = std::max(span, static_cast<size_t>(offset) + last->second.size());
                ++last;
            }
            const auto batch = source_.ReadFreshBytes(start, current.data(), span);
            for (auto item = first; item != last; ++item)
            {
                const auto &[address, bytes] = *item;
                if (batch.Ok())
                {
                    // Gaps are transported but never compared or frozen.
                    compare(address, bytes, current.data() + (address - start),
                            {ReadError::None, address, bytes.size(), bytes.size()});
                }
                else
                {
                    std::vector<uint8_t> exact(bytes.size());
                    const auto read = source_.ReadFreshBytes(address, exact.data(), bytes.size());
                    compare(address, bytes, exact.data(), read);
                }
            }
            first = last;
        }
        result.generationChanged = result.generationChanged || generation_ != source_.AddressSpaceGeneration();
        return result;
    }
} // namespace anduefker::memory
