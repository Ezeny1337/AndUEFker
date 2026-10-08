#include "anduefker/reflection/ReflectionReader.hpp"
#include "anduefker/ir/ReflectionLayout.hpp"
#include "anduefker/ue/BoolLayout.hpp"

#include <algorithm>
#include <chrono>
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

    ReflectionReader::ReflectionReader(IMemorySource &memory,
                                       const RuntimeBinding &binding,
                                       const EngineSchema &schema,
                                       uintptr_t moduleBase,
                                       uintptr_t moduleEnd,
                                       std::function<void(const std::string &)> progress)
        : memory_(memory),
          schema_(schema),
          moduleBase_(moduleBase),
          moduleEnd_(moduleEnd),
          objects_(memory_, binding, schema),
          progress_(std::move(progress))
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
        if (normalized == "ObjectProperty" || normalized == "ObjectPtrProperty" || normalized == "ObjectPropertyBase")
            return PropertyKind::Object;
        if (normalized == "SoftObjectProperty")
            return PropertyKind::SoftObject;
        if (normalized == "WeakObjectProperty")
            return PropertyKind::WeakObject;
        if (normalized == "LazyObjectProperty")
            return PropertyKind::LazyObject;
        if (normalized == "ClassProperty" || normalized == "ClassPtrProperty")
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

    void ReflectionReader::RecordPropertyDetail(const PropertyMetadata &metadata, PropertyIR &property,
                                                const std::string &reason, bool headerAvailable) const
    {
        ::anduefker::ir::PropertyDetailDiagnostic diagnostic;
        diagnostic.headerAvailable = headerAvailable;
        diagnostic.address = metadata.address;
        diagnostic.classAddress = metadata.classAddress;
        diagnostic.nextAddress = metadata.nextAddress;
        diagnostic.immediateOwner = metadata.ownerAddress;
        diagnostic.ownerIsUObject = metadata.ownerIsUObject;
        diagnostic.name = metadata.name;
        diagnostic.reflectedClass = metadata.className;
        diagnostic.normalizedClass = metadata.normalizedClassName;
        diagnostic.reason = reason;
        diagnostic.detailsStatus = ::anduefker::ue::PropertyDetailsStatusName(metadata.detailsStatus);
        diagnostic.offset = metadata.offset;
        diagnostic.elementSize = metadata.elementSize;
        diagnostic.arrayDim = metadata.arrayDim;
        diagnostic.flags = metadata.flags;
        diagnostic.referencedAddress = metadata.referencedAddress;
        diagnostic.secondaryAddress = metadata.secondaryAddress;
        diagnostic.boolean = {metadata.boolLayout[0], metadata.boolLayout[1], metadata.boolLayout[2], metadata.boolLayout[3]};
        for (size_t index = 0; index < metadata.detailReadCount; ++index)
        {
            const auto &read = metadata.detailReads[index];
            diagnostic.reads.push_back({read.member, read.offset, read.address, read.rawValue,
                                        static_cast<int32_t>(read.read.error), read.read.requested, read.read.transferred,
                                        ::anduefker::ue::PropertyDetailsStatusName(read.status)});
        }
        diagnostic.referencedClass = "not-observed";
        diagnostic.secondaryClass = "not-observed";
        diagnostic.objectPropertyClassNull = metadata.objectPropertyClassNull;
        diagnostic.objectPropertyClassPointerAddress = metadata.objectPropertyClassPointerAddress;
        // 仅在已完成读取与基本指针检查后补充类型身份；不尝试解码未知对象句柄。
        const auto referenceClass = [&](uintptr_t address, bool secondary) -> std::string
        {
            if (address == 0 || metadata.detailsStatus != PropertyMetadata::DetailsStatus::Complete)
                return "not-observed";
            const auto kind = PropertyKindFromName(metadata.className);
            if (kind == PropertyKind::FieldPath)
            {
                const auto cls = objects_.FieldClass(address);
                return cls ? cls->name : "<invalid-field-class>";
            }
            if (kind == PropertyKind::Map || (!secondary && (kind == PropertyKind::Array || kind == PropertyKind::Set ||
                                                             kind == PropertyKind::Enum || kind == PropertyKind::Optional)))
            {
                const auto field = objects_.Field(address);
                return field ? field->className : "<invalid-field>";
            }
            const auto cls = objects_.ClassName(address);
            return cls ? *cls : "<unreadable>";
        };
        diagnostic.referencedClass = referenceClass(metadata.referencedAddress, false);
        diagnostic.secondaryClass = referenceClass(metadata.secondaryAddress, true);
        property.detailDiagnostics.push_back(std::move(diagnostic));
    }

    TypeReferenceIR ReflectionReader::ReadTypeReference(const PropertyMetadata &metadata, PropertyIR &property,
                                                        ReflectionStats &stats, std::unordered_set<uintptr_t> &path,
                                                        size_t depth, size_t &remaining) const
    {
        TypeReferenceIR result;
        result.kind = PropertyKindFromName(metadata.className);
        result.reflectedClass = metadata.className;
        result.elementSize = metadata.elementSize;
        result.referencedObject = metadata.referencedAddress;
        result.secondaryObject = metadata.secondaryAddress;
        if (depth >= 32 || remaining == 0 || !path.insert(metadata.address).second)
        {
            property.diagnostics.push_back("nested property cycle or traversal limit: address=" + std::to_string(metadata.address));
            RecordPropertyDetail(metadata, property, "cycle-or-traversal-limit");
            ++stats.failures;
            return result;
        }
        --remaining;
        result.detailsResolved = metadata.detailsStatus == PropertyMetadata::DetailsStatus::Complete &&
                                 result.kind != PropertyKind::Unknown && metadata.elementSize > 0;
        if (metadata.detailsStatus != PropertyMetadata::DetailsStatus::Complete)
        {
            // 成员读取失败或必需引用为空时，仅记录证据，不能再以其结果做对象/子属性解引用。
            property.diagnostics.push_back("property details status=" + std::to_string(static_cast<int>(metadata.detailsStatus)) +
                                           " address=" + std::to_string(metadata.address));
            if (metadata.detailsStatus != PropertyMetadata::DetailsStatus::UnsupportedLayout)
                ++stats.failures;
            if (result.kind == PropertyKind::Unknown)
                ++stats.unknownProperties;
            const std::string detailReason = metadata.objectPropertyClassNull
                                                 ? "object-property-class-metadata-null"
                                                 : ::anduefker::ue::PropertyDetailsStatusName(metadata.detailsStatus);
            RecordPropertyDetail(metadata, property, detailReason);
            path.erase(metadata.address);
            return result;
        }
        if (result.kind == PropertyKind::Unknown)
            ++stats.unknownProperties;
        const auto nested = [&](uintptr_t address) -> std::shared_ptr<TypeReferenceIR>
        {
            if (address == 0)
                return {};
            const auto child = objects_.Property(address);
            if (!child)
            {
                ++stats.failures;
                property.diagnostics.push_back("nested property header unreadable: address=" + std::to_string(address));
                PropertyMetadata missing;
                missing.address = address;
                missing.name = "<unreadable>";
                missing.className = "<unreadable>";
                missing.normalizedClassName = "<unreadable>";
                missing.detailsStatus = PropertyMetadata::DetailsStatus::Unreadable;
                RecordPropertyDetail(missing, property, "nested-header-unreadable", false);
                return {};
            }
            if (schema_.features.useFProperty && (child->ownerIsUObject || child->ownerAddress != metadata.address))
            {
                ++stats.failures;
                property.diagnostics.push_back("nested property owner mismatch: address=" + std::to_string(address));
                RecordPropertyDetail(*child, property, "nested-owner-mismatch");
                return {};
            }
            return std::make_shared<TypeReferenceIR>(ReadTypeReference(*child, property, stats, path, depth + 1, remaining));
        };
        const auto objectMatches = [&](uintptr_t address, ::anduefker::ue::DefinitionKind expected)
        {
            if (address == 0)
                return false;
            const auto cls = objects_.Class(address);
            const auto kind = cls ? objects_.DefinitionKindForClass(*cls) : std::nullopt;
            return kind && *kind == expected;
        };
        bool semanticMatch = true;
        switch (result.kind)
        {
        case PropertyKind::Object:
        case PropertyKind::Class:
        case PropertyKind::SoftObject:
        case PropertyKind::SoftClass:
        case PropertyKind::WeakObject:
        case PropertyKind::LazyObject:
        case PropertyKind::Interface:
            semanticMatch = objectMatches(result.referencedObject, ::anduefker::ue::DefinitionKind::Class);
            break;
        case PropertyKind::Struct:
            semanticMatch = objectMatches(result.referencedObject, ::anduefker::ue::DefinitionKind::Struct);
            break;
        case PropertyKind::Byte:
            semanticMatch = result.referencedObject == 0 || objectMatches(result.referencedObject, ::anduefker::ue::DefinitionKind::Enum);
            break;
        case PropertyKind::Array:
        case PropertyKind::Set:
        case PropertyKind::Optional:
            result.inner = nested(result.referencedObject);
            semanticMatch = result.inner && result.inner->detailsResolved;
            break;
        case PropertyKind::Map:
            result.key = nested(result.referencedObject);
            result.value = nested(result.secondaryObject);
            semanticMatch = result.key && result.value && result.key->detailsResolved && result.value->detailsResolved;
            break;
        case PropertyKind::Enum:
            result.inner = nested(result.referencedObject);
            semanticMatch = result.inner && result.inner->detailsResolved && result.inner->elementSize == result.elementSize &&
                            objectMatches(result.secondaryObject, ::anduefker::ue::DefinitionKind::Enum);
            break;
        case PropertyKind::Delegate:
        case PropertyKind::MulticastDelegate:
        {
            const auto cls = objects_.ClassName(result.referencedObject);
            semanticMatch = cls && IsFunctionFieldKind(::anduefker::ue::FieldKindFromRuntimeName(*cls, false));
            break;
        }
        case PropertyKind::Bool:
            semanticMatch = ::anduefker::ue::IsValidBoolLayout(metadata.boolLayout, metadata.elementSize);
            break;
        case PropertyKind::FieldPath:
            semanticMatch = metadata.detailsStatus == PropertyMetadata::DetailsStatus::Complete &&
                            objects_.IsValidFieldClass(result.referencedObject);
            break;
        default:
            break;
        }
        if (!semanticMatch)
        {
            property.diagnostics.push_back("property type semantics unresolved: address=" + std::to_string(metadata.address));
            result.detailsResolved = false;
        }
        if (!result.detailsResolved)
        {
            std::string reason = "semantic-mismatch";
            if (result.kind == PropertyKind::Unknown)
                reason = "unknown-property-kind";
            else if (metadata.elementSize <= 0)
                reason = "invalid-element-size";
            else if (metadata.objectPropertyClassNull)
                reason = "object-property-class-metadata-null";
            RecordPropertyDetail(metadata, property, reason);
        }
        path.erase(metadata.address);
        return result;
    }

    std::optional<PropertyIR> ReflectionReader::ReadProperty(uintptr_t field, ReflectionStats &stats) const
    {
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
        result.boolean = {metadata->boolLayout[0], metadata->boolLayout[1], metadata->boolLayout[2], metadata->boolLayout[3]};
        std::unordered_set<uintptr_t> path;
        size_t remaining = 256;
        result.type = ReadTypeReference(*metadata, result, stats, path, 0, remaining);
        result.typeDetailsResolved = result.type.detailsResolved;
        if (!result.typeDetailsResolved)
        {
            ++stats.unresolvedTypeDetails;
            result.status = ParseStatus::Partial;
        }
        ++stats.parsedProperties;
        return result;
    }

    void ReflectionReader::ReadProperties(uintptr_t first, TypeIR &type, ReflectionStats &stats) const
    {
        PropertyChain chain = ReadPropertyChain(first, type.address, stats, type.layoutConflicts);
        type.properties = std::move(chain.properties);
        if (!chain.complete ||
            std::any_of(type.properties.begin(), type.properties.end(), [](const PropertyIR &property)
                        { return property.status != ParseStatus::Complete; }))
            type.status = ParseStatus::Partial;
    }

    ReflectionReader::PropertyChain ReflectionReader::ReadPropertyChain(uintptr_t first, uintptr_t owner,
                                                                        ReflectionStats &stats,
                                                                        std::vector<std::string> &diagnostics) const
    {
        PropertyChain result;
        const FieldChainResult chain = objects_.FieldsWithStatus(first, 65536);
        if (!chain.Complete())
        {
            ++stats.failures;
            result.complete = false;
            diagnostics.push_back("property field chain status=" + std::to_string(static_cast<int>(chain.status)) +
                                  " owner=" + std::to_string(owner) + " root=" + std::to_string(first));
        }
        for (const FieldMetadata &field : chain.fields)
        {
            if (memory_.LimitExceeded())
            {
                result.complete = false;
                ++stats.failures;
                diagnostics.push_back("capture observation limit reached in property chain");
                break;
            }
            if (!IsPropertyFieldKind(field.kind))
            {
                continue;
            }
            if (schema_.features.useFProperty &&
                (!field.ownerIsUObject || field.ownerAddress != owner))
            {
                ++stats.failures;
                result.complete = false;
                diagnostics.push_back("property owner mismatch: property=" + field.name +
                                      " address=" + std::to_string(field.address) +
                                      " expected_owner=" + std::to_string(owner) +
                                      " actual_owner=" + std::to_string(field.ownerAddress));
                continue;
            }
            const auto property = ReadProperty(field.address, stats);
            if (property)
                result.properties.push_back(*property);
            else
            {
                ++stats.failures;
                result.complete = false;
                diagnostics.push_back("property header unreadable: property=" + field.name +
                                      " class=" + field.className + " address=" + std::to_string(field.address));
            }
        }
        return result;
    }

    void ReflectionReader::ReadFunctionParameters(uintptr_t first, FunctionIR &function, ReflectionStats &stats) const
    {
        std::vector<PropertyMetadata> properties;
        const PropertyChain chain = ReadPropertyChain(first, function.address, stats, function.layoutConflicts);
        const std::vector<PropertyIR> &parsedProperties = chain.properties;
        for (const PropertyIR &property : parsedProperties)
        {
            PropertyMetadata metadata;
            metadata.arrayDim = property.arrayDim;
            metadata.elementSize = property.elementSize;
            metadata.offset = property.offset;
            metadata.flags = property.flags;
            properties.push_back(std::move(metadata));
        }

        const FunctionParameterSummary summary = AnalyzeFunctionParameters(
            properties, function.flags, schema_.features.functionDefaultsContinueAfterInitializer);
        function.parameters.clear();
        function.locals.clear();
        const size_t scannedPropertyCount = std::min(summary.scannedPropertyCount, parsedProperties.size());
        for (size_t index = 0; index < scannedPropertyCount; ++index)
        {
            if ((parsedProperties[index].flags & ::anduefker::ue::kCPFParm) != 0)
                function.parameters.push_back(parsedProperties[index]);
        }
        for (const PropertyIR &property : parsedProperties)
            if ((property.flags & ::anduefker::ue::kCPFParm) == 0)
                function.locals.push_back(property);
        function.derivedNumParams = summary.count;
        function.derivedParamSize = summary.paramEnd;
        function.defaultInitializerCount = summary.defaultInitializerCount;
        function.parameterSemanticsValid = summary.valid && chain.complete;
        const uint16_t expectedReturnOffset = summary.returnOffset >= 0
                                                  ? static_cast<uint16_t>(summary.returnOffset)
                                                  : std::numeric_limits<uint16_t>::max();
        function.parameterSemanticsConsistent = function.parameterSemanticsValid &&
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
        if (!function.layoutConflicts.empty() ||
            std::any_of(function.parameters.begin(), function.parameters.end(), [](const PropertyIR &property)
                        { return property.status != ParseStatus::Complete; }))
            function.status = ParseStatus::Partial;
    }

    void ReflectionReader::ReadFunctions(uintptr_t first, TypeIR &type, ReflectionStats &stats) const
    {
        const FieldChainResult chain = objects_.UFieldsWithStatus(first, 65536);
        if (!chain.Complete())
        {
            ++stats.failures;
            type.status = ParseStatus::Partial;
            type.layoutConflicts.push_back("function field chain status=" + std::to_string(static_cast<int>(chain.status)) +
                                           " root=" + std::to_string(first));
        }
        for (const FieldMetadata &field : chain.fields)
        {
            if (IsFunctionFieldKind(field.kind))
            {
                if (memory_.LimitExceeded())
                {
                    ++stats.failures;
                    type.status = ParseStatus::Partial;
                    type.layoutConflicts.push_back("capture observation limit reached in function chain");
                    break;
                }
                const auto flags = objects_.Flags(field.address);
                if (!flags || (*flags & (::anduefker::ue::kRFUnavailableDefinition | ::anduefker::ue::kRFClassDefaultObject)) != 0)
                {
                    ++stats.failures;
                    type.layoutConflicts.push_back("function definition is unavailable: function=" + field.name +
                                                   " address=" + std::to_string(field.address) +
                                                   " flags=" + (flags ? std::to_string(*flags) : "unreadable"));
                    continue;
                }
                FunctionIR function;
                function.address = field.address;
                function.name = field.name;
                const auto fullName = objects_.FullName(field.address);
                function.fullName = fullName ? *fullName : field.className + " " + field.name;
                if (!fullName)
                {
                    ++stats.failures;
                    function.status = ParseStatus::Partial;
                    function.layoutConflicts.push_back("function full name could not be resolved");
                }
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
                    type.layoutConflicts.push_back("function header unreadable: function=" + field.name +
                                                   " address=" + std::to_string(field.address));
                    continue;
                }
                function.numParams = function.headerNumParams;
                function.paramSize = function.headerParamSize;
                uintptr_t native = 0;
                if (!readMember(schema_.ufunction.nativeFunction, native))
                {
                    ++stats.failures;
                    function.status = ParseStatus::Partial;
                    function.layoutConflicts.push_back("function native pointer unreadable: address=" +
                                                       std::to_string(field.address) +
                                                       " offset=" + std::to_string(schema_.ufunction.nativeFunction));
                }
                else if (native >= moduleBase_ && native < moduleEnd_ && memory_.IsExecutable(native, sizeof(uintptr_t)))
                    function.nativeRva = native - moduleBase_;
                else if ((function.flags & ::anduefker::ue::kFUNCNative) != 0 &&
                         (native == 0 || !memory_.IsExecutable(native, sizeof(uintptr_t))))
                {
                    ++stats.failures;
                    function.status = ParseStatus::Partial;
                    function.layoutConflicts.push_back("native function pointer is invalid: pointer=" + std::to_string(native) +
                                                       " flags=" + std::to_string(function.flags));
                }
                function.nativeAddress = native;
                const auto parameters = objects_.StructProperties(field.address);
                if (parameters)
                    ReadFunctionParameters(*parameters, function, stats);
                else
                {
                    ++stats.failures;
                    function.status = ParseStatus::Partial;
                    function.layoutConflicts.push_back("function parameter chain root unreadable: address=" +
                                                       std::to_string(field.address) +
                                                       " offset=" + std::to_string(schema_.features.useFProperty ? schema_.ustruct.childProperties : schema_.ustruct.children));
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
        {
            ir.diagnostics.push_back("type identity unreadable: address=" + std::to_string(object) +
                                     " name_readable=" + std::to_string(name.has_value()) +
                                     " class_readable=" + std::to_string(className.has_value()));
            return std::nullopt;
        }
        const auto size = objects_.StructSize(object);
        const auto super = objects_.StructSuper(object);
        if (!size || *size < 0 || !super)
        {
            ir.diagnostics.push_back("type header invalid or unreadable: name=" + *name +
                                     " address=" + std::to_string(object) +
                                     " size_offset=" + std::to_string(schema_.ustruct.propertiesSizeOffset) +
                                     " size=" + (size ? std::to_string(*size) : "unreadable") +
                                     " super_offset=" + std::to_string(schema_.ustruct.superStruct) +
                                     " super_readable=" + std::to_string(super.has_value()));
            return std::nullopt;
        }

        TypeIR type;
        type.address = object;
        type.superAddress = *super;
        type.kind = kind;
        type.name = *name;
        const auto fullName = objects_.FullName(object);
        type.fullName = fullName ? *fullName : (*className + " " + *name);
        if (!fullName)
        {
            ++ir.stats.failures;
            type.status = ParseStatus::Partial;
            type.layoutConflicts.push_back("type full name could not be resolved");
        }
        type.size = *size;
        const auto properties = objects_.StructProperties(object);
        if (properties)
            ReadProperties(*properties, type, ir.stats);
        else
        {
            ++ir.stats.failures;
            type.status = ParseStatus::Partial;
            type.layoutConflicts.push_back("property chain root unreadable");
        }
        const auto children = objects_.StructChildren(object);
        if (children)
            ReadFunctions(*children, type, ir.stats);
        else
        {
            ++ir.stats.failures;
            type.status = ParseStatus::Partial;
            type.layoutConflicts.push_back("function chain root unreadable");
        }
        if (!type.layoutConflicts.empty() || std::any_of(type.functions.begin(), type.functions.end(), [](const FunctionIR &function)
                                                         { return function.status != ParseStatus::Complete; }))
            type.status = ParseStatus::Partial;
        return type;
    }

    EnumIR ReflectionReader::ReadEnum(uintptr_t object, ReflectionStats &stats) const
    {
        EnumIR result;
        result.address = object;
        const auto name = objects_.Name(object);
        const auto fullName = objects_.FullName(object);
        result.name = name ? *name : "<unreadable>";
        result.fullName = fullName ? *fullName : "Enum " + result.name;
        const auto failure = [&](const std::string &message)
        {
            result.status = ParseStatus::Partial;
            result.diagnostics.push_back(message);
            ++stats.failures;
            ++stats.enumReadFailures;
        };
        if (!name || !fullName)
            failure("enum identity could not be resolved");
        const auto readTail = [&](int32_t offset, bool byte, uint8_t &out)
        {
            const auto address = Add(object, offset);
            if (!address)
                return false;
            if (byte)
                return memory_.Read(*address, out);
            uint32_t value = 0;
            if (!memory_.Read(*address, value) || value > UINT8_MAX)
                return false;
            out = static_cast<uint8_t>(value);
            return true;
        };
        if (schema_.uenum.cppForm < 0 || !readTail(schema_.uenum.cppForm, schema_.features.enumCppFormIsByte, result.cppForm) || result.cppForm > 2)
            failure("enum CppForm is unreadable or invalid");
        if (schema_.features.enumHasFlags &&
            (schema_.uenum.flags < 0 || !readTail(schema_.uenum.flags, schema_.features.enumFlagsIsByte, result.flags)))
            failure("enum flags are unreadable or invalid");
        const auto values = objects_.ReadEnumValues(object);
        result.expectedValues = values.expectedCount;
        for (const EnumValueMetadata &value : values.values)
            result.values.push_back({value.name, value.value});
        if (!values.Complete())
            failure("enum values incomplete: status=" + std::to_string(static_cast<int>(values.status)) +
                    " expected=" + std::to_string(values.expectedCount) + " read=" + std::to_string(values.values.size()));
        return result;
    }

    ReflectionIR ReflectionReader::Read()
    {
        ReflectionIR result;
        std::vector<::anduefker::ir::CaptureInfo::Attempt> history;
        for (uint32_t attempt = 1; attempt <= 2; ++attempt)
        {
            const auto started = std::chrono::steady_clock::now();
            if (progress_)
                progress_("capture attempt=" + std::to_string(attempt) + " started");
            if (attempt > 1 && !memory_.RefreshAddressSpace())
            {
                result.status = result.types.empty() ? ParseStatus::Failed : ParseStatus::Partial;
                result.capture.observationsStable = false;
                result.diagnostics.push_back("address-space refresh failed before capture retry");
                break;
            }
            memory_.Reset();
            ::anduefker::memory::CaptureValidation validation;
            ::anduefker::ir::CaptureInfo::Attempt details;
            result = ReadAttempt(validation, details);
            memory_.CopyReadFailures(validation);
            details.number = attempt;
            details.elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          std::chrono::steady_clock::now() - started)
                                                          .count());
            details.observedRanges = validation.observedRanges;
            details.changedRanges = validation.changedRanges;
            details.unreadableRanges = validation.unreadableRanges;
            details.limitExceeded = validation.limitExceeded;
            details.generationChanged = validation.generationChanged;
            details.readFailures = validation.readFailures;
            details.failures = result.stats.failures;
            details.identityFailures = result.stats.identityFailures;
            const size_t diagnosticSamples = std::min<size_t>(result.diagnostics.size(), 32);
            details.diagnostics.assign(result.diagnostics.begin(), result.diagnostics.begin() + diagnosticSamples);
            for (const auto &failure : validation.readFailureSamples)
                details.readFailureSamples.push_back({failure.address, static_cast<int32_t>(failure.error),
                                                      failure.requested, failure.transferred});
            for (const auto &change : validation.changes)
                details.changes.push_back({change.address, change.size, static_cast<int32_t>(change.read.error),
                                           change.read.transferred, change.before, change.after});
            if (details.reason.empty())
                details.reason = validation.Stable() ? "observed-bytes-stable" : "observed-bytes-changed-or-unreadable";
            history.push_back(std::move(details));
            if (progress_)
                progress_("capture attempt=" + std::to_string(attempt) + " elapsed_ms=" +
                          std::to_string(history.back().elapsedMs) + " reason=" + history.back().reason);
            result.capture.observationsStable = validation.Stable() && validation.observedRanges != 0 && history.back().coverageComplete;
            result.capture.limitExceeded = validation.limitExceeded;
            result.capture.generationChanged = validation.generationChanged;
            result.capture.observedRanges = validation.observedRanges;
            result.capture.observedBytes = validation.observedBytes;
            result.capture.changedRanges = validation.changedRanges;
            result.capture.unreadableRanges = validation.unreadableRanges;
            result.capture.attempts = attempt;
            result.capture.readFailures = validation.readFailures;
            for (const auto &failure : validation.readFailureSamples)
                result.capture.readFailureSamples.push_back({failure.address, static_cast<int32_t>(failure.error),
                                                             failure.requested, failure.transferred});
            if (!result.capture.observationsStable)
            {
                result.status = result.types.empty() ? ParseStatus::Failed : ParseStatus::Partial;
                result.diagnostics.push_back("capture validation: changed=" + std::to_string(validation.changedRanges) +
                                             " unreadable=" + std::to_string(validation.unreadableRanges) +
                                             " limit_exceeded=" + std::to_string(validation.limitExceeded) +
                                             " generation_changed=" + std::to_string(validation.generationChanged));
                for (uintptr_t address : validation.failedAddresses)
                    result.diagnostics.push_back("capture validation failed at address=" + std::to_string(address));
                if (!validation.limitExceeded && attempt == 1)
                    continue;
            }
            break;
        }
        result.capture.history = std::move(history);
        result.diagnostics.push_back("capture attempts=" + std::to_string(result.capture.attempts) +
                                     "; consistency covers observed bytes, not an atomic process snapshot");
        for (const auto &failure : result.capture.readFailureSamples)
            result.diagnostics.push_back("capture read failure: address=" + std::to_string(failure.address) +
                                         " requested=" + std::to_string(failure.requested) +
                                         " transferred=" + std::to_string(failure.transferred) +
                                         " read_error=" + std::to_string(failure.error));
        if (result.capture.readFailures > result.capture.readFailureSamples.size())
            result.diagnostics.push_back("capture read failure samples omitted=" +
                                         std::to_string(result.capture.readFailures - result.capture.readFailureSamples.size()));
        return result;
    }

    ReflectionIR ReflectionReader::ReadAttempt(::anduefker::memory::CaptureValidation &validation,
                                               ::anduefker::ir::CaptureInfo::Attempt &details)
    {
        ReflectionIR result;
        ::anduefker::memory::CaptureObservationScope observe(memory_);
        if (!objects_.Initialize())
        {
            result.status = ParseStatus::Failed;
            ++result.stats.failures;
            result.diagnostics.push_back("reflection object store initialization failed");
            details.reason = "object-store-initialization-failed";
            return result;
        }
        result.stats.objectSlots = objects_.Count();
        details.initialCount = objects_.Count();
        constexpr int32_t maxObjectSlots = 2 * 1024 * 1024;
        constexpr int32_t maxAdditionalSlots = 65536;
        constexpr uint32_t maxTailRounds = 3;
        if (objects_.Count() > maxObjectSlots)
        {
            result.stats.unvisitedObjects = objects_.Count();
            ++result.stats.failures;
            details.reason = "initial-object-slot-budget-exceeded";
            return result;
        }

        constexpr int32_t maxObjectSamplesPerReason = 8;
        const auto recordObjectDiagnostic = [&](int32_t count, const auto &message)
        {
            if (count <= maxObjectSamplesPerReason)
                result.diagnostics.push_back(message());
            else
                ++result.stats.objectDiagnosticSamplesOmitted;
        };
        const auto objectIdentity = [&](int32_t index, uintptr_t object, const std::string &className,
                                        uint32_t flags)
        {
            const auto name = objects_.Name(object);
            return "index=" + std::to_string(index) + " address=" + std::to_string(object) +
                   " class=" + className + " name=" + (name ? *name : "<unreadable>") +
                   " flags=" + std::to_string(flags);
        };

        std::unordered_set<uintptr_t> seenTypes;
        std::unordered_map<uintptr_t, std::optional<::anduefker::ue::DefinitionKind>> classKinds;
        std::unordered_map<std::string, size_t> failureReasons;
        auto lastProgress = std::chrono::steady_clock::now();
        const auto recordFailure = [&](const std::string &reason, int32_t index, uintptr_t address)
        {
            const size_t count = ++failureReasons[reason];
            if (count <= 8)
                result.diagnostics.push_back("reflection object failure: reason=" + reason +
                                             " index=" + std::to_string(index) + " address=" + std::to_string(address));
        };
        for (int32_t index = 0;; ++index)
        {
            if (index == objects_.Count())
            {
                details.enumeratedCount = index;
                if (progress_)
                    progress_("capture: validating observed bytes; enumerated=" + std::to_string(index));
                validation = memory_.Validate();
                const auto boundary = objects_.RefreshObjectCount();
                details.countAddress = boundary.countAddress;
                details.finalCount = boundary.count;
                if (!boundary.valid)
                {
                    details.reason = boundary.reason;
                    ++result.stats.failures;
                    break;
                }
                if (!validation.Stable())
                {
                    details.reason = "observed-bytes-changed-or-unreadable";
                    break;
                }
                if (boundary.count == index)
                {
                    details.coverageComplete = true;
                    break;
                }
                result.stats.objectSlots = boundary.count;
                if (details.tailRounds >= maxTailRounds || boundary.count > maxObjectSlots ||
                    boundary.count - details.initialCount > maxAdditionalSlots)
                {
                    result.stats.unvisitedObjects = boundary.count - index;
                    ++result.stats.failures;
                    details.reason = "object-tail-budget-exceeded";
                    break;
                }
                ++details.tailRounds;
                if (progress_)
                    progress_("capture: count grew from=" + std::to_string(index) + " to=" + std::to_string(boundary.count) +
                              "; reading tail round=" + std::to_string(details.tailRounds));
            }
            if (progress_ && index % 256 == 0 && std::chrono::steady_clock::now() - lastProgress >= std::chrono::seconds(1))
            {
                progress_("reflection visited=" + std::to_string(index) + "/" + std::to_string(objects_.Count()) +
                          " types=" + std::to_string(result.stats.parsedTypes));
                lastProgress = std::chrono::steady_clock::now();
            }
            if (memory_.LimitExceeded())
            {
                result.stats.unvisitedObjects = objects_.Count() - index;
                ++result.stats.failures;
                result.diagnostics.push_back("capture observation limit reached; remaining objects were not visited");
                details.reason = "observation-budget-exceeded";
                validation = memory_.Validate();
                break;
            }
            const ObjectReadResult objectResult = objects_.Objects().ReadObject(index);
            if (!objectResult.IsValid())
            {
                ++result.stats.skippedObjects;
                if (objectResult.status == ObjectReadStatus::Empty)
                {
                    ++result.stats.emptyObjectSlots;
                    recordObjectDiagnostic(result.stats.emptyObjectSlots, [&]
                                           { return "empty object slot: index=" + std::to_string(index) +
                                                    " read_address=" + std::to_string(objectResult.readAddress); });
                }
                else
                {
                    ++result.stats.objectReadFailures;
                    ++result.stats.failures;
                    recordObjectDiagnostic(result.stats.objectReadFailures, [&]
                                           { return "object slot could not be read: index=" + std::to_string(index) +
                                                    " reason=" + ::anduefker::ue::ObjectReadStatusName(objectResult.status) +
                                                    " read_address=" + std::to_string(objectResult.readAddress) +
                                                    " pointer=" + std::to_string(objectResult.address) +
                                                    " read_error=" + std::to_string(static_cast<int>(objectResult.readError)); });
                }
                continue;
            }
            const uintptr_t object = objectResult.address;
            ++result.stats.validObjects;
            const auto className = objects_.ClassName(object);
            if (!className)
            {
                ++result.stats.classNameReadFailures;
                ++result.stats.failures;
                recordObjectDiagnostic(result.stats.classNameReadFailures, [&]
                                       { return "object class name could not be read: index=" + std::to_string(index) +
                                                " address=" + std::to_string(object); });
                continue;
            }
            const auto classAddress = objects_.Class(object);
            if (!classAddress)
            {
                ++result.stats.failures;
                recordFailure("class-pointer-unreadable", index, object);
                continue;
            }
            auto classification = classKinds.find(*classAddress);
            if (classification == classKinds.end())
            {
                ::anduefker::memory::CaptureObservationScope observe(memory_);
                classification = classKinds.emplace(*classAddress, objects_.DefinitionKindForClass(*classAddress)).first;
            }
            if (!classification->second)
            {
                ++result.stats.failures;
                ++result.stats.classNameReadFailures;
                recordFailure("class-hierarchy-unresolved", index, object);
                continue;
            }
            const auto kind = *classification->second;
            if (index >= details.initialCount)
            {
                const auto tailIndex = objects_.InternalIndex(object);
                const auto tailFlags = objects_.Flags(object);
                const auto internalFlags = objects_.Objects().ReadInternalFlags(objectResult);
                if (!tailIndex || *tailIndex != index || !tailFlags ||
                    !internalFlags || (*internalFlags & 0x80000000u) != 0 || // PendingConstruction in supported layouts.
                    (*tailFlags & ::anduefker::ue::kRFUnavailableDefinition) != 0 ||
                    objects_.InternalIndex(object, true) != tailIndex || objects_.Class(object, true) != classAddress ||
                    objects_.Objects().ReadObject(index, true).address != object)
                {
                    ++result.stats.failures;
                    ++result.stats.skippedIncompleteObjects;
                    recordFailure("new-object-not-ready-or-identity-changed", index, object);
                    memory_.Invalidate();
                    continue;
                }
            }
            if (kind == ::anduefker::ue::DefinitionKind::Other)
                continue;
            ::anduefker::memory::CaptureObservationScope observe(memory_);
            const auto verifiedObject = objects_.Objects().ReadObject(index, true);
            const auto observedIndex = objects_.InternalIndex(object);
            const auto internalIndex = objects_.InternalIndex(object, true);
            const auto verifiedClass = objects_.Class(object, true);
            if (!verifiedObject.IsValid() || verifiedObject.address != object || !internalIndex || *internalIndex != index ||
                observedIndex != internalIndex ||
                !verifiedClass || *verifiedClass != *classAddress)
            {
                ++result.stats.failures;
                ++result.stats.identityFailures;
                memory_.Invalidate();
                recordObjectDiagnostic(result.stats.identityFailures, [&]
                                       { return "reflection object identity mismatch: index=" + std::to_string(index) +
                                                " address=" + std::to_string(object); });
                continue;
            }
            const auto objectFlags = objects_.Flags(object);
            if (!objectFlags)
            {
                ++result.stats.failures;
                recordFailure("object-flags-unreadable", index, object);
                continue;
            }
            if ((*objectFlags & ::anduefker::ue::kRFClassDefaultObject) != 0)
            {
                ++result.stats.skippedClassDefaultObjects;
                ++result.stats.skippedObjects;
                recordObjectDiagnostic(result.stats.skippedClassDefaultObjects, [&]
                                       { return "skipped reflection class default object: " +
                                                objectIdentity(index, object, *className, *objectFlags); });
                continue;
            }
            if ((*objectFlags & ::anduefker::ue::kRFUnavailableDefinition) != 0)
            {
                ++result.stats.skippedIncompleteObjects;
                ++result.stats.skippedObjects;
                ++result.stats.failures;
                recordObjectDiagnostic(result.stats.skippedIncompleteObjects, [&]
                                       { return "reflection definition is not ready or is being destroyed: " +
                                                objectIdentity(index, object, *className, *objectFlags); });
                continue;
            }
            if (kind == ::anduefker::ue::DefinitionKind::Enum)
            {
                result.enums.push_back(ReadEnum(object, result.stats));
                ++result.stats.parsedEnums;
                if (index >= details.initialCount)
                    ++details.additionalEnums;
                continue;
            }
            if (!seenTypes.insert(object).second)
                continue;

            const auto type = ReadType(object, kind == ::anduefker::ue::DefinitionKind::Class ? TypeKind::Class : TypeKind::Struct, result);
            if (!type)
            {
                ++result.stats.failures;
                recordFailure("type-header-unavailable", index, object);
                continue;
            }
            result.types.push_back(*type);
            ++result.stats.parsedTypes;
            if (index >= details.initialCount)
                ++details.additionalTypes;
        }
        if (!details.coverageComplete && details.reason.empty())
            details.reason = "enumeration-coverage-incomplete";
        result.diagnostics.push_back("object enumeration: initial_count=" + std::to_string(details.initialCount) +
                                     " enumerated_count=" + std::to_string(details.enumeratedCount) +
                                     " final_count=" + std::to_string(details.finalCount) +
                                     " tail_rounds=" + std::to_string(details.tailRounds) + " reason=" + details.reason);

        if (result.stats.objectDiagnosticSamplesOmitted != 0)
            result.diagnostics.push_back("object diagnostic samples omitted=" +
                                         std::to_string(result.stats.objectDiagnosticSamplesOmitted) +
                                         "; sample limit per reason=" + std::to_string(maxObjectSamplesPerReason));
        for (const auto &[reason, count] : failureReasons)
            result.diagnostics.push_back("reflection object failure summary: reason=" + reason +
                                         " total=" + std::to_string(count) +
                                         " samples_omitted=" + std::to_string(count > 8 ? count - 8 : 0));

        std::unordered_map<uintptr_t, size_t> enumByAddress;
        for (size_t index = 0; index < result.enums.size(); ++index)
            enumByAddress.emplace(result.enums[index].address, index);
        const auto inferEnumType = [&](const auto &self, const TypeReferenceIR &reference, size_t depth) -> void
        {
            if (depth >= 32)
                return;
            if (reference.kind == PropertyKind::Enum && reference.inner && reference.inner->detailsResolved)
            {
                const auto index = enumByAddress.find(reference.secondaryObject);
                if (index != enumByAddress.end())
                {
                    EnumUnderlyingType underlying = EnumUnderlyingType::Unknown;
                    switch (reference.inner->kind)
                    {
                    case PropertyKind::Int8:
                        underlying = EnumUnderlyingType::Int8;
                        break;
                    case PropertyKind::Byte:
                        underlying = EnumUnderlyingType::UInt8;
                        break;
                    case PropertyKind::Int16:
                        underlying = EnumUnderlyingType::Int16;
                        break;
                    case PropertyKind::UInt16:
                        underlying = EnumUnderlyingType::UInt16;
                        break;
                    case PropertyKind::Int32:
                        underlying = EnumUnderlyingType::Int32;
                        break;
                    case PropertyKind::UInt32:
                        underlying = EnumUnderlyingType::UInt32;
                        break;
                    case PropertyKind::Int64:
                        underlying = EnumUnderlyingType::Int64;
                        break;
                    case PropertyKind::UInt64:
                        underlying = EnumUnderlyingType::UInt64;
                        break;
                    default:
                        break;
                    }
                    EnumIR &enumeration = result.enums[index->second];
                    if (enumeration.underlyingType == EnumUnderlyingType::Unknown)
                        enumeration.underlyingType = underlying;
                    else if (enumeration.underlyingType != underlying)
                    {
                        enumeration.status = ParseStatus::Partial;
                        enumeration.diagnostics.push_back("conflicting enum underlying property types");
                        ++result.stats.layoutConflicts;
                    }
                }
            }
            if (reference.inner)
                self(self, *reference.inner, depth + 1);
            if (reference.key)
                self(self, *reference.key, depth + 1);
            if (reference.value)
                self(self, *reference.value, depth + 1);
        };
        for (const TypeIR &type : result.types)
        {
            for (const PropertyIR &property : type.properties)
                inferEnumType(inferEnumType, property.type, 0);
            for (const FunctionIR &function : type.functions)
            {
                for (const PropertyIR &parameter : function.parameters)
                    inferEnumType(inferEnumType, parameter.type, 0);
                for (const PropertyIR &local : function.locals)
                    inferEnumType(inferEnumType, local.type, 0);
            }
        }

        std::unordered_map<std::string, size_t> detailCounts;
        const auto noteDetails = [&](const PropertyIR &property, const std::string &owner, uintptr_t ownerAddress)
        {
            for (const auto &detail : property.detailDiagnostics)
            {
                if (++detailCounts[detail.reason + ":" + detail.normalizedClass] <= 8)
                    result.diagnostics.push_back("property detail failure: owner=" + owner +
                                                 " owner_address=" + std::to_string(ownerAddress) +
                                                 " root=" + property.name + " property=" + detail.name +
                                                 " property_class=" + detail.reflectedClass +
                                                 " address=" + std::to_string(detail.address) +
                                                 " reason=" + detail.reason +
                                                 " referenced_address=" + std::to_string(detail.referencedAddress) +
                                                 " secondary_address=" + std::to_string(detail.secondaryAddress));
            }
        };
        for (const TypeIR &type : result.types)
        {
            for (const auto &property : type.properties)
                noteDetails(property, type.fullName, type.address);
            for (const auto &function : type.functions)
            {
                for (const auto &property : function.parameters)
                    noteDetails(property, function.fullName, function.address);
                for (const auto &property : function.locals)
                    noteDetails(property, function.fullName, function.address);
            }
        }
        for (const auto &[reason, count] : detailCounts)
            result.diagnostics.push_back("property detail summary: reason_and_class=" + reason + " total=" + std::to_string(count) +
                                         " samples_omitted=" + std::to_string(count > 8 ? count - 8 : 0));

        ::anduefker::ir::AnalyzeReflectionLayouts(result);
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
