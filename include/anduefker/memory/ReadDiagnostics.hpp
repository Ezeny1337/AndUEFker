#pragma once

#include <string>

#include "anduefker/memory/MemorySource.hpp"

namespace anduefker::memory
{
    // 计数器必须属于同一个内存源，且在各采样点之间不得发生重置
    [[nodiscard]] inline std::string DescribeReadStats(const ReadStats &after, const ReadStats &before = {})
    {
        return " operations=" + std::to_string(after.operations - before.operations) +
               " requested_bytes=" + std::to_string(after.requestedBytes - before.requestedBytes) +
               " transferred_bytes=" + std::to_string(after.transferredBytes - before.transferredBytes) +
               " cache_hits=" + std::to_string(after.cacheHits - before.cacheHits) +
               " cache_misses=" + std::to_string(after.cacheMisses - before.cacheMisses) +
               " backend_operations=" + std::to_string(after.backendOperations - before.backendOperations) +
               " backend_requested_bytes=" + std::to_string(after.backendRequestedBytes - before.backendRequestedBytes) +
               " backend_transferred_bytes=" + std::to_string(after.backendTransferredBytes - before.backendTransferredBytes) +
               " failures=" + std::to_string(after.failures - before.failures) +
               " not_initialized=" + std::to_string(after.notInitialized - before.notInitialized) +
               " invalid_arguments=" + std::to_string(after.invalidArguments - before.invalidArguments) +
               " unreadable_ranges=" + std::to_string(after.unreadableRanges - before.unreadableRanges) +
               " partial_reads=" + std::to_string(after.partialReads - before.partialReads) +
               " backend_failures=" + std::to_string(after.backendFailures - before.backendFailures);
    }
} // namespace anduefker::memory
