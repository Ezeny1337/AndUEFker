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
        TypeReferenceIR type;
        BoolLayoutIR boolean;
    };

    struct FunctionIR
    {
        uintptr_t address = 0;
        uintptr_t nativeRva = 0;
        std::string name;
        std::string fullName;
        uint32_t flags = 0;
        uint8_t numParams = 0;
        uint16_t paramSize = 0;
        std::vector<PropertyIR> parameters;
        std::vector<std::string> layoutConflicts;
    };

    struct TypeIR
    {
        uintptr_t address = 0;
        uintptr_t packageAddress = 0;
        uintptr_t superAddress = 0;
        TypeKind kind = TypeKind::Struct;
        std::string name;
        std::string fullName;
        int32_t size = 0;
        std::vector<PropertyIR> properties;
        std::vector<FunctionIR> functions;
        std::vector<std::string> layoutConflicts;
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
        uintptr_t packageAddress = 0;
        std::string name;
        std::string fullName;
        EnumUnderlyingType underlyingType = EnumUnderlyingType::Unknown;
        uint8_t cppForm = 0;
        uint8_t flags = 0;
        std::vector<EnumValueIR> values;
    };

    struct PackageIR
    {
        uintptr_t address = 0;
        std::string name;
        std::vector<size_t> types;
        std::vector<size_t> enums;
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
        int32_t skippedObjects = 0;
        int32_t failures = 0;
    };

    struct ReflectionIR
    {
        ParseStatus status = ParseStatus::Failed;
        ReflectionStats stats;
        std::vector<std::string> diagnostics;
        std::vector<PackageIR> packages;
        std::vector<TypeIR> types;
        std::vector<EnumIR> enums;
        std::unordered_map<uintptr_t, size_t> packageIndex;
    };
} // namespace anduefker::ir
