#include "anduefker/ue/FunctionSemantics.hpp"

#include <limits>

namespace anduefker::ue
{
    FunctionParameterSummary AnalyzeFunctionParameters(const std::vector<PropertyMetadata> &properties,
                                                         uint32_t functionFlags,
                                                         bool continueAfterDefaultInitializer)
    {
        FunctionParameterSummary result;
        for (size_t index = 0; index < properties.size(); ++index)
        {
            const PropertyMetadata &property = properties[index];
            if (property.arrayDim <= 0 || property.elementSize <= 0 || property.offset < 0)
            {
                result.valid = false;
                result.layoutConflict = true;
                result.scannedPropertyCount = index;
                break;
            }

            const int64_t end = static_cast<int64_t>(property.offset) +
                                static_cast<int64_t>(property.elementSize) * property.arrayDim;
            if (end <= property.offset || end > std::numeric_limits<uint16_t>::max())
            {
                result.valid = false;
                result.layoutConflict = true;
                result.scannedPropertyCount = index;
                break;
            }

            result.scannedPropertyCount = index + 1;

            if ((property.flags & kCPFParm) != 0)
            {
                ++result.count;
                result.paramEnd = static_cast<int32_t>(end);
                if ((property.flags & kCPFReturnParm) != 0)
                    result.returnOffset = property.offset;
                continue;
            }

            result.sawNonParameter = true;
            if (result.firstNonParameter == std::numeric_limits<size_t>::max())
                result.firstNonParameter = index;
            const bool hasDefaults = (functionFlags & kFUNCHasDefaults) != 0;
            if (!hasDefaults)
            {
                result.chainTerminated = true;
                break;
            }
            if ((property.flags & kCPFZeroConstructor) == 0)
            {
                ++result.defaultInitializerCount;
                if (!continueAfterDefaultInitializer)
                {
                    result.chainTerminated = true;
                    break;
                }
                result.continuedAfterDefaultInitializer = true;
            }
        }
        if (result.valid && !result.chainTerminated)
            result.chainTerminated = true;
        return result;
    }
} // namespace anduefker::ue
