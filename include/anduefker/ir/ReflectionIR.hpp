#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace anduefker::ir
{
    enum class ParseStatus
    {
        Complete,
        Partial,
        Failed,
    };

    [[nodiscard]] inline const char *ParseStatusName(ParseStatus status)
    {
        switch (status)
        {
        case ParseStatus::Complete:
            return "Complete";
        case ParseStatus::Partial:
            return "Partial";
        case ParseStatus::Failed:
            return "Failed";
        }
        return "Failed";
    }

    enum class TypeKind
    {
        Class,
        Struct,
    };

    enum class PropertyKind
    {
        Unknown,
        Bool,
        Byte,
        Int8,
        Int16,
        Int32,
        Int64,
        UInt16,
        UInt32,
        UInt64,
        Float,
        Double,
        Name,
        String,
        Text,
        Object,
        SoftObject,
        WeakObject,
        LazyObject,
        Class,
        SoftClass,
        Struct,
        Enum,
        Array,
        Set,
        Map,
        Interface,
        Delegate,
        MulticastDelegate,
        FieldPath,
        Optional,
    };

    struct TypeReferenceIR
    {
        PropertyKind kind = PropertyKind::Unknown;
        std::string reflectedClass;
        uintptr_t referencedObject = 0;
        uintptr_t secondaryObject = 0;
        int32_t elementSize = 0;
        bool detailsResolved = false;
        std::shared_ptr<TypeReferenceIR> inner;
        std::shared_ptr<TypeReferenceIR> key;
        std::shared_ptr<TypeReferenceIR> value;
    };

    struct BoolLayoutIR
    {
        uint8_t fieldSize = 0;
        uint8_t byteOffset = 0;
        uint8_t byteMask = 0;
        uint8_t fieldMask = 0;
    };

    struct PropertyDetailDiagnostic
    {
        bool headerAvailable = true;
        uintptr_t address = 0;
        uintptr_t immediateOwner = 0;
        bool ownerIsUObject = false;
        std::string name;
        std::string reflectedClass;
        std::string normalizedClass;
        std::string reason;
        std::string detailsStatus;
        int32_t offset = 0;
        int32_t elementSize = 0;
        int32_t arrayDim = 0;
        uint64_t flags = 0;
        uintptr_t referencedAddress = 0;
        uintptr_t secondaryAddress = 0;
        std::string referencedClass;
        std::string secondaryClass;
        BoolLayoutIR boolean;
        struct DetailRead
        {
            std::string member;
            int32_t offset = -1;
            uintptr_t address = 0;
            uintptr_t rawValue = 0;
            int32_t error = 0;
            size_t requested = 0;
            size_t transferred = 0;
        };
        std::vector<DetailRead> reads;
    };

    struct PropertyIR
    {
        uintptr_t address = 0;
        std::string name;
        std::string reflectedClass;
        int32_t offset = 0;
        int32_t elementSize = 0;
        int32_t arrayDim = 0;
        uint64_t flags = 0;
        bool isParameter = false;
        bool isReturnParameter = false;
        bool isOutParameter = false;
        bool isReferenceParameter = false;
        bool isConstParameter = false;
        bool typeDetailsResolved = true;
        ParseStatus status = ParseStatus::Complete;
        std::vector<std::string> diagnostics;
        TypeReferenceIR type;
        BoolLayoutIR boolean;
        // 仅保留失败节点的事实，嵌套属性受 ReadTypeReference 的遍历预算约束。
        std::vector<PropertyDetailDiagnostic> detailDiagnostics;
    };

    struct FunctionIR
    {
        uintptr_t address = 0;
        uintptr_t nativeRva = 0;
        uintptr_t nativeAddress = 0;
        std::string name;
        std::string fullName;
        uint32_t flags = 0;
        uint8_t numParams = 0;
        uint16_t paramSize = 0;
        uint16_t returnValueOffset = 0xFFFFu;
        uint8_t headerNumParams = 0;
        uint16_t headerParamSize = 0;
        uint32_t derivedNumParams = 0;
        int32_t derivedParamSize = 0;
        uint32_t defaultInitializerCount = 0;
        bool parameterSemanticsValid = false;
        bool parameterSemanticsConsistent = false;
        std::vector<PropertyIR> parameters;
        std::vector<PropertyIR> locals;
        std::vector<std::string> layoutConflicts;
        ParseStatus status = ParseStatus::Complete;
    };

    struct TypeIR
    {
        uintptr_t address = 0;
        uintptr_t superAddress = 0;
        TypeKind kind = TypeKind::Struct;
        std::string name;
        std::string fullName;
        int32_t size = 0;
        std::vector<PropertyIR> properties;
        std::vector<FunctionIR> functions;
        std::vector<std::string> layoutConflicts;
        ParseStatus status = ParseStatus::Complete;
    };

    struct EnumValueIR
    {
        std::string name;
        int64_t value = 0;
    };

    enum class EnumUnderlyingType
    {
        Unknown,
        Int8,
        UInt8,
        Int16,
        UInt16,
        Int32,
        UInt32,
        Int64,
        UInt64,
    };

    struct EnumIR
    {
        uintptr_t address = 0;
        std::string name;
        std::string fullName;
        EnumUnderlyingType underlyingType = EnumUnderlyingType::Unknown;
        uint8_t cppForm = 0;
        uint8_t flags = 0;
        std::vector<EnumValueIR> values;
        ParseStatus status = ParseStatus::Complete;
        int32_t expectedValues = -1;
        std::vector<std::string> diagnostics;
    };

    struct ReflectionStats
    {
        int32_t objectSlots = 0;
        int32_t validObjects = 0;
        int32_t parsedTypes = 0;
        int32_t parsedEnums = 0;
        int32_t parsedFunctions = 0;
        int32_t parsedProperties = 0;
        int32_t unknownProperties = 0;
        int32_t unresolvedTypeDetails = 0;
        int32_t layoutConflicts = 0;
        // 包含空槽、槽位读取失败、类默认对象和未完成加载的反射定义。
        int32_t skippedObjects = 0;
        int32_t emptyObjectSlots = 0;
        int32_t objectReadFailures = 0;
        int32_t classNameReadFailures = 0;
        int32_t skippedClassDefaultObjects = 0;
        int32_t skippedIncompleteObjects = 0;
        int32_t objectDiagnosticSamplesOmitted = 0;
        int32_t failures = 0;
        int32_t enumReadFailures = 0;
        int32_t identityFailures = 0;
        int32_t unvisitedObjects = 0;
    };

    struct CaptureInfo
    {
        bool observationsStable = false;
        bool limitExceeded = false;
        bool generationChanged = false;
        size_t observedRanges = 0;
        size_t observedBytes = 0;
        size_t changedRanges = 0;
        size_t unreadableRanges = 0;
        uint32_t attempts = 0;
        struct ReadFailure
        {
            uintptr_t address = 0;
            int32_t error = 0;
            size_t requested = 0;
            size_t transferred = 0;
        };
        size_t readFailures = 0;
        std::vector<ReadFailure> readFailureSamples;
    };

    struct ReflectionIR
    {
        ParseStatus status = ParseStatus::Failed;
        ReflectionStats stats;
        CaptureInfo capture;
        std::vector<std::string> diagnostics;
        std::vector<TypeIR> types;
        std::vector<EnumIR> enums;
    };
} // namespace anduefker::ir
