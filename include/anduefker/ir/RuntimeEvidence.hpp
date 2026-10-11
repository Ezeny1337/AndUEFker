#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace anduefker::ir
{
    struct ContainerElementSample
    {
        int32_t index = 0;
        uintptr_t address = 0;
        std::vector<uint8_t> bytes;
    };

    struct ContainerValue
    {
        std::string status = "not-read";
        std::string layoutId;
        std::string pointerEncoding;
        uintptr_t address = 0;
        uintptr_t data = 0;
        int32_t slots = -1;
        int32_t count = -1;
        int32_t capacity = -1;
        bool indicesComplete = false;
        bool elementsComplete = false;
        bool dataFrozen = false;
        std::vector<uint8_t> header;
        std::vector<ContainerElementSample> elements;
    };

    struct ValueCapture
    {
        bool observationsStable = false;
        size_t observedRanges = 0;
        size_t observedBytes = 0;
        size_t changedRanges = 0;
        size_t unreadableRanges = 0;
        size_t readFailures = 0;
        bool limitExceeded = false;
        bool generationChanged = false;
        struct ReadFailure
        {
            uintptr_t address = 0;
            int32_t error = 0;
            size_t requested = 0;
            size_t transferred = 0;
        };
        struct Change
        {
            uintptr_t address = 0;
            size_t size = 0;
            int32_t error = 0;
            size_t transferred = 0;
            std::vector<uint8_t> before;
            std::vector<uint8_t> after;
        };
        std::vector<ReadFailure> failures;
        std::vector<Change> changes;
    };

    struct DelegateBinding
    {
        uintptr_t address = 0;
        int32_t objectIndex = -1;
        int32_t objectSerial = 0;
        uintptr_t target = 0;
        std::string targetStatus;
        std::string functionName;
    };

    struct ValueObservation
    {
        uintptr_t objectAddress = 0;
        uintptr_t classAddress = 0;
        uintptr_t ownerAddress = 0;
        uintptr_t propertyAddress = 0;
        uintptr_t address = 0;
        std::string path;
        std::string kind;
        std::string layoutId;
        std::vector<std::string> layoutCandidates;
        std::map<std::string, std::string> candidateStatuses;
        bool classDefaultObject = false;
        std::string status = "not-read";
        std::vector<uint8_t> bytes;
        std::map<std::string, int64_t> numbers;
        std::map<std::string, uintptr_t> addresses;
        std::optional<ContainerValue> container;
        std::vector<DelegateBinding> bindings;
        bool bindingsComplete = false;
        ValueCapture capture;
    };

    struct SparseRootEvidence
    {
        std::string symbol = "_ZN22FSparseDelegateStorage15SparseDelegatesE";
        std::string status = "not-queried";
        std::string source;
        uintptr_t address = 0;
        std::vector<std::string> diagnostics;
        std::optional<ContainerValue> owners;
        bool enumerationComplete = false;
        ValueCapture capture;
        std::vector<ValueObservation> delegates;
    };

    // 可选的动态值证据独立于反射定义，没有实例或无法读取不会降低 SDK 完整性
    struct RuntimeEvidence
    {
        std::string status = "not-collected";
        int32_t objectSlots = 0;
        int32_t scannedSlots = 0;
        bool scanComplete = false;
        std::map<std::string, size_t> skipped;
        std::map<std::string, size_t> statuses;
        std::vector<ValueObservation> values;
        SparseRootEvidence sparse;
    };
} // namespace anduefker::ir
