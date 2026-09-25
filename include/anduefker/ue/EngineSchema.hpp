#pragma once

#include <cstdint>
#include <string>

namespace anduefker::ue
{
    enum class EngineFamily
    {
        Unknown,
        UE4UProperty,
        UE4FProperty,
        UE5FProperty,
    };

    struct EngineFeatures
    {
        bool useFProperty = false;
        bool useNamePool = false;
        bool casePreservingName = false;
        bool outlineNumberName = false;
        bool fFieldOwnerMask = false;
        bool enumHasUnderlyingType = false;
        bool enumUsesFNameData = false;
        bool enumStoresValues = true;
        bool arrayDimIsByte = false;
        bool largeWorldCoordinates = false;
    };

    struct FNameSchema
    {
        int32_t comparisonIndex = -1;
        int32_t number = -1;
        int32_t size = -1;
    };

    struct UObjectSchema
    {
        int32_t vtable = 0;
        int32_t flags = -1;
        int32_t internalIndex = -1;
        int32_t classPointer = -1;
        int32_t name = -1;
        int32_t outer = -1;
    };

    struct UFieldSchema
    {
        int32_t next = -1;
    };

    struct FFieldSchema
    {
        int32_t vtable = 0;
        int32_t classPointer = -1;
        int32_t owner = -1;
        int32_t next = -1;
        int32_t name = -1;
        int32_t editorOnlyMetadata = -1;
    };

    struct FFieldClassSchema
    {
        int32_t name = -1;
        int32_t castFlags = -1;
    };

    struct UStructSchema
    {
        int32_t superStruct = -1;
        int32_t children = -1;
        int32_t childProperties = -1;
        int32_t size = -1;
        int32_t minAlignment = -1;
        int32_t structBaseChain = -1;
    };

    struct UClassSchema
    {
        int32_t castFlags = -1;
        int32_t classDefaultObject = -1;
        int32_t implementedInterfaces = -1;
    };

    struct UFunctionSchema
    {
        int32_t functionFlags = -1;
        int32_t numParams = -1;
        int32_t paramSize = -1;
        int32_t nativeFunction = -1;
    };

    struct UEnumSchema
    {
        int32_t names = -1;
        int32_t cppForm = -1;
        int32_t flags = -1;
        int32_t underlyingType = -1;
    };

    struct PropertySchema
    {
        int32_t arrayDim = -1;
        int32_t elementSize = -1;
        int32_t propertyFlags = -1;
        int32_t offsetInternal = -1;
        int32_t baseSize = -1;
    };

    struct PropertySubtypesSchema
    {
        int32_t byteEnum = -1;
        int32_t boolBase = -1;
        int32_t objectClass = -1;
        int32_t classMetaClass = -1;
        int32_t structType = -1;
        int32_t arrayInner = -1;
        int32_t delegateSignature = -1;
        int32_t mapBase = -1;
        int32_t setElement = -1;
        int32_t enumBase = -1;
        int32_t fieldPathClass = -1;
        int32_t optionalValue = -1;
    };

    struct SchemaValidation
    {
        bool uobject = false;
        bool fname = false;
        bool fields = false;
        bool structs = false;
        bool properties = false;
        bool functions = false;
        bool enums = false;
        std::string familyEvidence;
        std::string failure;
    };

    struct EngineSchema
    {
        EngineFamily family = EngineFamily::Unknown;
        EngineFeatures features;
        FNameSchema fname;
        UObjectSchema uobject;
        UFieldSchema ufield;
        FFieldSchema ffield;
        FFieldClassSchema ffieldClass;
        UStructSchema ustruct;
        UClassSchema uclass;
        UFunctionSchema ufunction;
        UEnumSchema uenum;
        PropertySchema property;
        PropertySubtypesSchema propertySubtypes;
        SchemaValidation validation;

        [[nodiscard]] bool IsReadyForReflection() const;
    };
} // namespace anduefker::ue
