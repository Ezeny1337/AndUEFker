#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "anduefker/ue/ObjectModelReader.hpp"

namespace anduefker::ue
{
    inline constexpr uint64_t kCPFParm = 0x0000000000000080ull;
    inline constexpr uint64_t kCPFReturnParm = 0x0000000000000400ull;
    inline constexpr uint64_t kCPFZeroConstructor = 0x0000000000000200ull;
    inline constexpr uint32_t kFUNCNative = 0x00000400u;
    inline constexpr uint32_t kFUNCHasDefaults = 0x00800000u;

    struct FunctionParameterSummary
    {
        uint32_t count = 0;
        int32_t paramEnd = 0;
        int32_t returnOffset = -1;
        size_t firstNonParameter = std::numeric_limits<size_t>::max();
        size_t scannedPropertyCount = 0;
        uint32_t defaultInitializerCount = 0;
        bool valid = true;
        bool sawNonParameter = false;
        bool chainTerminated = false;
        bool layoutConflict = false;
        bool continuedAfterDefaultInitializer = false;
    };

    [[nodiscard]] FunctionParameterSummary AnalyzeFunctionParameters(const std::vector<PropertyMetadata> &properties,
                                                                     uint32_t functionFlags,
                                                                     bool continueAfterDefaultInitializer);
}
