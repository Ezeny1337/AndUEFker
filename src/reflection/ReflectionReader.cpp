#include "anduefker/reflection/ReflectionReader.hpp"

#include <algorithm>
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
        if (name == "BoolProperty")
            return PropertyKind::Bool;
        if (name == "ByteProperty")
            return PropertyKind::Byte;
        if (name == "Int8Property")
            return PropertyKind::Int8;
        if (name == "Int16Property")
            return PropertyKind::Int16;
        if (name == "IntProperty" || name == "Int32Property")
            return PropertyKind::Int32;
        if (name == "Int64Property")
            return PropertyKind::Int64;
        if (name == "UInt16Property")
            return PropertyKind::UInt16;
        if (name == "UInt32Property")
            return PropertyKind::UInt32;
        if (name == "UInt64Property")
            return PropertyKind::UInt64;
        if (name == "FloatProperty")
            return PropertyKind::Float;
        if (name == "DoubleProperty")
            return PropertyKind::Double;
        if (name == "NameProperty")
            return PropertyKind::Name;
        if (name == "StrProperty")
            return PropertyKind::String;
        if (name == "TextProperty")
            return PropertyKind::Text;
        if (name == "ObjectProperty" || name == "ObjectPtrProperty")
            return PropertyKind::Object;
        if (name == "SoftObjectProperty")
            return PropertyKind::SoftObject;
        if (name == "WeakObjectProperty")
            return PropertyKind::WeakObject;
        if (name == "LazyObjectProperty")
            return PropertyKind::LazyObject;
        if (name == "ClassProperty")
            return PropertyKind::Class;
        if (name == "SoftClassProperty")
            return PropertyKind::SoftClass;
        if (name == "StructProperty")
            return PropertyKind::Struct;
        if (name == "EnumProperty")
            return PropertyKind::Enum;
        if (name == "ArrayProperty")
            return PropertyKind::Array;
        if (name == "SetProperty")
            return PropertyKind::Set;
        if (name == "MapProperty")
            return PropertyKind::Map;
        if (name == "InterfaceProperty")
            return PropertyKind::Interface;
        if (name == "DelegateProperty")
            return PropertyKind::Delegate;
        if (name == "MulticastDelegateProperty" || name == "MulticastInlineDelegateProperty" ||
            name == "MulticastSparseDelegateProperty")
            return PropertyKind::MulticastDelegate;
        if (name == "FieldPathProperty")
            return PropertyKind::FieldPath;
        if (name == "OptionalProperty")
            return PropertyKind::Optional;
        if (name == "Utf8StrProperty" || name == "AnsiStrProperty")
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
        std::unordered_set<uintptr_t> visited;
        std::unordered_map<int32_t, uint8_t> boolMasks;
        std::unordered_map<int32_t, int32_t> boolStorageEnds;
        uintptr_t current = first;
        int64_t cursor = 0;
        while (current != 0 && visited.insert(current).second && visited.size() <= 65536)
        {
            const auto property = ReadProperty(current, 0, stats);
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
            const auto field = objects_.Field(current);
            if (!field)
                break;
            current = field->nextAddress;
        }
    }

    void ReflectionReader::ReadFunctionParameters(uintptr_t first, FunctionIR &function, ReflectionStats &stats) const
    {
        std::unordered_set<uintptr_t> visited;
        std::unordered_map<int32_t, uint8_t> boolMasks;
        std::unordered_map<int32_t, int32_t> boolStorageEnds;
        uintptr_t current = first;
        int64_t cursor = 0;
        uint32_t derivedParameterCount = 0;
        int64_t derivedParameterEnd = 0;
        bool sawProperty = false;
        while (current != 0 && visited.insert(current).second && visited.size() <= 65536)
        {
            const auto property = ReadProperty(current, 0, stats);
            if (!property)
            {
                ++stats.failures;
                break;
            }
            sawProperty = true;
            const bool isParameter = (property->flags & 0x00000080ull) != 0;
            if (isParameter)
            {
                function.parameters.push_back(*property);
                ++derivedParameterCount;
            }
            if (!isParameter)
            {
                const auto field = objects_.Field(current);
                if (!field)
                {
                    ++stats.failures;
                    break;
                }
                current = field->nextAddress;
                continue;
            }
            const int64_t total = static_cast<int64_t>(property->elementSize) * property->arrayDim;
            bool conflict = property->offset < 0 || total <= 0 ||
                            static_cast<int64_t>(property->offset) + total < property->offset;
            if (isParameter && !conflict)
                derivedParameterEnd = std::max(derivedParameterEnd, static_cast<int64_t>(property->offset) + total);
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
            const auto field = objects_.Field(current);
            if (!field)
            {
                ++stats.failures;
                break;
            }
            current = field->nextAddress;
        }

        // 即使运行时 Schema 探测流程必须隐匿 UFunction::NumParms 与 ParmsSize 的原始偏移量
        // 但它们仍可从同一个 CPF_Parm 链中推导得出
        // 这能在无需为 runtime.json 伪造/硬编码偏移量的前提下，确保生成的参数布局依然可用
        if (sawProperty && derivedParameterCount <= 0xFFu && derivedParameterEnd <= 0xFFFF)
        {
            function.numParams = static_cast<uint8_t>(derivedParameterCount);
            function.paramSize = static_cast<uint16_t>(derivedParameterEnd);
        }
    }

    void ReflectionReader::ReadFunctions(uintptr_t first, TypeIR &type, ReflectionStats &stats) const
    {
        std::unordered_set<uintptr_t> visited;
        uintptr_t current = first;
        while (current != 0 && visited.insert(current).second && visited.size() <= 65536)
        {
            const auto field = objects_.UField(current);
            if (!field)
            {
                ++stats.failures;
                break;
            }
            if (field->className == "Function")
            {
                FunctionIR function;
                function.address = current;
                function.name = field->name;
                function.fullName = field->className + " " + field->name;
                if (schema_.ufunction.functionFlags >= 0)
                {
                    const auto address = Add(current, schema_.ufunction.functionFlags);
                    uint32_t flags = 0;
                    if (address && memory_.Read(*address, flags))
                        function.flags = flags;
                }
                if (schema_.ufunction.numParams >= 0)
                {
                    const auto address = Add(current, schema_.ufunction.numParams);
                    uint8_t numParams = 0;
                    if (address && memory_.Read(*address, numParams))
                        function.numParams = numParams;
                }
                if (schema_.ufunction.paramSize >= 0)
                {
                    const auto address = Add(current, schema_.ufunction.paramSize);
                    uint16_t paramSize = 0;
                    if (address && memory_.Read(*address, paramSize))
                        function.paramSize = paramSize;
                }
                if (schema_.ufunction.nativeFunction >= 0)
                {
                    uintptr_t native = 0;
                    const auto address = Add(current, schema_.ufunction.nativeFunction);
                    if (address && memory_.Read(*address, native) && native >= moduleBase_ && native < moduleEnd_)
                        function.nativeRva = native - moduleBase_;
                }
                const auto parameters = objects_.StructProperties(current);
                if (parameters)
                    ReadFunctionParameters(*parameters, function, stats);
                type.functions.push_back(std::move(function));
                ++stats.parsedFunctions;
            }
            current = field->nextAddress;
        }
    }

    std::optional<uintptr_t> ReflectionReader::FindPackage(uintptr_t object) const
    {
        std::unordered_set<uintptr_t> visited;
        uintptr_t current = object;
        uintptr_t last = object;
        for (size_t depth = 0; depth < 64 && current != 0; ++depth)
        {
            if (!visited.insert(current).second)
                return std::nullopt;
            const auto outer = objects_.Outer(current);
            if (!outer)
                return std::nullopt;
            if (*outer == 0)
                return last;
            last = *outer;
            current = *outer;
        }
        return std::nullopt;
    }

    std::optional<TypeIR> ReflectionReader::ReadType(uintptr_t object, TypeKind kind, ReflectionIR &ir) const
    {
        const auto name = objects_.Name(object);
        const auto className = objects_.ClassName(object);
        if (!name || !className)
            return std::nullopt;
        const auto size = objects_.StructSize(object);
        const auto super = objects_.StructSuper(object);
        if (!size || *size < 0)
            return std::nullopt;

        TypeIR type;
        type.address = object;
        const auto package = FindPackage(object);
        type.packageAddress = package.value_or(0);
        type.superAddress = super.value_or(0);
        type.kind = kind;
        type.name = *name;
        const auto fullName = objects_.FullName(object);
        type.fullName = fullName ? *fullName : (*className + " " + *name);
        type.size = *size;
        const auto properties = objects_.StructProperties(object);
        if (properties)
            ReadProperties(*properties, type, ir.stats);
        const auto children = objects_.StructChildren(object);
        if (children)
            ReadFunctions(*children, type, ir.stats);
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
                const auto package = FindPackage(*object);
                enumeration.packageAddress = package.value_or(0);
                enumeration.name = *name;
                const auto fullName = objects_.FullName(*object);
                enumeration.fullName = fullName ? *fullName : ("Enum " + *name);
                if (schema_.uenum.cppForm >= 0)
                {
                    uint8_t value = 0;
                    const auto address = Add(*object, schema_.uenum.cppForm);
                    if (address && memory_.Read(*address, value))
                        enumeration.cppForm = value;
                }
                if (schema_.uenum.flags >= 0)
                {
                    uint8_t value = 0;
                    const auto address = Add(*object, schema_.uenum.flags);
                    if (address && memory_.Read(*address, value))
                        enumeration.flags = value;
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

        const auto packageIndexFor = [&](uintptr_t packageAddress) -> size_t
        {
            const auto existing = result.packageIndex.find(packageAddress);
            if (existing != result.packageIndex.end())
                return existing->second;

            PackageIR package;
            package.address = packageAddress;
            const auto packageName = objects_.Name(packageAddress);
            package.name = packageName ? *packageName : "<unknown>";
            const size_t index = result.packages.size();
            result.packages.push_back(std::move(package));
            result.packageIndex.emplace(packageAddress, index);
            return index;
        };
        for (size_t index = 0; index < result.types.size(); ++index)
        {
            if (result.types[index].packageAddress != 0)
                result.packages[packageIndexFor(result.types[index].packageAddress)].types.push_back(index);
        }
        for (size_t index = 0; index < result.enums.size(); ++index)
        {
            if (result.enums[index].packageAddress != 0)
                result.packages[packageIndexFor(result.enums[index].packageAddress)].enums.push_back(index);
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
        else if (result.stats.failures != 0 || result.stats.unknownProperties != 0 || result.stats.unresolvedTypeDetails != 0)
            result.status = ParseStatus::Partial;
        else
            result.status = ParseStatus::Complete;
        return result;
    }
} // namespace anduefker::reflection
