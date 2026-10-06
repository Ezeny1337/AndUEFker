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
        generation_ = source_.AddressSpaceGeneration();
        readFailures_ = 0;
        readFailureSamples_.clear();
    }

    ReadResult CaptureMemorySource::ReadBytes(uintptr_t address, void *buffer, size_t size) const
    {
        const auto readFresh = [&]()
        {
            const ReadResult read = source_.ReadFreshBytes(address, buffer, size);
            if (!read.Ok())
            {
                ++readFailures_;
                if (readFailureSamples_.size() < 16)
                    readFailureSamples_.push_back(read);
            }
            return read;
        };
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
        if (previousSize != 0 && std::memcmp(buffer, previous->second.data(), previousSize) != 0)
            changedDuringRead_ = true;
        if (size > kMaxReadSize || size - previousSize > kMaxBytes - observedBytes_ ||
            (previous == observations_.end() && observations_.size() >= kMaxRanges))
        {
            limitExceeded_ = true;
            return read;
        }
        const auto *bytes = static_cast<const uint8_t *>(buffer);
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
        result.readFailures = readFailures_;
        result.readFailureSamples = readFailureSamples_;
        std::vector<uint8_t> current(kMaxReadSize);
        for (const auto &[address, bytes] : observations_)
        {
            const ReadResult read = source_.ReadFreshBytes(address, current.data(), bytes.size());
            if (!read.Ok())
                ++result.unreadableRanges;
            else if (std::memcmp(current.data(), bytes.data(), bytes.size()) != 0)
                ++result.changedRanges;
            else
                continue;
            if (result.failedAddresses.size() < 8)
                result.failedAddresses.push_back(address);
        }
        result.generationChanged = result.generationChanged || generation_ != source_.AddressSpaceGeneration();
        return result;
    }
} // namespace anduefker::memory
