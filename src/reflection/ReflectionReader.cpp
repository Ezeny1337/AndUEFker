#include "anduefker/reflection/ReflectionReader.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace anduefker::reflection
{
    namespace
    {
        std::optional<uintptr_t> Add(uintptr_t base, int32_t offset)
        {
            if (offset < 0 || base > UINTPTR_MAX - static_cast<uintptr_t>(offset))
                return std::nullopt;
            return base + static_cast<uintptr_t>(offset);
        }
    } // namespace

    ReflectionReader::ReflectionReader(const IMemorySource &memory,
                                       const RuntimeBinding &binding,
                                       const EngineSchema &schema,
                                       uintptr_t moduleBase,
                                       uintptr_t moduleEnd)
        : memory_(memory),
          schema_(schema),
          moduleBase_(moduleBase),
          moduleEnd_(moduleEnd),
          objects_(memory, binding, schema)
    {
    }

    PropertyKind ReflectionReader::PropertyKindFromName(const std::string &name) const
    {
        const std::string normalized = NormalizeRuntimeFieldName(name);
        if (normalized == "BoolProperty")
            return PropertyKind::Bool;
        if (normalized == "ByteProperty")
            return PropertyKind::Byte;
        if (normalized == "Int8Property")
            return PropertyKind::Int8;
        if (normalized == "Int16Property")
            return PropertyKind::Int16;
        if (normalized == "IntProperty" || normalized == "Int32Property")
            return PropertyKind::Int32;
        if (normalized == "Int64Property")
            return PropertyKind::Int64;
        if (normalized == "UInt16Property")
            return PropertyKind::UInt16;
        if (normalized == "UInt32Property")
            return PropertyKind::UInt32;
        if (normalized == "UInt64Property")
            return PropertyKind::UInt64;
        if (normalized == "FloatProperty")
            return PropertyKind::Float;
        if (normalized == "DoubleProperty")
            return PropertyKind::Double;
        if (normalized == "NameProperty")
            return PropertyKind::Name;
        if (normalized == "StrProperty")
            return PropertyKind::String;
        if (normalized == "TextProperty")
            return PropertyKind::Text;
        if (normalized == "ObjectProperty" || normalized == "ObjectPtrProperty")
            return PropertyKind::Object;
        if (normalized == "SoftObjectProperty")
            return PropertyKind::SoftObject;
        if (normalized == "WeakObjectProperty")
            return PropertyKind::WeakObject;
        if (normalized == "LazyObjectProperty")
            return PropertyKind::LazyObject;
        if (normalized == "ClassProperty")
            return PropertyKind::Class;
        if (normalized == "SoftClassProperty")
            return PropertyKind::SoftClass;
        if (normalized == "StructProperty")
            return PropertyKind::Struct;
        if (normalized == "EnumProperty")
            return PropertyKind::Enum;
        if (normalized == "ArrayProperty")
            return PropertyKind::Array;
        if (normalized == "SetProperty")
            return PropertyKind::Set;
        if (normalized == "MapProperty")
            return PropertyKind::Map;
        if (normalized == "InterfaceProperty")
            return PropertyKind::Interface;
        if (normalized == "DelegateProperty")
            return PropertyKind::Delegate;
        if (normalized == "MulticastDelegateProperty" || normalized == "MulticastInlineDelegateProperty" ||
            normalized == "MulticastSparseDelegateProperty")
            return PropertyKind::MulticastDelegate;
        if (normalized == "FieldPathProperty")
            return PropertyKind::FieldPath;
        if (normalized == "OptionalProperty")
            return PropertyKind::Optional;
        if (normalized == "Utf8StrProperty" || normalized == "AnsiStrProperty")
            return PropertyKind::String;
        return PropertyKind::Unknown;
    }

    std::optional<PropertyIR> ReflectionReader::ReadProperty(uintptr_t field, size_t depth, ReflectionStats &stats) const
    {
        if (depth > 32)
            return std::nullopt;
        const auto metadata = objects_.Property(field);
        if (!metadata)
            return std::nullopt;

        PropertyIR result;
        result.address = field;
        result.name = metadata->name;
        result.reflectedClass = metadata->className;
        result.offset = metadata->offset;
        result.elementSize = metadata->elementSize;
        result.arrayDim = metadata->arrayDim;
        result.flags = metadata->flags;
        result.isParameter = (result.flags & 0x80u) != 0;
        result.isReturnParameter = (result.flags & 0x400u) != 0;
        result.isOutParameter = (result.flags & 0x100u) != 0;
        result.isReferenceParameter = (result.flags & 0x08000000u) != 0;
        result.isConstParameter = (result.flags & 0x2u) != 0;
        result.type.kind = PropertyKindFromName(result.reflectedClass);
        result.type.reflectedClass = result.reflectedClass;
        result.type.elementSize = result.elementSize;
        result.typeDetailsResolved = result.type.kind != PropertyKind::Unknown;

        const auto readReference = [&](int32_t offset) -> uintptr_t
        {
            const auto address = Add(field, offset);
            if (!address)
                return 0;
            uintptr_t value = 0;
            return memory_.Read(*address, value) ? value : 0;
        };
        const auto readNestedType = [&](uintptr_t address) -> std::shared_ptr<TypeReferenceIR>
        {
            if (address == 0)
                return {};
            const auto metadata = objects_.Property(address);
            if (!metadata)
                return {};
            auto nested = std::make_shared<TypeReferenceIR>();
            nested->kind = PropertyKindFromName(metadata->className);
            nested->reflectedClass = metadata->className;
            nested->elementSize = metadata->elementSize;
            nested->referencedObject = metadata->referencedAddress;
            nested->secondaryObject = metadata->secondaryAddress;
            return nested;
        };

        switch (result.type.kind)
        {
        case PropertyKind::Object:
        case PropertyKind::SoftObject:
        case PropertyKind::WeakObject:
        case PropertyKind::LazyObject:
        case PropertyKind::Interface:
            if (schema_.propertySubtypes.objectClass >= 0)
                result.type.referencedObject = readReference(schema_.propertySubtypes.objectClass);
            break;
        case PropertyKind::Class:
        case PropertyKind::SoftClass:
            if (schema_.propertySubtypes.classMetaClass >= 0)
                result.type.referencedObject = readReference(schema_.propertySubtypes.classMetaClass);
            break;
        case PropertyKind::Struct:
            if (schema_.propertySubtypes.structType >= 0)
                result.type.referencedObject = readReference(schema_.propertySubtypes.structType);
            break;
        case PropertyKind::Byte:
            if (schema_.propertySubtypes.byteEnum >= 0)
                result.type.referencedObject = readReference(schema_.propertySubtypes.byteEnum);
            break;
        case PropertyKind::Array:
            if (schema_.propertySubtypes.arrayInner >= 0)
            {
                result.type.referencedObject = readReference(schema_.propertySubtypes.arrayInner);
                result.type.inner = readNestedType(result.type.referencedObject);
            }
            break;
        case PropertyKind::Set:
            if (schema_.propertySubtypes.setElement >= 0)
            {
                result.type.referencedObject = readReference(schema_.propertySubtypes.setElement);
                result.type.inner = readNestedType(result.type.referencedObject);
            }
            break;
        case PropertyKind::Map:
            if (schema_.propertySubtypes.mapBase >= 0)
            {
                result.type.referencedObject = readReference(schema_.propertySubtypes.mapBase);
                result.type.secondaryObject = readReference(schema_.propertySubtypes.mapBase + static_cast<int32_t>(sizeof(uintptr_t)));
                result.type.key = readNestedType(result.type.referencedObject);
                result.type.value = readNestedType(result.type.secondaryObject);
            }
            break;
        case PropertyKind::Enum:
            if (schema_.propertySubtypes.enumBase >= 0)
            {
                result.type.referencedObject = readReference(schema_.propertySubtypes.enumBase);
                result.type.secondaryObject = readReference(schema_.propertySubtypes.enumBase + static_cast<int32_t>(sizeof(uintptr_t)));
                result.type.inner = readNestedType(result.type.referencedObject);
            }
            break;
        case PropertyKind::Optional:
            if (schema_.propertySubtypes.optionalValue >= 0)
            {
                result.type.referencedObject = readReference(schema_.propertySubtypes.optionalValue);
                result.type.inner = readNestedType(result.type.referencedObject);
            }
            break;
        case PropertyKind::Delegate:
        case PropertyKind::MulticastDelegate:
            if (schema_.propertySubtypes.delegateSignature >= 0)
                result.type.referencedObject = readReference(schema_.propertySubtypes.delegateSignature);
            break;
        default:
            break;
        }

        if (result.type.kind == PropertyKind::Bool && schema_.propertySubtypes.boolBase >= 0)
        {
            uint8_t values[4]{};
            const auto address = Add(field, schema_.propertySubtypes.boolBase);
            if (address && memory_.ReadBytes(*address, values, sizeof(values)).Ok())
                result.boolean = {values[0], values[1], values[2], values[3]};
        }

        switch (result.type.kind)
        {
        case PropertyKind::Object:
        case PropertyKind::Class:
        case PropertyKind::SoftObject:
        case PropertyKind::SoftClass:
        case PropertyKind::WeakObject:
        case PropertyKind::LazyObject:
        case PropertyKind::Struct:
        case PropertyKind::Interface:
            result.typeDetailsResolved = result.type.referencedObject != 0;
            break;
        case PropertyKind::Array:
        case PropertyKind::Set:
        case PropertyKind::Map:
        case PropertyKind::Enum:
            result.typeDetailsResolved = result.type.referencedObject != 0 &&
                                         (result.type.kind != PropertyKind::Map || result.type.secondaryObject != 0) &&
                                         (result.type.kind != PropertyKind::Enum || result.type.secondaryObject != 0);
            break;
        case PropertyKind::Delegate:
        case PropertyKind::MulticastDelegate:
            result.typeDetailsResolved = result.type.referencedObject != 0;
            break;
        case PropertyKind::Optional:
            result.typeDetailsResolved = result.type.referencedObject != 0 && result.type.inner != nullptr;
            break;
        default:
            break;
        }

        if (result.type.kind == PropertyKind::Unknown)
            ++stats.unknownProperties;
        if (!result.typeDetailsResolved)
            ++stats.unresolvedTypeDetails;
        ++stats.parsedProperties;
        return result;
    }

    void ReflectionReader::ReadProperties(uintptr_t first, TypeIR &type, ReflectionStats &stats) const
    {
        const FieldChainResult chain = objects_.FieldsWithStatus(first, 65536);
        if (!chain.Complete())
        {
            ++stats.failures;
            type.layoutConflicts.push_back("property field chain status=" + std::to_string(static_cast<int>(chain.status)));
            return;
        }
        std::unordered_map<int32_t, uint8_t> boolMasks;
        std::unordered_map<int32_t, int32_t> boolStorageEnds;
        int64_t cursor = 0;
        for (const FieldMetadata &field : chain.fields)
        {
            if (!IsPropertyFieldKind(field.kind))
            {
                continue;
            }
            if (schema_.features.useFProperty &&
                (!field.ownerIsUObject || field.ownerAddress != type.address))
            {
                ++stats.failures;
                type.layoutConflicts.push_back("property owner mismatch: property=" + field.name);
                continue;
            }
            const auto property = ReadProperty(field.address, 0, stats);
            if (property)
                type.properties.push_back(*property);
            else
                ++stats.failures;
            if (property)
            {
                const int64_t total = static_cast<int64_t>(property->elementSize) * property->arrayDim;
                bool conflict = property->offset < 0 || total <= 0 ||
                                static_cast<int64_t>(property->offset) + total < property->offset;
                if (!conflict && property->type.kind == PropertyKind::Bool &&
                    property->boolean.fieldSize > 0 && property->boolean.fieldSize <= 8 &&
                    property->boolean.byteOffset < property->boolean.fieldSize &&
                    property->boolean.byteMask != 0 && property->boolean.fieldMask != 0)
                {
                    const int32_t storageOffset = property->offset + property->boolean.byteOffset;
                    const int32_t storageEnd = storageOffset + property->boolean.fieldSize;
                    if (storageEnd < storageOffset)
                    {
                        conflict = true;
                    }
                    else if (property->boolean.fieldSize == 1)
                    {
                        const uint8_t mask = property->boolean.fieldMask;
                        const auto existing = boolMasks.find(storageOffset);
                        if (property->offset < cursor && existing == boolMasks.end())
                            conflict = true;
                        else if (existing != boolMasks.end() && (existing->second & mask) != 0)
                            conflict = true;
                        else
                            boolMasks[storageOffset] |= mask;
                    }
                    else
                    {
                        const auto existingEnd = boolStorageEnds.find(storageOffset);
                        if (property->offset < cursor && existingEnd == boolStorageEnds.end())
                            conflict = true;
                        else if (existingEnd != boolStorageEnds.end() && existingEnd->second > storageOffset)
                            conflict = true;
                        else
                            boolStorageEnds[storageOffset] = storageEnd;
                    }
                    if (!conflict)
                        cursor = std::max<int64_t>(cursor, storageEnd);
                }
                else if (!conflict)
                {
                    conflict = property->offset < cursor;
                }
                if (conflict)
                {
                    type.layoutConflicts.push_back("property=" + property->name +
                                                   " offset=" + std::to_string(property->offset) +
                                                   " element_size=" + std::to_string(property->elementSize) +
                                                   " array_dim=" + std::to_string(property->arrayDim));
                    ++stats.layoutConflicts;
                }
                else
                {
                    cursor = static_cast<int64_t>(property->offset) + total;
                }
            }
        }
    }

    void ReflectionReader::ReadFunctionParameters(uintptr_t first, FunctionIR &function, ReflectionStats &stats) const
    {
        std::vector<PropertyMetadata> properties;
        std::vector<PropertyIR> parsedProperties;
        const FieldChainResult chain = objects_.FieldsWithStatus(first, 65536);
        if (!chain.Complete())
        {
            ++stats.failures;
            function.parameterSemanticsValid = false;
            function.layoutConflicts.push_back("parameter field chain status=" + std::to_string(static_cast<int>(chain.status)));
            return;
        }

        std::unordered_map<int32_t, uint8_t> boolMasks;
        std::unordered_map<int32_t, int32_t> boolStorageEnds;
        int64_t cursor = 0;
        for (const FieldMetadata &field : chain.fields)
        {
            if (!IsPropertyFieldKind(field.kind))
            {
                continue;
            }
            if (schema_.features.useFProperty &&
                (!field.ownerIsUObject || field.ownerAddress != function.address))
            {
                ++stats.failures;
                function.layoutConflicts.push_back("parameter owner mismatch: property=" + field.name);
                continue;
            }
            const auto metadata = objects_.Property(field.address);
            const auto property = ReadProperty(field.address, 0, stats);
            if (!metadata || !property)
            {
                ++stats.failures;
                break;
            }
            properties.push_back(*metadata);
            parsedProperties.push_back(*property);
            const bool isParameter = (property->flags & 0x00000080ull) != 0;
            if (!isParameter)
                continue;
            const int64_t total = static_cast<int64_t>(property->elementSize) * property->arrayDim;
            bool conflict = property->offset < 0 || total <= 0 ||
                            static_cast<int64_t>(property->offset) + total < property->offset;
            if (!conflict && property->type.kind == PropertyKind::Bool &&
                property->boolean.fieldSize > 0 && property->boolean.fieldSize <= 8 &&
                property->boolean.byteOffset < property->boolean.fieldSize &&
                property->boolean.byteMask != 0 && property->boolean.fieldMask != 0)
            {
                const int32_t storageOffset = property->offset + property->boolean.byteOffset;
                const int32_t storageEnd = storageOffset + property->boolean.fieldSize;
                if (storageEnd < storageOffset)
                {
                    conflict = true;
                }
                else if (property->boolean.fieldSize == 1)
                {
                    const uint8_t mask = property->boolean.fieldMask;
                    const auto existing = boolMasks.find(storageOffset);
                    if (property->offset < cursor && existing == boolMasks.end())
                        conflict = true;
                    else if (existing != boolMasks.end() && (existing->second & mask) != 0)
                        conflict = true;
                    else
                        boolMasks[storageOffset] |= mask;
                }
                else
                {
                    const auto existingEnd = boolStorageEnds.find(storageOffset);
                    if (property->offset < cursor && existingEnd == boolStorageEnds.end())
                        conflict = true;
                    else if (existingEnd != boolStorageEnds.end() && existingEnd->second > storageOffset)
                        conflict = true;
                    else
                        boolStorageEnds[storageOffset] = storageEnd;
                }
                if (!conflict)
                    cursor = std::max<int64_t>(cursor, storageEnd);
            }
            else if (!conflict)
            {
                conflict = property->offset < cursor;
            }
            if (conflict)
            {
                function.layoutConflicts.push_back("parameter=" + property->name +
                                                   " offset=" + std::to_string(property->offset) +
                                                   " element_size=" + std::to_string(property->elementSize) +
                                                   " array_dim=" + std::to_string(property->arrayDim));
                ++stats.layoutConflicts;
            }
            else
            {
                cursor = static_cast<int64_t>(property->offset) + total;
            }
        }

        const FunctionParameterSummary summary = AnalyzeFunctionParameters(
            properties, function.flags, schema_.features.functionDefaultsContinueAfterInitializer);
        function.parameters.clear();
        const size_t scannedPropertyCount = std::min(summary.scannedPropertyCount, parsedProperties.size());
        for (size_t index = 0; index < scannedPropertyCount; ++index)
        {
            if ((parsedProperties[index].flags & ::anduefker::ue::kCPFParm) != 0)
                function.parameters.push_back(parsedProperties[index]);
        }
        function.derivedNumParams = summary.count;
        function.derivedParamSize = summary.paramEnd;
        function.defaultInitializerCount = summary.defaultInitializerCount;
        function.parameterSemanticsValid = summary.valid;
        const uint16_t expectedReturnOffset = summary.returnOffset >= 0
                                                  ? static_cast<uint16_t>(summary.returnOffset)
                                                  : std::numeric_limits<uint16_t>::max();
        function.parameterSemanticsConsistent = summary.valid &&
                                                function.headerNumParams == summary.count &&
                                                function.headerParamSize == summary.paramEnd &&
                                                function.returnValueOffset == expectedReturnOffset;
        if (!function.parameterSemanticsConsistent)
        {
            function.layoutConflicts.push_back("function header parameters do not match derived semantics: header_count=" +
                                               std::to_string(function.headerNumParams) +
                                               " derived_count=" + std::to_string(summary.count) +
                                               " header_size=" + std::to_string(function.headerParamSize) +
                                               " derived_size=" + std::to_string(summary.paramEnd) +
                                               " header_return=" + std::to_string(function.returnValueOffset) +
                                               " derived_return=" + std::to_string(expectedReturnOffset));
            ++stats.layoutConflicts;
        }
    }

    void ReflectionReader::ReadFunctions(uintptr_t first, TypeIR &type, ReflectionStats &stats) const
    {
        const FieldChainResult chain = objects_.UFieldsWithStatus(first, 65536);
        if (!chain.Complete())
        {
            ++stats.failures;
            type.layoutConflicts.push_back("function field chain status=" + std::to_string(static_cast<int>(chain.status)));
            return;
        }
        for (const FieldMetadata &field : chain.fields)
        {
            if (IsFunctionFieldKind(field.kind))
            {
                FunctionIR function;
                function.address = field.address;
                function.name = field.name;
                function.fullName = field.className + " " + field.name;
                const auto readMember = [&](int32_t offset, auto &value)
                {
                    const auto address = Add(field.address, offset);
                    return address && memory_.Read(*address, value);
                };
                if (!readMember(schema_.ufunction.functionFlags, function.flags) ||
                    !readMember(schema_.ufunction.numParams, function.headerNumParams) ||
                    !readMember(schema_.ufunction.paramSize, function.headerParamSize) ||
                    !readMember(schema_.ufunction.returnValueOffset, function.returnValueOffset))
                {
                    ++stats.failures;
                    type.layoutConflicts.push_back("function header unreadable: function=" + field.name);
                    continue;
                }
                function.numParams = function.headerNumParams;
                function.paramSize = function.headerParamSize;
                uintptr_t native = 0;
                if (!readMember(schema_.ufunction.nativeFunction, native))
                {
                    ++stats.failures;
                    function.layoutConflicts.push_back("function native pointer unreadable");
                }
                else if (native >= moduleBase_ && native < moduleEnd_ && memory_.IsExecutable(native, sizeof(uintptr_t)))
                    function.nativeRva = native - moduleBase_;
                const auto parameters = objects_.StructProperties(field.address);
                if (parameters)
                    ReadFunctionParameters(*parameters, function, stats);
                else
                {
                    ++stats.failures;
                    function.layoutConflicts.push_back("function parameter chain root unreadable");
                }
                type.functions.push_back(std::move(function));
                ++stats.parsedFunctions;
            }
        }
    }

    std::optional<TypeIR> ReflectionReader::ReadType(uintptr_t object, TypeKind kind, ReflectionIR &ir) const
    {
        const auto name = objects_.Name(object);
        const auto className = objects_.ClassName(object);
        if (!name || !className)
            return std::nullopt;
        const auto size = objects_.StructSize(object);
        const auto super = objects_.StructSuper(object);
        if (!size || *size < 0 || !super)
            return std::nullopt;

        TypeIR type;
        type.address = object;
        type.superAddress = *super;
        type.kind = kind;
        type.name = *name;
        const auto fullName = objects_.FullName(object);
        type.fullName = fullName ? *fullName : (*className + " " + *name);
        type.size = *size;
        const auto properties = objects_.StructProperties(object);
        if (properties)
            ReadProperties(*properties, type, ir.stats);
        else
        {
            ++ir.stats.failures;
            type.layoutConflicts.push_back("property chain root unreadable");
        }
        const auto children = objects_.StructChildren(object);
        if (children)
            ReadFunctions(*children, type, ir.stats);
        else
        {
            ++ir.stats.failures;
            type.layoutConflicts.push_back("function chain root unreadable");
        }
        return type;
    }

    ReflectionIR ReflectionReader::Read()
    {
        ReflectionIR result;
        if (!objects_.Initialize())
        {
            result.status = ParseStatus::Failed;
            ++result.stats.failures;
            return result;
        }
        result.stats.objectSlots = objects_.Count();

        std::unordered_set<uintptr_t> seenTypes;
        for (int32_t index = 0; index < objects_.Count(); ++index)
        {
            const auto object = objects_.ObjectAt(index);
            if (!object)
            {
                ++result.stats.skippedObjects;
                continue;
            }
            ++result.stats.validObjects;
            const auto className = objects_.ClassName(*object);
            if (!className)
                continue;
            if (*className != "Enum" && *className != "Class" && *className != "ScriptStruct")
                continue;
            const auto objectFlags = objects_.Flags(*object);
            if (!objectFlags)
            {
                ++result.stats.failures;
                continue;
            }
            if ((*objectFlags & ::anduefker::ue::kRFClassDefaultObject) != 0)
            {
                ++result.stats.skippedObjects;
                continue;
            }
            if ((*objectFlags & ::anduefker::ue::kRFIncompleteLoad) != 0)
            {
                ++result.stats.skippedObjects;
                ++result.stats.failures;
                result.diagnostics.push_back("reflection definition is not fully loaded: address=" +
                                             std::to_string(*object));
                continue;
            }
            if (*className == "Enum")
            {
                const auto name = objects_.Name(*object);
                if (!name)
                {
                    ++result.stats.failures;
                    continue;
                }
                EnumIR enumeration;
                enumeration.address = *object;
                enumeration.name = *name;
                const auto fullName = objects_.FullName(*object);
                enumeration.fullName = fullName ? *fullName : ("Enum " + *name);
                if (schema_.uenum.cppForm >= 0)
                {
                    const auto address = Add(*object, schema_.uenum.cppForm);
                    if (address)
                    {
                        if (schema_.features.enumCppFormIsByte)
                        {
                            uint8_t value = 0;
                            if (memory_.Read(*address, value))
                                enumeration.cppForm = value;
                        }
                        else
                        {
                            uint32_t value = 0;
                            if (memory_.Read(*address, value))
                                enumeration.cppForm = static_cast<uint8_t>(value);
                        }
                    }
                }
                if (schema_.uenum.flags >= 0)
                {
                    const auto address = Add(*object, schema_.uenum.flags);
                    if (address)
                    {
                        if (schema_.features.enumFlagsIsByte)
                        {
                            uint8_t value = 0;
                            if (memory_.Read(*address, value))
                                enumeration.flags = value;
                        }
                        else
                        {
                            uint32_t value = 0;
                            if (memory_.Read(*address, value))
                                enumeration.flags = static_cast<uint8_t>(value);
                        }
                    }
                }
                for (const EnumValueMetadata &value : objects_.EnumValues(*object))
                    enumeration.values.push_back(EnumValueIR{value.name, value.value});
                result.enums.push_back(std::move(enumeration));
                ++result.stats.parsedEnums;
                continue;
            }
            if (*className != "Class" && *className != "ScriptStruct")
                continue;
            if (!seenTypes.insert(*object).second)
                continue;

            const auto type = ReadType(*object, *className == "Class" ? TypeKind::Class : TypeKind::Struct, result);
            if (!type)
            {
                ++result.stats.failures;
                continue;
            }
            result.types.push_back(*type);
            ++result.stats.parsedTypes;
        }

        std::unordered_map<uintptr_t, size_t> enumByAddress;
        for (size_t index = 0; index < result.enums.size(); ++index)
            enumByAddress.emplace(result.enums[index].address, index);
        for (const TypeIR &type : result.types)
        {
            for (const PropertyIR &property : type.properties)
            {
                if (property.type.kind != PropertyKind::Enum || property.type.secondaryObject == 0 ||
                    property.type.referencedObject == 0)
                    continue;
                const auto enumIndex = enumByAddress.find(property.type.secondaryObject);
                if (enumIndex == enumByAddress.end() || result.enums[enumIndex->second].underlyingType != EnumUnderlyingType::Unknown)
                    continue;
                const auto underlying = objects_.Field(property.type.referencedObject);
                if (!underlying)
                    continue;
                if (underlying->className == "Int8Property")
                    result.enums[enumIndex->second].underlyingType = EnumUnderlyingType::Int8;
                else if (underlying->className == "ByteProperty")
                    result.enums[enumIndex->second].underlyingType = EnumUnderlyingType::UInt8;
                else if (underlying->className == "Int16Property")
                    result.enums[enumIndex->second].underlyingType = EnumUnderlyingType::Int16;
                else if (underlying->className == "UInt16Property")
                    result.enums[enumIndex->second].underlyingType = EnumUnderlyingType::UInt16;
                else if (underlying->className == "IntProperty")
                    result.enums[enumIndex->second].underlyingType = EnumUnderlyingType::Int32;
                else if (underlying->className == "UInt32Property")
                    result.enums[enumIndex->second].underlyingType = EnumUnderlyingType::UInt32;
                else if (underlying->className == "Int64Property")
                    result.enums[enumIndex->second].underlyingType = EnumUnderlyingType::Int64;
                else if (underlying->className == "UInt64Property")
                    result.enums[enumIndex->second].underlyingType = EnumUnderlyingType::UInt64;
            }
        }

        if (result.stats.parsedTypes == 0)
            result.status = ParseStatus::Failed;
        else if (result.stats.failures != 0 || result.stats.unknownProperties != 0 ||
                 result.stats.unresolvedTypeDetails != 0 || result.stats.layoutConflicts != 0)
            result.status = ParseStatus::Partial;
        else
            result.status = ParseStatus::Complete;
        return result;
    }
} // namespace anduefker::reflection
